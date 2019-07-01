//====================================================================================================================================
// OpenCL-based memory bandwidth benchmark for OpenCL-capable FPGAs
// (c) 2019, Hamid Reza Zohouri @ Tokyo Institute of Technology
//====================================================================================================================================

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <omp.h>
#include <CL/cl.h>

#include "common/util.h"
#include "common/timer.h"

#ifdef NO_INTERLEAVE
	#include "CL/cl_ext.h"
#endif

#ifdef LEGACY
	#define MEM_BANK_1 CL_MEM_BANK_1_ALTERA
	#define MEM_BANK_2 CL_MEM_BANK_2_ALTERA
#else
	#define MEM_BANK_1 CL_CHANNEL_1_INTELFPGA
	#define MEM_BANK_2 CL_CHANNEL_2_INTELFPGA
#endif

#if defined(BLK3D) || defined(CHBLK3D)
	#define DIM 3
#elif defined(BLK2D) || defined(CHBLK2D)
	#define DIM 2
#else
	#define DIM 1
#endif

// global variables
static cl_context       context;
#if defined(STD) || defined(BLK2D) || defined(BLK3D)
static cl_command_queue queue;
#elif defined(CHSTD) || defined(SCH) || defined(CHBLK2D) || defined(CHBLK3D)
static cl_command_queue queue_read, queue_write;
#endif
static cl_device_id*    deviceList;
static cl_int           deviceCount;

static inline void init()
{
	size_t deviceSize;
	cl_int error;
	cl_uint platformCount;
	cl_platform_id* platforms = NULL;
	cl_device_type   deviceType;
	cl_context_properties ctxprop[3];

	display_device_info(&platforms, &platformCount);
	select_device_type(&deviceType);
	validate_selection(platforms, &platformCount, ctxprop, &deviceType);
	
	// create OpenCL context
	context = clCreateContextFromType(ctxprop, deviceType, NULL, NULL, &error);
	if(!context)
	{
		printf("ERROR: clCreateContextFromType(%s) failed with error code: ", (deviceType == CL_DEVICE_TYPE_ACCELERATOR) ? "FPGA" : (deviceType == CL_DEVICE_TYPE_GPU) ? "GPU" : "CPU");
		display_error_message(error, stdout);
		exit(-1);
	}

	// get list of devices
	CL_SAFE_CALL( clGetContextInfo(context, CL_CONTEXT_DEVICES, 0, NULL, &deviceSize) );
	deviceCount = (int) (deviceSize / sizeof(cl_device_id));
	if(deviceCount < 1)
	{
		printf("ERROR: No devices found.\n");
		exit(-1);
	}

	// allocate memory for devices
	deviceList = (cl_device_id*)malloc(sizeof(cl_device_id) * deviceCount);
	if(!deviceList)
	{
		printf("ERROR: malloc(deviceList) failed.\n");
		exit(-1);
	}

	CL_SAFE_CALL( clGetContextInfo(context, CL_CONTEXT_DEVICES, deviceSize, deviceList, NULL) );

	// create command queue for the first device
#if defined(STD) || defined(BLK2D) || defined(BLK3D)
	queue = clCreateCommandQueue(context, deviceList[0], 0, NULL);
	if(!queue)
	{
		printf("ERROR: clCreateCommandQueue(queue) failed with error code: ");
		display_error_message(error, stdout);
		exit(-1);
	}
#elif defined(CHSTD) || defined(CHBLK2D) || defined(CHBLK3D)
	queue_read = clCreateCommandQueue(context, deviceList[0], 0, NULL);
	if(!queue_read)
	{
		printf("ERROR: clCreateCommandQueue(queue_read) failed with error code: ");
		display_error_message(error, stdout);
		exit(-1);
	}

	queue_write = clCreateCommandQueue(context, deviceList[0], 0, NULL);
	if(!queue_write)
	{
		printf("ERROR: clCreateCommandQueue(queue_write) failed with error code: ");
		display_error_message(error, stdout);
		exit(-1);
	}
#elif SCH
	// FPGA_1
	queue_read = clCreateCommandQueue(context, deviceList[0], 0, NULL);
	if(!queue_read)
	{
		printf("ERROR: clCreateCommandQueue(queue_read) failed with error code: ");
		display_error_message(error, stdout);
		exit(-1);
	}

	// FPGA_2
	queue_write = clCreateCommandQueue(context, deviceList[1], 0, NULL);
	if(!queue_write)
	{
		printf("ERROR: clCreateCommandQueue(queue_write) failed with error code: ");
		display_error_message(error, stdout);
		exit(-1);
	}
#endif

	free(platforms); // platforms isn't needed in the main function
}

static inline void usage(char **argv)
{
#if defined(STD) || defined(CHSTD)
	printf("\nUsage: %s -s <buffer size in MiB> -n <number of iterations> -pad <array padding indexes> -o <number of overlapped indexes> --verbose --verify\n", argv[0]);
#elif defined(BLK2D) || defined(CHBLK2D)
	printf("\nUsage: %s -x <row width> -y <column height> -n <number of iterations> -pad <array padding indexes> -pad_x <row padding indexes> -hw <halo width> --verbose --verify\n", argv[0]);
#elif defined(BLK3D) || defined(CHBLK3D)
	printf("\nUsage: %s -x <row width> -y <column height> -z <plane size> -n <number of iterations> -pad <array padding indexes> -pad_x <row padding indexes> -pad_y <column padding indexes> -hw <halo width> --verbose --verify\n", argv[0]);
#else
	printf("\nUsage: %s -s <buffer size in MiB> -n <number of iterations> -pad <array padding indexes> --verbose --verify\n", argv[0]);
#endif
}

