/* ----------------------------------------------------------------
 *
 * libnvme-strom.c
 *
 * Collection of routines to use 'nvme-strom' kernel module
 * --------
 * Copyright 2016 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2016 (C) The PG-Strom Development Team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2,
 * as published by the Free Software Foundation.
 * ----------------------------------------------------------------
 */
#include <fcntl.h>
#include <libgen.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <cuda.h>
#include "nvme_strom.h"

#define offsetof(type, field)   ((long) &((type *)0)->field)

/* command line options */
static int		device_index = 0;
static int		num_chunks = 6;
static size_t	chunk_size = 32UL << 20;
static int		enable_checks = 0;
static int		print_mapping = 0;
static int		test_by_vfs = 0;

static sem_t	buffer_sem;
static pthread_mutex_t	buffer_lock;

/*
 * nvme_strom_ioctl - entrypoint of NVME-Strom
 */
static int
nvme_strom_ioctl(int cmd, const void *arg)
{
	static __thread int fdesc_nvme_strom = -1;

	if (fdesc_nvme_strom < 0)
	{
		fdesc_nvme_strom = open(NVME_STROM_IOCTL_PATHNAME, O_RDONLY);
		if (fdesc_nvme_strom < 0)
		{
			fprintf(stderr, "failed to open \"%s\" : %m\n",
					NVME_STROM_IOCTL_PATHNAME);
			return -1;
		}
	}
	return ioctl(fdesc_nvme_strom, cmd, arg);
}

static void
cuda_exit_on_error(CUresult rc, const char *apiname)
{
	if (rc != CUDA_SUCCESS)
	{
		const char *error_name;

		if (cuGetErrorName(rc, &error_name) != CUDA_SUCCESS)
			error_name = "unknown error";

		fprintf(stderr, "failed on %s: %s\n", apiname, error_name);
		exit(1);
	}
}

static void
system_exit_on_error(int rc, const char *apiname)
{
	if (rc)
	{
		fprintf(stderr, "failed on %s: %m\n", apiname);
		exit(1);
	}
}

static void
ioctl_check_file(const char *filename, int fdesc)
{
	StromCmd__CheckFile uarg;
	int		rc;

	memset(&uarg, 0, sizeof(uarg));
	uarg.fdesc = fdesc;

	rc = nvme_strom_ioctl(STROM_IOCTL__CHECK_FILE, &uarg);
	if (rc)
	{
		fprintf(stderr, "STROM_IOCTL__CHECK_FILE('%s') --> %d: %m\n",
				filename, rc);
		exit(1);
	}
}

static unsigned long
ioctl_map_gpu_memory(CUdeviceptr cuda_devptr, size_t buffer_size)
{
	StromCmd__MapGpuMemory uarg;
	int			retval;

	memset(&uarg, 0, sizeof(StromCmd__MapGpuMemory));
	uarg.vaddress = cuda_devptr;
	uarg.length = buffer_size;

	retval = nvme_strom_ioctl(STROM_IOCTL__MAP_GPU_MEMORY, &uarg);
	if (retval)
	{
		fprintf(stderr, "STROM_IOCTL__MAP_GPU_MEMORY(%p, %lu) --> %d: %m",
			   (void *)cuda_devptr, buffer_size, retval);
		exit(1);
	}
	return uarg.handle;
}

static void
ioctl_info_gpu_memory(unsigned long handle, unsigned int num_pages)
{
	StromCmd__InfoGpuMemory *uarg;
	size_t	required;
	int		i, retval;

	required = offsetof(StromCmd__InfoGpuMemory, pages[num_pages]);
	uarg = malloc(required);
	if (!uarg)
	{
		fprintf(stderr, "out of memory: %m\n");
		exit(1);
	}
	memset(uarg, 0, required);
	uarg->handle = handle;
	uarg->nrooms = num_pages;

	retval = nvme_strom_ioctl(STROM_IOCTL__INFO_GPU_MEMORY, uarg);
	if (retval)
	{
		fprintf(stderr,
				"STROM_IOCTL__INFO_GPU_MEMORY(handle=%lx) --> %d: %m\n",
				handle, retval);
		exit(1);
	}

	printf("Handle=%lx version=%u gpu_page_sz=%u\n",
		   handle, uarg->version, uarg->gpu_page_sz);
	for (i=0; i < uarg->nitems; i++)
	{
		printf("V:%016lx <--> P:%016lx\n",
			   (unsigned long)uarg->pages[i].vaddr,
			   (unsigned long)uarg->pages[i].paddr);
	}
	free(uarg);
}