int main(int argc, char **argv)
{
	// input arguments
	int size_MiB = 100; 							// buffer size, default size is 100 MiB
	int iter = 1;									// number of iterations
	int pad = 0;									// padding
	int verbose = 0, verify = 0;
#if defined(STD) || defined(CHSTD)
	int overlap = 0;
#elif defined(BLK2D) || defined(CHBLK2D)
	int halo = 0;
	int pad_x = 0;
	int dim_x = 5120;
	int dim_y = 5120;
#elif defined(BLK3D) || defined(CHBLK3D)
	int halo = 0;
	int pad_x = 0;
	int pad_y = 0;
	int dim_x = 320;
	int dim_y = 320;
	int dim_z = 256;
#endif

	// timing measurement
	TimeStamp start, end;
	double totalr1w1Time = 0, avgr1w1Time = 0;
#ifndef SCH
	double totalr2w1Time = 0, avgr2w1Time = 0;
#endif

	// for OpenCL errors
	cl_int error = 0;

	int arg = 1;
	while (arg < argc)
	{
	#if !defined(BLK2D) && !defined(CHBLK2D) && !defined(BLK3D) && !defined(CHBLK3D)
		if(strcmp(argv[arg], "-s") == 0)
		{
			size_MiB = atoi(argv[arg + 1]);
			arg += 2;
		}
	#else
		if(strcmp(argv[arg], "-x") == 0)
		{
			dim_x = atoi(argv[arg + 1]);
			arg += 2;
		}
		else if(strcmp(argv[arg], "-y") == 0)
		{
			dim_y = atoi(argv[arg + 1]);
			arg += 2;
		}
		else if(strcmp(argv[arg], "-pad_x") == 0)
		{
			pad_x = atoi(argv[arg + 1]);
			arg += 2;
		}
		#if defined(BLK3D) || defined(CHBLK3D)
		else if(strcmp(argv[arg], "-z") == 0)
		{
			dim_z = atoi(argv[arg + 1]);
			arg += 2;
		}
		else if(strcmp(argv[arg], "-pad_y") == 0)
		{
			pad_y = atoi(argv[arg + 1]);
			arg += 2;
		}
		#endif
	#endif
		else if (strcmp(argv[arg], "-n") == 0)
		{
			iter = atoi(argv[arg + 1]);
			arg += 2;
		}
		else if (strcmp(argv[arg], "-pad") == 0)
		{
			pad = atoi(argv[arg + 1]);
			arg += 2;
		}
	#if defined(STD) || defined(CHSTD)
		else if (strcmp(argv[arg], "-o") == 0)
		{
			overlap = atoi(argv[arg + 1]);
			arg += 2;
		}
	#elif defined(BLK2D) || defined(CHBLK2D) || defined(BLK3D) || defined(CHBLK3D)
		else if (strcmp(argv[arg], "-hw") == 0)
		{
			halo = atoi(argv[arg + 1]);
			arg += 2;
		}
	#endif
		else if (strcmp(argv[arg], "--verbose") == 0)
		{
			verbose = 1;
			arg += 1;
		}
		else if (strcmp(argv[arg], "--verify") == 0)
		{
			verify = 1;
			arg += 1;
		}
		else if (strcmp(argv[arg], "-h") == 0 || strcmp(argv[arg], "--help") == 0)
		{
			usage(argv);
			return 0;
		}
		else
		{
			printf("\nInvalid input!");
			usage(argv);
			return -1;
		}
	}

	// set array size based in input buffer size, default is 256k floats (= 100 MiB)
#if defined(BLK2D) || defined(CHBLK2D)
	size_MiB = ((long)dim_x * (long)dim_y * sizeof(float)) / (1024 * 1024);
	long size_B = (long)dim_x * (long)dim_y * sizeof(float);
	long array_size = size_B / sizeof(float);
	long padded_array_size = pad + dim_y * (pad_x + dim_x) + (pad_x + dim_x);
	long padded_size_Byte = padded_array_size * sizeof(float);
	int  padded_size_MiB = padded_size_Byte / (1024 * 1024);
#elif defined(BLK3D) || defined(CHBLK3D)
	size_MiB = ((long)dim_x * (long)dim_y * (long)dim_z * sizeof(float)) / (1024 * 1024);
	long size_B = (long)dim_x * (long)dim_y * (long)dim_z * sizeof(float);
	long array_size = size_B / sizeof(float);
	long padded_array_size = pad + dim_z * (pad_x + dim_x) * (pad_y + dim_y) + (pad_y + dim_y) * (pad_x + dim_x) + (pad_x + dim_x);
	long padded_size_Byte = padded_array_size * sizeof(float);
	int  padded_size_MiB = padded_size_Byte / (1024 * 1024);
#else
	long size_B = (long)size_MiB * 1024 * 1024;
	long array_size = size_B / sizeof(float);
	long padded_array_size = array_size + pad;
	long padded_size_Byte = padded_array_size * sizeof(float);
	int  padded_size_MiB = padded_size_Byte / (1024 * 1024);
#endif

	// OpenCL initialization
	init();

	// load kernel file and build program
#ifdef INTEL_FPGA
	#ifdef SCH
		size_t kernelFileSizeFPGA1, kernelFileSizeFPGA2;
		char *kernelSourceFPGA1 = read_kernel("fpga-stream-kernel_FPGA_1.aocx", &kernelFileSizeFPGA1);
		cl_program progFPGA1 = clCreateProgramWithBinary(context, 1, &deviceList[0], &kernelFileSizeFPGA1, (const unsigned char**)&kernelSourceFPGA1, NULL, &error);
		if(error != CL_SUCCESS)
		{
			printf("ERROR: clCreateProgramWithBinary(FPGA1) failed with error: ");
			display_error_message(error, stdout);
			return -1;
		}

		char *kernelSourceFPGA2 = read_kernel("fpga-stream-kernel_FPGA_2.aocx", &kernelFileSizeFPGA2);
		cl_program progFPGA2 = clCreateProgramWithBinary(context, 1, &deviceList[1], &kernelFileSizeFPGA2, (const unsigned char**)&kernelSourceFPGA2, NULL, &error);
		if(error != CL_SUCCESS)
		{
			printf("ERROR: clCreateProgramWithBinary(FPGA2) failed with error: ");
			display_error_message(error, stdout);
			return -1;
		}
	#else
		size_t kernelFileSize;
		char *kernelSource = read_kernel("fpga-stream-kernel.aocx", &kernelFileSize);
		cl_program prog = clCreateProgramWithBinary(context, deviceCount, deviceList, &kernelFileSize, (const unsigned char**)&kernelSource, NULL, &error);
		if(error != CL_SUCCESS)
		{
			printf("ERROR: clCreateProgramWithBinary() failed with error: ");
			display_error_message(error, stdout);
			return -1;
		}
	#endif
#else // for CPU/GPUs
	#if defined(STD) || defined(BLK2D) || defined(BLK3D)
		size_t kernelFileSize;
		#ifdef STD
			char *kernelSource = read_kernel("fpga-stream-kernel-std.cl", &kernelFileSize);
		#elif BLK2D
			char *kernelSource = read_kernel("fpga-stream-kernel-blk2d.cl", &kernelFileSize);
		#else
			char *kernelSource = read_kernel("fpga-stream-kernel-blk3d.cl", &kernelFileSize);
		#endif
		cl_program prog = clCreateProgramWithSource(context, 1, (const char**)&kernelSource, NULL, &error);
		if(error != CL_SUCCESS)
		{
			printf("ERROR: clCreateProgramWithSource() failed with error: ");
			display_error_message(error, stdout);
			return -1;
		}
	#else
		printf("Kernel not supported on this device!\n");
		return -1;
	#endif
#endif

	char clOptions[200] = "";

#ifndef INTEL_FPGA
	sprintf(clOptions + strlen(clOptions), "-DVEC=%d -DBLOCK_X=%d ", VEC, BLOCK_X);
#endif

#ifdef NDR
	sprintf(clOptions + strlen(clOptions), "-DNDR");
#endif

	// compile kernel file
#ifdef SCH
	clBuildProgram_SAFE(progFPGA1, 1, &deviceList[0], clOptions, NULL, NULL);
	clBuildProgram_SAFE(progFPGA2, 1, &deviceList[1], clOptions, NULL, NULL);
#else
	clBuildProgram_SAFE(prog, deviceCount, deviceList, clOptions, NULL, NULL);
#endif

	// create kernel objects
#if defined(STD) || defined(BLK2D) || defined(BLK3D)
	cl_kernel r1w1Kernel, r2w1Kernel;

	r1w1Kernel = clCreateKernel(prog, "r1w1", &error);
	if(error != CL_SUCCESS)
	{
		printf("ERROR: clCreateKernel(r1w1) failed with error: ");
		display_error_message(error, stdout);
		return -1;
	}

	r2w1Kernel = clCreateKernel(prog, "r2w1", &error);
	if(error != CL_SUCCESS)
	{
		printf("ERROR: clCreateKernel(r2w1) failed with error: ");
		display_error_message(error, stdout);
		return -1;
	}

	clReleaseProgram(prog);
#elif defined(CHSTD) || defined(CHBLK2D) || defined(CHBLK3D)
	cl_kernel r1w1ReadKernel, r1w1WriteKernel, r2w1ReadKernel, r2w1WriteKernel;

	r1w1ReadKernel = clCreateKernel(prog, "r1w1_read", &error);
	if(error != CL_SUCCESS)
	{
		printf("ERROR: clCreateKernel(r1w1_read) failed with error: ");
		display_error_message(error, stdout);
		return -1;
	}

	r1w1WriteKernel= clCreateKernel(prog, "r1w1_write", &error);
	if(error != CL_SUCCESS)
	{
		printf("ERROR: clCreateKernel(r1w1_write) failed with error: ");
		display_error_message(error, stdout);
		return -1;
	}

	r2w1ReadKernel = clCreateKernel(prog, "r2w1_read", &error);
	if(error != CL_SUCCESS)
	{
		printf("ERROR: clCreateKernel(r2w1_write) failed with error: ");
		display_error_message(error, stdout);
		return -1;
	}

	r2w1WriteKernel = clCreateKernel(prog, "r2w1_write", &error);
	if(error != CL_SUCCESS)
	{
		printf("ERROR: clCreateKernel(r2w1_write) failed with error: ");
		display_error_message(error, stdout);
		return -1;
	}

	clReleaseProgram(prog);
#elif SCH
	cl_kernel r1w1ReadKernel, r1w1WriteKernel;

	r1w1ReadKernel = clCreateKernel(progFPGA1, "r1w1_read", &error);
	if(error != CL_SUCCESS)
	{
		printf("ERROR: clCreateKernel(r1w1_read) failed with error: ");
		display_error_message(error, stdout);
		return -1;
	}

	r1w1WriteKernel= clCreateKernel(progFPGA2, "r1w1_write", &error);
	if(error != CL_SUCCESS)
	{
		printf("ERROR: clCreateKernel(r1w1_write) failed with error: ");
		display_error_message(error, stdout);
		return -1;
	}

	clReleaseProgram(progFPGA1);
	clReleaseProgram(progFPGA2);
#endif

#ifdef STD
	printf("Kernel type:           Standard\n");
#elif CHSTD
	printf("Kernel type:           Channelized standard\n");
#elif BLK2D
	printf("Kernel type:           2D overlapped blocking\n");
#elif CHBLK2D
	printf("Kernel type:           Channelized 2D overlapped blocking\n");
#elif BLK3D
	printf("Kernel type:           3D overlapped blocking\n");
#elif CHBLK3D
	printf("Kernel type:           Channelized 3D overlapped blocking\n");
#elif SCH
	printf("Kernel type:           Nallatech 510T serial channel\n");
#endif

#ifdef NDR
	printf("Kernel model:          NDRange\n");
#else
	printf("Kernel model:          Single Work-item\n");
#endif

#if defined(BLK2D) || defined(CHBLK2D) || defined(BLK3D) || defined(CHBLK3D)
	printf("X dimension size:      %d indexes\n", dim_x);
	printf("Y dimension size:      %d indexes\n", dim_y);
#endif
#if defined(BLK3D) || defined(CHBLK3D)
	printf("Z dimension size:      %d indexes\n", dim_z);
#endif

	printf("Array size:            %ld indexes\n", array_size);
	printf("Buffer size:           %d MiB\n", size_MiB);
	printf("Total memory usage:    %d MiB\n", 3 * size_MiB);
	
#ifdef NDR
	#if defined(STD) || defined(BLK2D) || defined(CHBLK2D)
	printf("Work-group\\Block size: %d\n", BLOCK_X);
	#elif defined(BLK3D) || defined(CHBLK3D)
	printf("Work-group\\Block size: %dx%d\n", BLOCK_X, BLOCK_Y);
	#else
	printf("Work-group size:       %d\n", WGS);
	#endif
#else
	#if defined(STD) || defined(BLK2D) || defined(CHBLK2D)
	printf("Block size:            %d\n", BLOCK_X);
	#elif defined(BLK3D) || defined(CHBLK3D)
	printf("Block size:            %dx%d\n", BLOCK_X, BLOCK_Y);
	#endif
#endif

	printf("Vector size:           %d\n", VEC);

#ifdef STD
	printf("Array padding:         %d\n", pad);
	printf("Overlap:               %d\n\n", overlap);
#elif defined(BLK2D) || defined(CHBLK2D)
	printf("Array padding:         %d\n", pad);
	printf("Row padding:           %d\n", pad_x);
	printf("Halo width:            %d\n\n", halo);
#elif defined(BLK3D) || defined(CHBLK3D)
	printf("Array padding:         %d\n", pad);
	printf("Row padding:           %d\n", pad_x);
	printf("Column padding:        %d\n", pad_y);
	printf("Halo width:            %d\n\n", halo);
#else
	printf("Array padding:         %d\n\n", pad);
#endif

	// create host buffers
	if (verbose) printf("Creating host buffers...\n");
	float* hostA = alignedMalloc(padded_size_Byte);
	float* hostB = alignedMalloc(padded_size_Byte);
	float* hostC = alignedMalloc(padded_size_Byte);

	// populate host buffers
	if (verbose) printf("Filling host buffers with random data...\n");
#if defined(BLK2D) || defined(CHBLK2D)
	#pragma omp parallel default(none) firstprivate(dim_x, dim_y, pad, pad_x) shared(hostA, hostB)
	{
		uint seed = omp_get_thread_num();
		#pragma omp for collapse(2)
		for (int i = 0; i < dim_y; i++)
		{
			for (int j = 0; j < dim_x; j++)
			{
				long index = pad + i * (pad_x + dim_x) + (pad_x + j);
				// generate random float numbers between 0 and 1000
				hostA[index] = 1000.0 * (float)rand_r(&seed) / (float)(RAND_MAX);
				hostB[index] = 1000.0 * (float)rand_r(&seed) / (float)(RAND_MAX);
			}
		}
	}
#elif defined(BLK3D) || defined(CHBLK3D)
	#pragma omp parallel default(none) firstprivate(dim_x, dim_y, dim_z, pad, pad_x, pad_y) shared(hostA, hostB)
	{
		uint seed = omp_get_thread_num();
		#pragma omp for collapse(3)
		for (int i = 0; i < dim_z; i++)
		{
			for (int j = 0; j < dim_y; j++)
			{
				for (int k = 0; k < dim_x; k++)
				{
					long index = pad + i * (pad_x + dim_x) * (pad_y + dim_y) + (j + pad_y) * (pad_x + dim_x) + (pad_x + k);
					// generate random float numbers between 0 and 1000
					hostA[index] = 1000.0 * (float)rand_r(&seed) / (float)(RAND_MAX);
					hostB[index] = 1000.0 * (float)rand_r(&seed) / (float)(RAND_MAX);
				}
			}
		}
	}
#else
	#pragma omp parallel default(none) firstprivate(array_size, pad) shared(hostA, hostB)
	{
		uint seed = omp_get_thread_num();
		#pragma omp for
		for (long i = 0; i < array_size; i++)
		{
			// generate random float numbers between 0 and 1000
			hostA[pad + i] = 1000.0 * (float)rand_r(&seed) / (float)(RAND_MAX);
			hostB[pad + i] = 1000.0 * (float)rand_r(&seed) / (float)(RAND_MAX);
		}
	}
#endif

	// create device buffers
	if (verbose) printf("Creating device buffers...\n");
#ifdef NO_INTERLEAVE
	cl_mem deviceA = clCreateBuffer(context, CL_MEM_READ_ONLY  | MEM_BANK_1, padded_size_Byte, NULL, &error);
	if(error != CL_SUCCESS) { printf("ERROR: clCreateBuffer deviceA (size: %d MiB) failed with error: ", padded_size_MiB); display_error_message(error, stdout); return -1;}
	cl_mem deviceB = clCreateBuffer(context, CL_MEM_READ_ONLY  | MEM_BANK_2, padded_size_Byte, NULL, &error);
	if(error != CL_SUCCESS) { printf("ERROR: clCreateBuffer deviceB (size: %d MiB) failed with error: ", padded_size_MiB); display_error_message(error, stdout); return -1;}
	cl_mem deviceC = clCreateBuffer(context, CL_MEM_WRITE_ONLY | MEM_BANK_2, padded_size_Byte, NULL, &error);
	if(error != CL_SUCCESS) { printf("ERROR: clCreateBuffer deviceC (size: %d MiB) failed with error: ", padded_size_MiB); display_error_message(error, stdout); return -1;}
#else
	cl_mem deviceA = clCreateBuffer(context, CL_MEM_READ_ONLY , padded_size_Byte, NULL, &error);
	if(error != CL_SUCCESS) { printf("ERROR: clCreateBuffer deviceA (size: %d MiB) failed with error: ", padded_size_MiB); display_error_message(error, stdout); return -1;}
	cl_mem deviceB = clCreateBuffer(context, CL_MEM_READ_ONLY , padded_size_Byte, NULL, &error);
	if(error != CL_SUCCESS) { printf("ERROR: clCreateBuffer deviceB (size: %d MiB) failed with error: ", padded_size_MiB); display_error_message(error, stdout); return -1;}
	cl_mem deviceC = clCreateBuffer(context, CL_MEM_WRITE_ONLY, padded_size_Byte, NULL, &error);
	if(error != CL_SUCCESS) { printf("ERROR: clCreateBuffer deviceC (size: %d MiB) failed with error: ", padded_size_MiB); display_error_message(error, stdout); return -1;}
#endif

	//write buffers
	if (verbose) printf("Writing data to device...\n");
#if defined(STD) || defined(BLK2D) || defined(BLK3D)
	CL_SAFE_CALL(clEnqueueWriteBuffer(queue, deviceA, 1, 0, padded_size_Byte, hostA, 0, 0, 0));
	CL_SAFE_CALL(clEnqueueWriteBuffer(queue, deviceB, 1, 0, padded_size_Byte, hostB, 0, 0, 0));
#elif defined(CHSTD) || defined(CHBLK2D) || defined(CHBLK3D)
	CL_SAFE_CALL(clEnqueueWriteBuffer(queue_read, deviceA, 1, 0, padded_size_Byte, hostA, 0, 0, 0));
	CL_SAFE_CALL(clEnqueueWriteBuffer(queue_read, deviceB, 1, 0, padded_size_Byte, hostB, 0, 0, 0));
#elif SCH
	CL_SAFE_CALL(clEnqueueWriteBuffer(queue_read, deviceA, 1, 0, padded_size_Byte, hostA, 0, 0, 0));
#endif

#ifdef STD
	int valid_blk_x = BLOCK_X - overlap;
	long exit_index = (array_size % valid_blk_x == 0) ? array_size : array_size + valid_blk_x - (array_size % valid_blk_x);
	int num_blk_x = exit_index / valid_blk_x;

	#ifdef NDR
		long total_index = (long)(BLOCK_X / VEC) * (long)num_blk_x;

		// set local and global work size
		size_t localSize[3] = {(size_t)(BLOCK_X / VEC), 1, 1};
		size_t globalSize[3] = {(size_t)total_index, 1, 1};

		CL_SAFE_CALL( clSetKernelArg(r1w1Kernel, 0, sizeof(cl_mem  ), (void*) &deviceA   ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1Kernel, 1, sizeof(cl_mem  ), (void*) &deviceC   ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1Kernel, 2, sizeof(cl_int  ), (void*) &pad       ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1Kernel, 3, sizeof(cl_long ), (void*) &array_size) );
		CL_SAFE_CALL( clSetKernelArg(r1w1Kernel, 4, sizeof(cl_int  ), (void*) &overlap   ) );

		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 0, sizeof(cl_mem  ), (void*) &deviceA   ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 1, sizeof(cl_mem  ), (void*) &deviceB   ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 2, sizeof(cl_mem  ), (void*) &deviceC   ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 3, sizeof(cl_int  ), (void*) &pad       ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 4, sizeof(cl_long ), (void*) &array_size) );
		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 5, sizeof(cl_int  ), (void*) &overlap   ) );
	#else
		long loop_exit = (long)(BLOCK_X / VEC) * (long)num_blk_x;

		CL_SAFE_CALL( clSetKernelArg(r1w1Kernel, 0, sizeof(cl_mem  ), (void*) &deviceA   ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1Kernel, 1, sizeof(cl_mem  ), (void*) &deviceC   ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1Kernel, 2, sizeof(cl_int  ), (void*) &pad       ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1Kernel, 3, sizeof(cl_long ), (void*) &array_size) );
		CL_SAFE_CALL( clSetKernelArg(r1w1Kernel, 4, sizeof(cl_long ), (void*) &loop_exit ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1Kernel, 5, sizeof(cl_int  ), (void*) &overlap   ) );

		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 0, sizeof(cl_mem  ), (void*) &deviceA   ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 1, sizeof(cl_mem  ), (void*) &deviceB   ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 2, sizeof(cl_mem  ), (void*) &deviceC   ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 3, sizeof(cl_int  ), (void*) &pad       ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 4, sizeof(cl_long ), (void*) &array_size) );
		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 5, sizeof(cl_long ), (void*) &loop_exit ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 6, sizeof(cl_int  ), (void*) &overlap   ) );
	#endif
#elif CHSTD
	int valid_blk_x = BLOCK_X - overlap;
	long exit_index = (array_size % valid_blk_x == 0) ? array_size : array_size + valid_blk_x - (array_size % valid_blk_x);
	int num_blk_x = exit_index / valid_blk_x;

	#ifdef NDR
		long total_index = (long)(BLOCK_X / VEC) * (long)num_blk_x;

		// set local and global work size
		size_t localSize[3] = {(size_t)(BLOCK_X / VEC), 1, 1};
		size_t globalSize[3] = {(size_t)total_index, 1, 1};

		CL_SAFE_CALL( clSetKernelArg(r1w1ReadKernel , 0, sizeof(cl_mem  ), (void*) &deviceA   ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1ReadKernel , 1, sizeof(cl_int  ), (void*) &pad       ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1ReadKernel , 2, sizeof(cl_long ), (void*) &array_size) );
		CL_SAFE_CALL( clSetKernelArg(r1w1ReadKernel , 3, sizeof(cl_int  ), (void*) &overlap   ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1WriteKernel, 0, sizeof(cl_mem  ), (void*) &deviceC   ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1WriteKernel, 1, sizeof(cl_int  ), (void*) &pad       ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1WriteKernel, 2, sizeof(cl_long ), (void*) &array_size) );
		CL_SAFE_CALL( clSetKernelArg(r1w1WriteKernel, 3, sizeof(cl_int  ), (void*) &overlap   ) );

		CL_SAFE_CALL( clSetKernelArg(r2w1ReadKernel , 0, sizeof(cl_mem  ), (void*) &deviceA   ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1ReadKernel , 1, sizeof(cl_mem  ), (void*) &deviceB   ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1ReadKernel , 2, sizeof(cl_int  ), (void*) &pad       ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1ReadKernel , 3, sizeof(cl_long ), (void*) &array_size) );
		CL_SAFE_CALL( clSetKernelArg(r2w1ReadKernel , 4, sizeof(cl_int  ), (void*) &overlap   ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1WriteKernel, 0, sizeof(cl_mem  ), (void*) &deviceC   ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1WriteKernel, 1, sizeof(cl_int  ), (void*) &pad       ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1WriteKernel, 2, sizeof(cl_long ), (void*) &array_size) );
		CL_SAFE_CALL( clSetKernelArg(r2w1WriteKernel, 3, sizeof(cl_int  ), (void*) &overlap   ) );
	#else
		long loop_exit = (long)(BLOCK_X / VEC) * (long)num_blk_x;

		CL_SAFE_CALL( clSetKernelArg(r1w1ReadKernel , 0, sizeof(cl_mem  ), (void*) &deviceA   ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1ReadKernel , 1, sizeof(cl_int  ), (void*) &pad       ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1ReadKernel , 2, sizeof(cl_long ), (void*) &array_size) );
		CL_SAFE_CALL( clSetKernelArg(r1w1ReadKernel , 3, sizeof(cl_long ), (void*) &loop_exit ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1ReadKernel , 4, sizeof(cl_int  ), (void*) &overlap   ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1WriteKernel, 0, sizeof(cl_mem  ), (void*) &deviceC   ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1WriteKernel, 1, sizeof(cl_int  ), (void*) &pad       ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1WriteKernel, 2, sizeof(cl_long ), (void*) &array_size) );
		CL_SAFE_CALL( clSetKernelArg(r1w1WriteKernel, 3, sizeof(cl_long ), (void*) &loop_exit ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1WriteKernel, 4, sizeof(cl_int  ), (void*) &overlap   ) );

		CL_SAFE_CALL( clSetKernelArg(r2w1ReadKernel , 0, sizeof(cl_mem  ), (void*) &deviceA   ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1ReadKernel , 1, sizeof(cl_mem  ), (void*) &deviceB   ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1ReadKernel , 2, sizeof(cl_int  ), (void*) &pad       ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1ReadKernel , 3, sizeof(cl_long ), (void*) &array_size) );
		CL_SAFE_CALL( clSetKernelArg(r2w1ReadKernel , 4, sizeof(cl_long ), (void*) &loop_exit ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1ReadKernel , 5, sizeof(cl_int  ), (void*) &overlap   ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1WriteKernel, 0, sizeof(cl_mem  ), (void*) &deviceC   ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1WriteKernel, 1, sizeof(cl_int  ), (void*) &pad       ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1WriteKernel, 2, sizeof(cl_long ), (void*) &array_size) );
		CL_SAFE_CALL( clSetKernelArg(r2w1WriteKernel, 3, sizeof(cl_long ), (void*) &loop_exit ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1WriteKernel, 4, sizeof(cl_int  ), (void*) &overlap   ) );
	#endif