typedef struct
{
	int				fdesc;
	loff_t			fpos;
	int				index;
	int				is_running;
	CUstream		cuda_stream;
	unsigned long	dma_task_id;
	long			status;
	void		   *src_buffer;
	void		   *dest_buffer;
} async_task;

static void
callback_dma_wait(CUstream cuda_stream, CUresult status, void *private)
{
	StromCmd__MemCpySsdToGpuWait	uarg;
	async_task	   *atask = private;
	int				rv;

	cuda_exit_on_error(status, "async_task");

	uarg.ntasks = 1;
	uarg.nwaits = 1;
	uarg.dma_task_id[0] = atask->dma_task_id;
	rv = nvme_strom_ioctl(STROM_IOCTL__MEMCPY_SSD2GPU_WAIT, &uarg);
	system_exit_on_error(rv, "STROM_IOCTL__MEMCPY_SSD2GPU_WAIT");
	system_exit_on_error(atask->status, "Async SSD-to-GPU DMA");
}

static void
callback_release_atask(CUstream cuda_stream, CUresult status, void *private)
{
	async_task	   *atask = private;
	ssize_t			retval;

	/* Do we take a sanity check? */
	if (enable_checks)
	{
		if (!test_by_vfs)
		{
			retval = lseek(atask->fdesc, atask->fpos, SEEK_SET);
			system_exit_on_error(retval, "lseek");
			retval = read(atask->fdesc, atask->src_buffer, chunk_size);
			system_exit_on_error(retval != chunk_size, "read");
		}
		retval = memcmp(atask->src_buffer, atask->dest_buffer, chunk_size);
		system_exit_on_error(retval, "memcmp");
	}
	pthread_mutex_lock(&buffer_lock);
	atask->is_running = 0;
	pthread_mutex_unlock(&buffer_lock);

	sem_post(&buffer_sem);
}

static async_task *
setup_async_tasks(int fdesc)
{
	async_task	   *async_tasks;
	CUresult		rc;
	int				i, rv;

	async_tasks = malloc(sizeof(async_task) * num_chunks);
	system_exit_on_error(!async_tasks, "malloc");

	rv = sem_init(&buffer_sem, 0, num_chunks);
	system_exit_on_error(rv, "sem_init");
	rv = pthread_mutex_init(&buffer_lock, NULL);
	system_exit_on_error(rv, "pthread_mutex_init");

	for (i=0; i < num_chunks; i++)
	{
		async_tasks[i].fdesc = dup(fdesc);
		system_exit_on_error(async_tasks[i].fdesc < 0, "dup");
		async_tasks[i].fpos  = 0;
		async_tasks[i].index = i;
		async_tasks[i].is_running = 0;
		rc = cuStreamCreate(&async_tasks[i].cuda_stream,
							CU_STREAM_DEFAULT);
		cuda_exit_on_error(rc, "cuStreamCreate");
		async_tasks[i].status = 0;

		if (enable_checks || test_by_vfs)
		{
			rc = cuMemAllocHost(&async_tasks[i].src_buffer, chunk_size);
			cuda_exit_on_error(rc, "cuMemAllocHost");
		}
		else
			async_tasks[i].src_buffer = NULL;

		rc = cuMemAllocHost(&async_tasks[i].dest_buffer, chunk_size);
		cuda_exit_on_error(rc, "cuMemAllocHost");
	}
	return async_tasks;
}

static void
show_throughput(const char *filename, size_t file_size,
				struct timeval tv1, struct timeval tv2)
{
	long		time_ms;
	double		throughput;
	const char *unitsz;

	time_ms = ((tv2.tv_sec * 1000 + tv2.tv_usec / 1000) -
			   (tv1.tv_sec * 1000 + tv1.tv_usec / 1000));
	throughput = (double)file_size / (double)(time_ms / 1000);
	if (throughput < (double)(4UL << 10))
	{
		unitsz = "Bytes";
	}
	else if (throughput < (double)(4UL << 20))
	{
		throughput /= (double)(1UL << 10);
		unitsz = "KB";
	}
	else if (throughput < (double)(4UL << 30))
	{
		throughput /= (double)(1UL << 20);
		unitsz = "MB";
	}
	else
	{
		throughput /= (double)(1UL << 30);
        unitsz = "GB";
	}
	printf("file: %s, read: %luKB, time: %.3fms, band: %.2f%s/s\n",
		   filename, file_size, (double)time_ms, throughput, unitsz);
}