#elif BLK2D
	int valid_blk_x = BLOCK_X - 2 * halo;
	int last_x = (dim_x % valid_blk_x == 0) ? dim_x : dim_x + valid_blk_x - (dim_x % valid_blk_x);
	int num_blk_x = last_x / valid_blk_x;

	#ifdef NDR
		int total_dim_x = (BLOCK_X / VEC) * num_blk_x;

		// set local and global work size
		#ifdef INTEL_FPGA
			size_t localSize[3] = {(size_t)(BLOCK_X / VEC), (size_t)dim_y, 1}; // localSize[1] is set like this to ensure the same index traversal ordering as the SWI kernel
		#else
			size_t localSize[3] = {(size_t)(BLOCK_X / VEC), 1, 1}; // localSize[1] is set like this since the above case does not work on GPUs due to local work-group size limit
		#endif
		size_t globalSize[3] = {(size_t)total_dim_x, (size_t)dim_y, 1};

		CL_SAFE_CALL( clSetKernelArg(r1w1Kernel, 0, sizeof(cl_mem  ), (void*) &deviceA   ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1Kernel, 1, sizeof(cl_mem  ), (void*) &deviceC   ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1Kernel, 2, sizeof(cl_int  ), (void*) &pad       ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1Kernel, 3, sizeof(cl_int  ), (void*) &pad_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1Kernel, 4, sizeof(cl_int  ), (void*) &dim_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1Kernel, 5, sizeof(cl_int  ), (void*) &halo      ) );

		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 0, sizeof(cl_mem  ), (void*) &deviceA   ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 1, sizeof(cl_mem  ), (void*) &deviceB   ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 2, sizeof(cl_mem  ), (void*) &deviceC   ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 3, sizeof(cl_int  ), (void*) &pad       ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 4, sizeof(cl_int  ), (void*) &pad_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 5, sizeof(cl_int  ), (void*) &dim_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 6, sizeof(cl_int  ), (void*) &halo      ) );
	#else
		long loop_exit = (long)(BLOCK_X / VEC) * (long)num_blk_x * (long)dim_y;

		CL_SAFE_CALL( clSetKernelArg(r1w1Kernel, 0, sizeof(cl_mem  ), (void*) &deviceA   ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1Kernel, 1, sizeof(cl_mem  ), (void*) &deviceC   ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1Kernel, 2, sizeof(cl_int  ), (void*) &pad       ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1Kernel, 3, sizeof(cl_int  ), (void*) &pad_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1Kernel, 4, sizeof(cl_int  ), (void*) &dim_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1Kernel, 5, sizeof(cl_int  ), (void*) &dim_y     ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1Kernel, 6, sizeof(cl_long ), (void*) &loop_exit ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1Kernel, 7, sizeof(cl_int  ), (void*) &halo      ) );

		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 0, sizeof(cl_mem  ), (void*) &deviceA   ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 1, sizeof(cl_mem  ), (void*) &deviceB   ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 2, sizeof(cl_mem  ), (void*) &deviceC   ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 3, sizeof(cl_int  ), (void*) &pad       ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 4, sizeof(cl_int  ), (void*) &pad_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 5, sizeof(cl_int  ), (void*) &dim_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 6, sizeof(cl_int  ), (void*) &dim_y     ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 7, sizeof(cl_long ), (void*) &loop_exit ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 8, sizeof(cl_int  ), (void*) &halo      ) );
	#endif
#elif CHBLK2D
	int valid_blk_x = BLOCK_X - 2 * halo;
	int last_x = (dim_x % valid_blk_x == 0) ? dim_x : dim_x + valid_blk_x - (dim_x % valid_blk_x);
	int num_blk_x = last_x / valid_blk_x;

	#ifdef NDR
		int total_dim_x = (BLOCK_X / VEC) * num_blk_x;

		// set local and global work size
		size_t localSize[3] = {(size_t)(BLOCK_X / VEC), (size_t)dim_y, 1};
		size_t globalSize[3] = {(size_t)total_dim_x, (size_t)dim_y, 1};

		CL_SAFE_CALL( clSetKernelArg(r1w1ReadKernel , 0, sizeof(cl_mem  ), (void*) &deviceA   ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1ReadKernel , 1, sizeof(cl_int  ), (void*) &pad       ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1ReadKernel , 2, sizeof(cl_int  ), (void*) &pad_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1ReadKernel , 3, sizeof(cl_int  ), (void*) &dim_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1ReadKernel , 4, sizeof(cl_int  ), (void*) &halo      ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1WriteKernel, 0, sizeof(cl_mem  ), (void*) &deviceC   ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1WriteKernel, 1, sizeof(cl_int  ), (void*) &pad       ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1WriteKernel, 2, sizeof(cl_int  ), (void*) &pad_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1WriteKernel, 3, sizeof(cl_int  ), (void*) &dim_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1WriteKernel, 4, sizeof(cl_int  ), (void*) &halo      ) );

		CL_SAFE_CALL( clSetKernelArg(r2w1ReadKernel  , 0, sizeof(cl_mem  ), (void*) &deviceA   ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1ReadKernel  , 1, sizeof(cl_mem  ), (void*) &deviceB   ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1ReadKernel  , 2, sizeof(cl_int  ), (void*) &pad       ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1ReadKernel  , 3, sizeof(cl_int  ), (void*) &pad_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1ReadKernel  , 4, sizeof(cl_int  ), (void*) &dim_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1ReadKernel  , 5, sizeof(cl_int  ), (void*) &halo      ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1WriteKernel , 0, sizeof(cl_mem  ), (void*) &deviceC   ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1WriteKernel , 1, sizeof(cl_int  ), (void*) &pad       ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1WriteKernel , 2, sizeof(cl_int  ), (void*) &pad_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1WriteKernel , 3, sizeof(cl_int  ), (void*) &dim_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1WriteKernel , 4, sizeof(cl_int  ), (void*) &halo      ) );
	#else
		long loop_exit = (long)(BLOCK_X / VEC) * (long)num_blk_x * (long)dim_y;

		CL_SAFE_CALL( clSetKernelArg(r1w1ReadKernel , 0, sizeof(cl_mem  ), (void*) &deviceA   ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1ReadKernel , 1, sizeof(cl_int  ), (void*) &pad       ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1ReadKernel , 2, sizeof(cl_int  ), (void*) &pad_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1ReadKernel , 3, sizeof(cl_int  ), (void*) &dim_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1ReadKernel , 4, sizeof(cl_int  ), (void*) &dim_y     ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1ReadKernel , 5, sizeof(cl_long ), (void*) &loop_exit ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1ReadKernel , 6, sizeof(cl_int  ), (void*) &halo      ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1WriteKernel, 0, sizeof(cl_mem  ), (void*) &deviceC   ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1WriteKernel, 1, sizeof(cl_int  ), (void*) &pad       ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1WriteKernel, 2, sizeof(cl_int  ), (void*) &pad_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1WriteKernel, 3, sizeof(cl_int  ), (void*) &dim_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1WriteKernel, 4, sizeof(cl_int  ), (void*) &dim_y     ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1WriteKernel, 5, sizeof(cl_long ), (void*) &loop_exit ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1WriteKernel, 6, sizeof(cl_int  ), (void*) &halo      ) );

		CL_SAFE_CALL( clSetKernelArg(r2w1ReadKernel  , 0, sizeof(cl_mem  ), (void*) &deviceA   ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1ReadKernel  , 1, sizeof(cl_mem  ), (void*) &deviceB   ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1ReadKernel  , 2, sizeof(cl_int  ), (void*) &pad       ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1ReadKernel  , 3, sizeof(cl_int  ), (void*) &pad_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1ReadKernel  , 4, sizeof(cl_int  ), (void*) &dim_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1ReadKernel  , 5, sizeof(cl_int  ), (void*) &dim_y     ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1ReadKernel  , 6, sizeof(cl_long ), (void*) &loop_exit ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1ReadKernel  , 7, sizeof(cl_int  ), (void*) &halo      ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1WriteKernel , 0, sizeof(cl_mem  ), (void*) &deviceC   ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1WriteKernel , 1, sizeof(cl_int  ), (void*) &pad       ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1WriteKernel , 2, sizeof(cl_int  ), (void*) &pad_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1WriteKernel , 3, sizeof(cl_int  ), (void*) &dim_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1WriteKernel , 4, sizeof(cl_int  ), (void*) &dim_y     ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1WriteKernel , 5, sizeof(cl_long ), (void*) &loop_exit ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1WriteKernel , 6, sizeof(cl_int  ), (void*) &halo      ) );
	#endif
#elif BLK3D
	int valid_blk_x = BLOCK_X - 2 * halo;
	int valid_blk_y = BLOCK_Y - 2 * halo;
	int last_x = (dim_x % valid_blk_x == 0) ? dim_x : dim_x + valid_blk_x - (dim_x % valid_blk_x);
	int last_y = (dim_y % valid_blk_y == 0) ? dim_y : dim_y + valid_blk_y - (dim_y % valid_blk_y);
	int num_blk_x = last_x / valid_blk_x;
	int num_blk_y = last_y / valid_blk_y;

	#ifdef NDR
		int total_dim_x = (BLOCK_X / VEC) * num_blk_x;
		int total_dim_y = BLOCK_Y * num_blk_y;

		// set local and global work size
		#ifdef INTEL_FPGA
			size_t localSize[3] = {(size_t)(BLOCK_X / VEC), (size_t)(BLOCK_Y), (size_t)dim_z}; // localSize[1] is set like this to ensure the same index traversal ordering as the SWI kernel
		#else
			size_t localSize[3] = {(size_t)(BLOCK_X / VEC), (size_t)(BLOCK_Y), 1}; // localSize[1] is set like this since the above case does not work on GPUs due to local work-group size limit
		#endif
		size_t globalSize[3] = {(size_t)total_dim_x, (size_t)total_dim_y, (size_t)dim_z};

		CL_SAFE_CALL( clSetKernelArg(r1w1Kernel, 0 , sizeof(cl_mem  ), (void*) &deviceA   ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1Kernel, 1 , sizeof(cl_mem  ), (void*) &deviceC   ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1Kernel, 2 , sizeof(cl_int  ), (void*) &pad       ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1Kernel, 3 , sizeof(cl_int  ), (void*) &pad_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1Kernel, 4 , sizeof(cl_int  ), (void*) &pad_y     ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1Kernel, 5 , sizeof(cl_int  ), (void*) &dim_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1Kernel, 6 , sizeof(cl_int  ), (void*) &dim_y     ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1Kernel, 7 , sizeof(cl_int  ), (void*) &halo      ) );

		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 0 , sizeof(cl_mem  ), (void*) &deviceA   ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 1 , sizeof(cl_mem  ), (void*) &deviceB   ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 2 , sizeof(cl_mem  ), (void*) &deviceC   ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 3 , sizeof(cl_int  ), (void*) &pad       ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 4 , sizeof(cl_int  ), (void*) &pad_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 5 , sizeof(cl_int  ), (void*) &pad_y     ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 6 , sizeof(cl_int  ), (void*) &dim_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 7 , sizeof(cl_int  ), (void*) &dim_y     ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 8 , sizeof(cl_int  ), (void*) &halo      ) );
	#else
		long loop_exit = (long)(BLOCK_X / VEC) * (long)num_blk_x * (long)BLOCK_Y * (long)num_blk_y * (long)dim_z;

		CL_SAFE_CALL( clSetKernelArg(r1w1Kernel, 0 , sizeof(cl_mem  ), (void*) &deviceA   ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1Kernel, 1 , sizeof(cl_mem  ), (void*) &deviceC   ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1Kernel, 2 , sizeof(cl_int  ), (void*) &pad       ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1Kernel, 3 , sizeof(cl_int  ), (void*) &pad_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1Kernel, 4 , sizeof(cl_int  ), (void*) &pad_y     ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1Kernel, 5 , sizeof(cl_int  ), (void*) &dim_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1Kernel, 6 , sizeof(cl_int  ), (void*) &dim_y     ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1Kernel, 7 , sizeof(cl_int  ), (void*) &dim_z     ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1Kernel, 8 , sizeof(cl_int  ), (void*) &last_x    ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1Kernel, 9 , sizeof(cl_long ), (void*) &loop_exit ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1Kernel, 10, sizeof(cl_int  ), (void*) &halo      ) );

		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 0 , sizeof(cl_mem  ), (void*) &deviceA   ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 1 , sizeof(cl_mem  ), (void*) &deviceB   ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 2 , sizeof(cl_mem  ), (void*) &deviceC   ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 3 , sizeof(cl_int  ), (void*) &pad       ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 4 , sizeof(cl_int  ), (void*) &pad_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 5 , sizeof(cl_int  ), (void*) &pad_y     ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 6 , sizeof(cl_int  ), (void*) &dim_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 7 , sizeof(cl_int  ), (void*) &dim_y     ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 8 , sizeof(cl_int  ), (void*) &dim_z     ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 9 , sizeof(cl_int  ), (void*) &last_x    ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 10, sizeof(cl_long ), (void*) &loop_exit ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1Kernel , 11, sizeof(cl_int  ), (void*) &halo      ) );
	#endif