static void
exec_test_by_strom(CUdeviceptr cuda_devptr, unsigned long handle,
				   const char *filename, int fdesc, size_t file_size)
{
	StromCmd__MemCpySsdToGpuAsync uarg;
	async_task	   *async_tasks;
	CUresult		rc;
	int				i, j, rv;
	size_t			offset;
	struct timeval	tv1, tv2;

	async_tasks = setup_async_tasks(fdesc);
	gettimeofday(&tv1, NULL);
	for (offset=0; offset < file_size; offset += chunk_size)
	{
		async_task *atask = NULL;

		rv = sem_wait(&buffer_sem);
		system_exit_on_error(rv, "sem_wait");

		/* find out an available async_task */
		pthread_mutex_lock(&buffer_lock);
		for (j=0; j < num_chunks; j++)
		{
			atask = &async_tasks[i++ % num_chunks];
			if (!atask->is_running)
			{
				atask->is_running = 1;
				break;		/* found */
			}
		}
		if (j == num_chunks)
		{
			fprintf(stderr, "Bug? no free async_task but semaphore > 0\n");
			exit(1);
		}
		pthread_mutex_unlock(&buffer_lock);

		/* kick SSD-to-GPU DMA */
		memset(&uarg, 0, sizeof(uarg));
		atask->status         = 0;
		uarg.p_status         = &atask->status;
		uarg.handle           = handle;
		uarg.fdesc            = fdesc;
		uarg.nchunks          = 1;
		uarg.chunks[0].fpos   = offset;
		uarg.chunks[0].offset = atask->index * chunk_size;
		uarg.chunks[0].length = chunk_size;
		rv = nvme_strom_ioctl(STROM_IOCTL__MEMCPY_SSD2GPU_ASYNC, &uarg);
		system_exit_on_error(rv, "STROM_IOCTL__MEMCPY_SSD2GPU_ASYNC");
		atask->dma_task_id    = uarg.dma_task_id;

		/* kick callback for synchronization */
		rc = cuStreamAddCallback(atask->cuda_stream,
								 callback_dma_wait, atask, 0);
		cuda_exit_on_error(rc, "cuStreamAddCallback");

		/* kick GPU-to-RAM DMA */
		rc = cuMemcpyDtoHAsync(atask->dest_buffer,
							   cuda_devptr + atask->index * chunk_size,
							   chunk_size,
							   atask->cuda_stream);
		cuda_exit_on_error(rc, "cuMemcpyDtoHAsync");

		/* kick callback to release atask */
		rc = cuStreamAddCallback(atask->cuda_stream,
								 callback_release_atask, atask, 0);
		cuda_exit_on_error(rc, "cuStreamAddCallback");
	}
	/* wait for completion of the asyncronous tasks */
	do {
		rv = sem_wait(&buffer_sem);
		system_exit_on_error(rv, "sem_wait");

		pthread_mutex_lock(&buffer_lock);
		for (j=0; j < num_chunks; j++)
		{
			async_task *atask = &async_tasks[j];
			if (atask->is_running)
				break;	/* here is still running task */
		}
		pthread_mutex_unlock(&buffer_lock);
	} while (j < num_chunks);

	gettimeofday(&tv2, NULL);
	show_throughput(filename, file_size, tv1, tv2);
}

static void
exec_test_by_vfs(CUdeviceptr cuda_devptr, unsigned long handle,
				 const char *filename, int fdesc, size_t file_size)
{
	async_task	   *async_tasks;
	CUresult		rc;
	int				i, j, rv;
	size_t			offset;
	ssize_t			retval;
	struct timeval	tv1, tv2;

	async_tasks = setup_async_tasks(fdesc);
	gettimeofday(&tv1, NULL);
	for (offset=0; offset < file_size; offset += chunk_size)
	{
		async_task *atask = NULL;

		rv = sem_wait(&buffer_sem);
		system_exit_on_error(rv, "sem_wait");

		/* find out an available async_task */
		pthread_mutex_lock(&buffer_lock);
		for (j=0; j < num_chunks; j++)
		{
			atask = &async_tasks[i++ % num_chunks];
			if (!atask->is_running)
			{
				atask->is_running = 1;
				break;		/* found */
			}
		}
		if (j == num_chunks)
		{
			fprintf(stderr, "Bug? no free async_task but semaphore > 0\n");
			exit(1);
		}
		pthread_mutex_unlock(&buffer_lock);

		/* Load SSD-to-RAM */
		retval = read(fdesc, atask->src_buffer, chunk_size);
		system_exit_on_error(retval != chunk_size, "read");

		/* Kick RAM-to-GPU DMA */
		rc = cuMemcpyHtoDAsync(cuda_devptr + atask->index * chunk_size,
							   atask->src_buffer, chunk_size,
							   atask->cuda_stream);
		cuda_exit_on_error(rc, "cuMemcpyHtoDAsync");

		/* Kick GPU-to-RAM DMA */
		rc = cuMemcpyDtoHAsync(atask->dest_buffer,
							   cuda_devptr + atask->index * chunk_size,
							   chunk_size,
							   atask->cuda_stream);
		cuda_exit_on_error(rc, "cuMemcpyDtoHAsync");

		/* Kick callback to release atask */
		rc = cuStreamAddCallback(atask->cuda_stream,
								 callback_release_atask, atask, 0);
		cuda_exit_on_error(rc, "cuStreamAddCallback");
	}
	/* wait for completion of the asyncronous tasks */
	do {
		rv = sem_wait(&buffer_sem);
		system_exit_on_error(rv, "sem_wait");

		pthread_mutex_lock(&buffer_lock);
		for (j=0; j < num_chunks; j++)
		{
			async_task *atask = &async_tasks[j];
			if (atask->is_running)
				break;  /* here is still running task */
		}
		pthread_mutex_unlock(&buffer_lock);
	} while (j < num_chunks);

	gettimeofday(&tv2, NULL);
	show_throughput(filename, file_size, tv1, tv2);
}