#elif CHBLK3D
	int valid_blk_x = BLOCK_X - 2 * halo;
	int valid_blk_y = BLOCK_Y - 2 * halo;
	int last_x = (dim_x % valid_blk_x == 0) ? dim_x : dim_x + valid_blk_x - (dim_x % valid_blk_x);
	int last_y = (dim_y % valid_blk_y == 0) ? dim_y : dim_y + valid_blk_y - (dim_y % valid_blk_y);
	int num_blk_x = last_x / valid_blk_x;
	int num_blk_y = last_y / valid_blk_y;

	#ifdef NDR
		int total_dim_x = (BLOCK_X / VEC) * num_blk_x;
		int total_dim_y = BLOCK_Y * num_blk_y;

		// set local and global work size
		#ifdef INTEL_FPGA
			size_t localSize[3] = {(size_t)(BLOCK_X / VEC), (size_t)(BLOCK_Y), (size_t)dim_z}; // localSize[1] is set like this to ensure the same index traversal ordering as the SWI kernel
		#else
			size_t localSize[3] = {(size_t)(BLOCK_X / VEC), (size_t)(BLOCK_Y), 1}; // localSize[1] is set like this since the above case does not work on GPUs due to local work-group size limit
		#endif
		size_t globalSize[3] = {(size_t)total_dim_x, (size_t)total_dim_y, (size_t)dim_z};

		CL_SAFE_CALL( clSetKernelArg(r1w1ReadKernel , 0 , sizeof(cl_mem  ), (void*) &deviceA   ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1ReadKernel , 1 , sizeof(cl_int  ), (void*) &pad       ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1ReadKernel , 2 , sizeof(cl_int  ), (void*) &pad_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1ReadKernel , 3 , sizeof(cl_int  ), (void*) &pad_y     ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1ReadKernel , 4 , sizeof(cl_int  ), (void*) &dim_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1ReadKernel , 5 , sizeof(cl_int  ), (void*) &dim_y     ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1ReadKernel , 6 , sizeof(cl_int  ), (void*) &halo      ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1WriteKernel, 0 , sizeof(cl_mem  ), (void*) &deviceC   ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1WriteKernel, 1 , sizeof(cl_int  ), (void*) &pad       ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1WriteKernel, 2 , sizeof(cl_int  ), (void*) &pad_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1WriteKernel, 3 , sizeof(cl_int  ), (void*) &pad_y     ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1WriteKernel, 4 , sizeof(cl_int  ), (void*) &dim_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1WriteKernel, 5 , sizeof(cl_int  ), (void*) &dim_y     ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1WriteKernel, 6 , sizeof(cl_int  ), (void*) &halo      ) );

		CL_SAFE_CALL( clSetKernelArg(r2w1ReadKernel  , 0 , sizeof(cl_mem  ), (void*) &deviceA   ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1ReadKernel  , 1 , sizeof(cl_mem  ), (void*) &deviceB   ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1ReadKernel  , 2 , sizeof(cl_int  ), (void*) &pad       ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1ReadKernel  , 3 , sizeof(cl_int  ), (void*) &pad_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1ReadKernel  , 4 , sizeof(cl_int  ), (void*) &pad_y     ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1ReadKernel  , 5 , sizeof(cl_int  ), (void*) &dim_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1ReadKernel  , 6 , sizeof(cl_int  ), (void*) &dim_y     ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1ReadKernel  , 7 , sizeof(cl_int  ), (void*) &halo      ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1WriteKernel , 0 , sizeof(cl_mem  ), (void*) &deviceC   ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1WriteKernel , 1 , sizeof(cl_int  ), (void*) &pad       ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1WriteKernel , 2 , sizeof(cl_int  ), (void*) &pad_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1WriteKernel , 3 , sizeof(cl_int  ), (void*) &pad_y     ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1WriteKernel , 4 , sizeof(cl_int  ), (void*) &dim_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1WriteKernel , 5 , sizeof(cl_int  ), (void*) &dim_y     ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1WriteKernel , 6 , sizeof(cl_int  ), (void*) &halo      ) );
	#else
		long loop_exit = (long)(BLOCK_X / VEC) * (long)num_blk_x * (long)BLOCK_Y * (long)num_blk_y * (long)dim_z;

		CL_SAFE_CALL( clSetKernelArg(r1w1ReadKernel , 0 , sizeof(cl_mem  ), (void*) &deviceA   ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1ReadKernel , 1 , sizeof(cl_int  ), (void*) &pad       ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1ReadKernel , 2 , sizeof(cl_int  ), (void*) &pad_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1ReadKernel , 3 , sizeof(cl_int  ), (void*) &pad_y     ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1ReadKernel , 4 , sizeof(cl_int  ), (void*) &dim_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1ReadKernel , 5 , sizeof(cl_int  ), (void*) &dim_y     ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1ReadKernel , 6 , sizeof(cl_int  ), (void*) &dim_z     ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1ReadKernel , 7 , sizeof(cl_int  ), (void*) &last_x    ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1ReadKernel , 8 , sizeof(cl_long ), (void*) &loop_exit ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1ReadKernel , 9 , sizeof(cl_int  ), (void*) &halo      ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1WriteKernel, 0 , sizeof(cl_mem  ), (void*) &deviceC   ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1WriteKernel, 1 , sizeof(cl_int  ), (void*) &pad       ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1WriteKernel, 2 , sizeof(cl_int  ), (void*) &pad_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1WriteKernel, 3 , sizeof(cl_int  ), (void*) &pad_y     ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1WriteKernel, 4 , sizeof(cl_int  ), (void*) &dim_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1WriteKernel, 5 , sizeof(cl_int  ), (void*) &dim_y     ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1WriteKernel, 6 , sizeof(cl_int  ), (void*) &dim_z     ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1WriteKernel, 7 , sizeof(cl_int  ), (void*) &last_x    ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1WriteKernel, 8 , sizeof(cl_long ), (void*) &loop_exit ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1WriteKernel, 9 , sizeof(cl_int  ), (void*) &halo      ) );

		CL_SAFE_CALL( clSetKernelArg(r2w1ReadKernel  , 0 , sizeof(cl_mem  ), (void*) &deviceA   ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1ReadKernel  , 1 , sizeof(cl_mem  ), (void*) &deviceB   ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1ReadKernel  , 2 , sizeof(cl_int  ), (void*) &pad       ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1ReadKernel  , 3 , sizeof(cl_int  ), (void*) &pad_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1ReadKernel  , 4 , sizeof(cl_int  ), (void*) &pad_y     ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1ReadKernel  , 5 , sizeof(cl_int  ), (void*) &dim_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1ReadKernel  , 6 , sizeof(cl_int  ), (void*) &dim_y     ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1ReadKernel  , 7 , sizeof(cl_int  ), (void*) &dim_z     ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1ReadKernel  , 8 , sizeof(cl_int  ), (void*) &last_x    ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1ReadKernel  , 9 , sizeof(cl_long ), (void*) &loop_exit ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1ReadKernel  , 10, sizeof(cl_int  ), (void*) &halo      ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1WriteKernel , 0 , sizeof(cl_mem  ), (void*) &deviceC   ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1WriteKernel , 1 , sizeof(cl_int  ), (void*) &pad       ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1WriteKernel , 2 , sizeof(cl_int  ), (void*) &pad_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1WriteKernel , 3 , sizeof(cl_int  ), (void*) &pad_y     ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1WriteKernel , 4 , sizeof(cl_int  ), (void*) &dim_x     ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1WriteKernel , 5 , sizeof(cl_int  ), (void*) &dim_y     ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1WriteKernel , 6 , sizeof(cl_int  ), (void*) &dim_z     ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1WriteKernel , 7 , sizeof(cl_int  ), (void*) &last_x    ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1WriteKernel , 8 , sizeof(cl_long ), (void*) &loop_exit ) );
		CL_SAFE_CALL( clSetKernelArg(r2w1WriteKernel , 9 , sizeof(cl_int  ), (void*) &halo      ) );
	#endif
#elif SCH
	#ifdef NDR
		CL_SAFE_CALL( clSetKernelArg(r1w1ReadKernel , 0, sizeof(cl_mem  ), (void*) &deviceA   ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1ReadKernel , 1, sizeof(cl_int  ), (void*) &pad       ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1WriteKernel, 0, sizeof(cl_mem  ), (void*) &deviceC   ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1WriteKernel, 1, sizeof(cl_int  ), (void*) &pad       ) );
	#else
		CL_SAFE_CALL( clSetKernelArg(r1w1ReadKernel , 0, sizeof(cl_mem  ), (void*) &deviceA   ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1ReadKernel , 1, sizeof(cl_int  ), (void*) &pad       ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1ReadKernel , 2, sizeof(cl_long ), (void*) &array_size) );
		CL_SAFE_CALL( clSetKernelArg(r1w1WriteKernel, 0, sizeof(cl_mem  ), (void*) &deviceC   ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1WriteKernel, 1, sizeof(cl_int  ), (void*) &pad       ) );
		CL_SAFE_CALL( clSetKernelArg(r1w1WriteKernel, 2, sizeof(cl_long ), (void*) &array_size) );
	#endif
#endif

	// device warm-up
	if (verbose) printf("Device warm-up...\n");
#if defined(STD) || defined(BLK2D) || defined(BLK3D)
	#ifdef NDR
		CL_SAFE_CALL( clEnqueueNDRangeKernel(queue, r1w1Kernel, DIM, NULL, globalSize, localSize, 0, 0, NULL) );
	#else
		CL_SAFE_CALL( clEnqueueTask(queue, r1w1Kernel, 0, NULL, NULL) );
	#endif
		clFinish(queue);
#elif defined(CHSTD) || defined(SCH) || defined(CHBLK2D) || defined(CHBLK3D)
	#ifdef NDR
		CL_SAFE_CALL( clEnqueueNDRangeKernel(queue_read , r1w1ReadKernel , DIM, NULL, globalSize, localSize, 0, 0, NULL) );
		CL_SAFE_CALL( clEnqueueNDRangeKernel(queue_write, r1w1WriteKernel, DIM, NULL, globalSize, localSize, 0, 0, NULL) );
	#else
		CL_SAFE_CALL( clEnqueueTask(queue_read , r1w1ReadKernel , 0, NULL, NULL) );
		CL_SAFE_CALL( clEnqueueTask(queue_write, r1w1WriteKernel, 0, NULL, NULL) );
	#endif
		clFinish(queue_write);
#endif

	// r1w1 kernel
	if (verify || verbose) printf("Executing \"r1w1\" kernel...\n");
	for (int i = 0; i < iter; i++)
	{
		GetTime(start);

#if defined(STD) || defined(BLK2D) || defined(BLK3D)
	#ifdef NDR
		CL_SAFE_CALL( clEnqueueNDRangeKernel(queue, r1w1Kernel, DIM, NULL, globalSize, localSize, 0, 0, NULL) );
	#else
		CL_SAFE_CALL( clEnqueueTask(queue, r1w1Kernel, 0, NULL, NULL) );
	#endif
		clFinish(queue);
#elif defined(CHSTD) || defined(SCH) || defined(CHBLK2D) || defined(CHBLK3D)
	#ifdef NDR
		CL_SAFE_CALL( clEnqueueNDRangeKernel(queue_read , r1w1ReadKernel , DIM, NULL, globalSize, localSize, 0, 0, NULL) );
		CL_SAFE_CALL( clEnqueueNDRangeKernel(queue_write, r1w1WriteKernel, DIM, NULL, globalSize, localSize, 0, 0, NULL) );
	#else
		CL_SAFE_CALL( clEnqueueTask(queue_read , r1w1ReadKernel , 0, NULL, NULL) );
		CL_SAFE_CALL( clEnqueueTask(queue_write, r1w1WriteKernel, 0, NULL, NULL) );
	#endif
		clFinish(queue_write);
#endif

		GetTime(end);
		totalr1w1Time += TimeDiff(start, end);
	}

	// verify r1w1 kernel
	if (verify)
	{
		// read data back to host
		printf("Reading data back from device...\n");
	#if defined(STD) || defined(BLK2D) || defined(BLK3D)
		CL_SAFE_CALL(clEnqueueReadBuffer(queue, deviceC, 1, 0, padded_size_Byte, hostC, 0, 0, 0));
		clFinish(queue);
	#elif defined (CHSTD) || defined(SCH) || defined(CHBLK2D) || defined(CHBLK3D)
		CL_SAFE_CALL(clEnqueueReadBuffer(queue_write, deviceC, 1, 0, padded_size_Byte, hostC, 0, 0, 0));
		clFinish(queue_write);
	#endif

		printf("Verifying \"r1w1\" kernel: ");
		int success = 1;
	#if defined(BLK2D) || defined(CHBLK2D)
		#pragma omp parallel for ordered collapse(2) default(none) firstprivate(dim_x, dim_y, pad, pad_x, hostA, hostC, verbose) shared(success)
		for (int i = 0; i < dim_y; i++)
		{
			for (int j = 0; j < dim_x; j++)
			{
				long index = pad + i * (pad_x + dim_x) + (pad_x + j);
				if (hostA[index] != hostC[index])
				{
					if (verbose) printf("Mismatch at index %ld: Expected = %0.6f, Obtained = %0.6f\n", index, hostA[index], hostC[index]);
					success = 0;
				}
			}
		}
	#elif defined(BLK3D) || defined(CHBLK3D)
		#pragma omp parallel for ordered collapse(3) default(none) firstprivate(dim_x, dim_y, dim_z, pad, pad_x, pad_y, hostA, hostC, verbose) shared(success)
		for (int i = 0; i < dim_z; i++)
		{
			for (int j = 0; j < dim_y; j++)
			{
				for (int k = 0; k < dim_x; k++)
				{
					long index = pad + i * (pad_x + dim_x) * (pad_y + dim_y) + (j + pad_y) * (pad_x + dim_x) + (pad_x + k);
					if (hostA[index] != hostC[index])
					{
						if (verbose) printf("Mismatch at index %ld: Expected = %0.6f, Obtained = %0.6f\n", index, hostA[index], hostC[index]);
						success = 0;
					}
				}
			}
		}
	#else
		#pragma omp parallel for ordered default(none) firstprivate(array_size, pad, hostA, hostC, verbose) shared(success)
		for (long i = 0; i < array_size; i++)
		{
			if (hostA[pad + i] != hostC[pad + i])
			{
				if (verbose) printf("Mismatch at index %ld: Expected = %0.6f, Obtained = %0.6f\n", i, hostA[pad + i], hostC[pad + i]);
				success = 0;
			}
		}
	#endif

		if (success)
		{
			printf("SUCCESS!\n");
		}
		else
		{
			printf("FAILURE!\n");
		}
	}