/*
 *
 */
static void usage(const char *cmdname)
{
	fprintf(stderr,
			"usage: %s [OPTIONS] <filename>\n"
			"    -d <device index>:        (default 0)\n"
			"    -n <num of chunks>:       (default 6)\n"
			"    -s <size of chunk in MB>: (default 32MB)\n"
			"    -c : Enables corruption check (default off)\n"
			"    -h : Print this message (default off)\n"
			"    -f : Test by normal VFS access (default off)\n",
			basename(strdup(cmdname)));
	exit(1);
}

/*
 * entrypoint of driver_test
 */
int main(int argc, char * const argv[])
{
	const char	   *filename = NULL;
	int				fdesc = -1;
	struct stat		stbuf;
	size_t			filesize;
	size_t			buffer_size;
	CUresult		rc;
	CUdevice		cuda_device;
	CUcontext		cuda_context;
	CUdeviceptr		cuda_devptr;
	unsigned long	mgmem_handle;
	int				code;

	while ((code = getopt(argc, argv, "d:n:s:cpf")) != 0)
	{
		switch (code)
		{
			case 'd':
				device_index = atoi(optarg);
				break;
			case 'n':		/* number of chunks */
				num_chunks = atoi(optarg);
				break;
			case 's':		/* size of chunks in MB */
				chunk_size = (size_t)atoi(optarg) << 20;
				break;
			case 'c':
				enable_checks = 1;
				break;
			case 'p':
				print_mapping = 1;
				break;
			case 'f':
				test_by_vfs = 1;
				break;
			case 'h':
			default:
				usage(argv[0]);
		}
	}
	buffer_size = (size_t)chunk_size * num_chunks;

	if (optind + 1 == argc)
		filename = argv[optind];
	else
		usage(argv[0]);

	/* open the target file */
	fdesc = open(filename, O_RDONLY);
	if (fdesc < 0)
	{
		fprintf(stderr, "failed to open \"%s\": %m\n", filename);
		return 1;
	}

	if (fstat(fdesc, &stbuf) != 0)
	{
		fprintf(stderr, "failed on fstat(\"%s\"): %m\n", filename);
		return 1;
	}
	filesize = (stbuf.st_size & ~(stbuf.st_blksize - 1));

	/* is this file supported? */
	ioctl_check_file(filename, fdesc);

	/* allocate and map device memory */
	rc = cuInit(0);
	cuda_exit_on_error(rc, "cuInit");

	rc = cuDeviceGet(&cuda_device, device_index);
	cuda_exit_on_error(rc, "cuDeviceGet");

	rc = cuCtxCreate(&cuda_context, CU_CTX_SCHED_AUTO, cuda_device);
	cuda_exit_on_error(rc, "cuCtxCreate");

	rc = cuMemAlloc(&cuda_devptr, buffer_size);
	cuda_exit_on_error(rc, "cuMemAlloc");

	rc = cuMemsetD32(cuda_devptr, 0x41424344,
					 chunk_size * num_chunks / sizeof(int));
	cuda_exit_on_error(rc, "cuMemsetD32");

	mgmem_handle = ioctl_map_gpu_memory(cuda_devptr, buffer_size);

	/* print device memory map information */
	if (print_mapping)
		ioctl_info_gpu_memory(mgmem_handle, buffer_size / 4096);

	/* execute test by SSD-to-GPU or SSD-to-CPU-to-GPU */
	if (!test_by_vfs)
		exec_test_by_strom(cuda_devptr, mgmem_handle,
						   filename, fdesc, filesize);
	else
		exec_test_by_vfs(cuda_devptr, mgmem_handle,
						 filename, fdesc, filesize);
	return 0;
}