#ifndef SCH
	// r2w1 kernel
	if (verify || verbose) printf("Executing \"r2w1\" kernel...\n");
	for (int i = 0; i < iter; i++)
	{
		GetTime(start);

#if defined(STD) || defined(BLK2D) || defined(BLK3D)
	#ifdef NDR
		CL_SAFE_CALL( clEnqueueNDRangeKernel(queue, r2w1Kernel, DIM, NULL, globalSize, localSize, 0, 0, NULL) );
	#else
		CL_SAFE_CALL( clEnqueueTask(queue, r2w1Kernel, 0, NULL, NULL) );
	#endif
		clFinish(queue);
#elif defined(CHSTD) || defined(CHBLK2D) || defined(CHBLK3D)
	#ifdef NDR
		CL_SAFE_CALL( clEnqueueNDRangeKernel(queue_read , r2w1ReadKernel , DIM, NULL, globalSize, localSize, 0, 0, NULL) );
		CL_SAFE_CALL( clEnqueueNDRangeKernel(queue_write, r2w1WriteKernel, DIM, NULL, globalSize, localSize, 0, 0, NULL) );
	#else
		CL_SAFE_CALL( clEnqueueTask(queue_read , r2w1ReadKernel , 0, NULL, NULL) );
		CL_SAFE_CALL( clEnqueueTask(queue_write, r2w1WriteKernel, 0, NULL, NULL) );
	#endif
		clFinish(queue_write);
#endif

		GetTime(end);
		totalr2w1Time += TimeDiff(start, end);
	}

	// verify r2w1 kernel
	if (verify)
	{
		// read data back to host
		printf("Reading data back from device...\n");
	#if defined(STD) || defined(BLK2D) || defined(BLK3D)
		CL_SAFE_CALL(clEnqueueReadBuffer(queue, deviceC, 1, 0, padded_size_Byte, hostC, 0, 0, 0));
		clFinish(queue);
	#elif defined(CHSTD) || defined(CHBLK2D) || defined(CHBLK3D)
		CL_SAFE_CALL(clEnqueueReadBuffer(queue_write, deviceC, 1, 0, padded_size_Byte, hostC, 0, 0, 0));
		clFinish(queue_write);
	#endif

		printf("Verifying \"r2w1\" kernel: ");
		int success = 1;
	#if defined(BLK2D) || defined(CHBLK2D)
		#pragma omp parallel for ordered collapse(2) default(none) firstprivate(dim_x, dim_y, pad, pad_x, hostA, hostB, hostC, verbose) shared(success)
		for (int i = 0; i < dim_y; i++)
		{
			for (int j = 0; j < dim_x; j++)
			{
				long index = pad + i * (pad_x + dim_x) + (pad_x + j);
				float out = hostA[index] + hostB[index];
				if (fabs(hostC[index] - out) > 0.001)
				{
					if (verbose) printf("Mismatch at index %ld: Expected = %0.6f, Obtained = %0.6f\n", index, out, hostC[index]);
					success = 0;
				}
			}
		}
	#elif defined(BLK3D) || defined(CHBLK3D)
		#pragma omp parallel for ordered collapse(3) default(none) firstprivate(dim_x, dim_y, dim_z, pad, pad_x, pad_y, hostA, hostB, hostC, verbose) shared(success)
		for (int i = 0; i < dim_z; i++)
		{
			for (int j = 0; j < dim_y; j++)
			{
				for (int k = 0; k < dim_x; k++)
				{
					long index = pad + i * (pad_x + dim_x) * (pad_y + dim_y) + (j + pad_y) * (pad_x + dim_x) + (pad_x + k);
					float out = hostA[index] + hostB[index];
					if (fabs(hostC[index] - out) > 0.001)
					{
						if (verbose) printf("Mismatch at index %ld: Expected = %0.6f, Obtained = %0.6f\n", index, out, hostC[index]);
						success = 0;
					}
				}
			}
		}
	#else
		#pragma omp parallel for ordered default(none) firstprivate(array_size, pad, hostA, hostB, hostC, verbose) shared(success)
		for (long i = 0; i < array_size; i++)
		{
			float out = hostA[pad + i] + hostB[pad + i];
			if (fabs(hostC[pad + i] - out) > 0.001)
			{
				if (verbose) printf("Mismatch at index %ld: Expected = %0.6f, Obtained = %0.6f\n", i, out, hostC[pad + i]);
				success = 0;
			}
		}
	#endif

		if (success)
		{
			printf("SUCCESS!\n");
		}
		else
		{
			printf("FAILURE!\n");
		}
	}
#endif

	if (verify || verbose) printf("\n");
#ifdef STD
	avgr1w1Time = totalr1w1Time / (double)iter;
	avgr2w1Time = totalr2w1Time / (double)iter;
	long totalSize_B = ((num_blk_x * BLOCK_X) - (exit_index + overlap - array_size)) * sizeof(float);
	printf("r1w1: %.3f GB/s (%.3f GiB/s) @%.1f ms\n", (double)(2 * totalSize_B) / (1.0E6 * avgr1w1Time), (double)(2 * totalSize_B * 1000.0) / (pow(1024.0, 3) * avgr1w1Time), avgr1w1Time);
	printf("r2w1: %.3f GB/s (%.3f GiB/s) @%.1f ms\n", (double)(3 * totalSize_B) / (1.0E6 * avgr2w1Time ), (double)(3 * totalSize_B * 1000.0) / (pow(1024.0, 3) * avgr2w1Time ), avgr2w1Time);
#elif defined(BLK2D) || defined(CHBLK2D)
	avgr1w1Time = totalr1w1Time / (double)iter;
	avgr2w1Time = totalr2w1Time / (double)iter;
	int extra_halo_x = (dim_x % valid_blk_x >= halo || dim_x % valid_blk_x == 0) ? 0 : halo - (dim_x % valid_blk_x); // in case the halo width in the last block is not fully traversed
	long totalSize_B = ((num_blk_x * BLOCK_X) - (last_x + 2 * halo - dim_x) - extra_halo_x) * dim_y * sizeof(float);
	printf("r1w1: %.3f GB/s (%.3f GiB/s) @%.1f ms\n", (double)(2 * totalSize_B) / (1.0E6 * avgr1w1Time), (double)(2 * totalSize_B * 1000.0) / (pow(1024.0, 3) * avgr1w1Time), avgr1w1Time);
	printf("r2w1: %.3f GB/s (%.3f GiB/s) @%.1f ms\n", (double)(3 * totalSize_B) / (1.0E6 * avgr2w1Time ), (double)(3 * totalSize_B * 1000.0) / (pow(1024.0, 3) * avgr2w1Time ), avgr2w1Time);
#elif defined(BLK3D) || defined(CHBLK3D)
	avgr1w1Time = totalr1w1Time / (double)iter;
	avgr2w1Time = totalr2w1Time / (double)iter;
	int extra_halo_x = (dim_x % valid_blk_x >= halo || dim_x % valid_blk_x == 0) ? 0 : halo - (dim_x % valid_blk_x); // in case the halo width in the last block is not fully traversed
	int extra_halo_y = (dim_y % valid_blk_y >= halo || dim_y % valid_blk_y == 0) ? 0 : halo - (dim_y % valid_blk_y); // in case the halo width in the last block is not fully traversed
	long totalSize_B = ((num_blk_x * BLOCK_X) * (num_blk_y * BLOCK_X) - ((last_x + 2 * halo) * (last_y + 2 * halo) - ((dim_x - extra_halo_x) * (dim_y - extra_halo_y))) - (num_blk_x - 1 + num_blk_y - 1) * (2 * halo) * halo - ((last_x + halo + extra_halo_x - dim_x) * (num_blk_y - 1) + (last_y + halo + extra_halo_y - dim_y) * (num_blk_x - 1)) * 2 * halo) * dim_z * sizeof(float);
	printf("r1w1: %.3f GB/s (%.3f GiB/s) @%.1f ms\n", (double)(2 * totalSize_B) / (1.0E6 * avgr1w1Time), (double)(2 * totalSize_B * 1000.0) / (pow(1024.0, 3) * avgr1w1Time), avgr1w1Time);
	printf("r2w1: %.3f GB/s (%.3f GiB/s) @%.1f ms\n", (double)(3 * totalSize_B) / (1.0E6 * avgr2w1Time ), (double)(3 * totalSize_B * 1000.0) / (pow(1024.0, 3) * avgr2w1Time ), avgr2w1Time);
#elif SCH
	avgr1w1Time = totalr1w1Time / (double)iter;
	printf("Channel bandwidth: %.3f GB/s (%.3f GiB/s) @%.1f ms\n", (double)(1 * size_B) / (1.0E6 * avgr1w1Time), (double)(1 * size_MiB * 1000.0) / (1024.0 * avgr1w1Time), avgr1w1Time);
	printf("Memory bandwidth : %.3f GB/s (%.3f GiB/s) @%.1f ms\n", (double)(2 * size_B) / (1.0E6 * avgr1w1Time), (double)(2 * size_MiB * 1000.0) / (1024.0 * avgr1w1Time), avgr1w1Time);
#else
	avgr1w1Time = totalr1w1Time / (double)iter;
	avgr2w1Time = totalr2w1Time / (double)iter;
	printf("r1w1: %.3f GB/s (%.3f GiB/s) @%.1f ms\n", (double)(2 * size_B) / (1.0E6 * avgr1w1Time), (double)(2 * size_MiB * 1000.0) / (1024.0 * avgr1w1Time), avgr1w1Time);
	printf("r2w1: %.3f GB/s (%.3f GiB/s) @%.1f ms\n", (double)(3 * size_B) / (1.0E6 * avgr2w1Time ), (double)(3 * size_MiB * 1000.0) / (1024.0 * avgr2w1Time ), avgr2w1Time );
#endif

#if defined(STD) || defined(BLK2D) || defined(BLK3D)
	clReleaseCommandQueue(queue);
#elif defined(CHSTD) || defined(SCH) || defined(CHBLK2D) || defined(CHBLK3D)
	clReleaseCommandQueue(queue_read);
	clReleaseCommandQueue(queue_write);
#endif
	clReleaseContext(context);
	clReleaseMemObject(deviceA);
	clReleaseMemObject(deviceB);
	clReleaseMemObject(deviceC);

	free(hostA);
	free(hostB);
	free(hostC);
#ifdef SCH
	free(kernelSourceFPGA1);
	free(kernelSourceFPGA2);
#else
	free(kernelSource);
#endif
	free(deviceList);
}