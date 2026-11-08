/*
 * cltest - OpenCL probe: lists platforms/devices through OpenCL.dll and runs a vector add on the
 * first GPU device.  With a DLL path as argument it also tries to load that ICD directly.
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

typedef int cl_int;
typedef unsigned cl_uint;
typedef void *cl_platform_id, *cl_device_id, *cl_context, *cl_command_queue, *cl_mem, *cl_program, *cl_kernel;
#define CL_DEVICE_TYPE_GPU (1 << 2)
#define CL_DEVICE_TYPE_ALL 0xFFFFFFFF
#define CL_PLATFORM_NAME 0x0902
#define CL_DEVICE_NAME 0x102B
#define CL_MEM_READ_WRITE 1
#define CL_MEM_COPY_HOST_PTR (1 << 5)

#define FN(r, n, a) typedef r(__stdcall *P_##n) a; static P_##n p_##n;
FN(cl_int, clGetPlatformIDs, (cl_uint, cl_platform_id *, cl_uint *))
FN(cl_int, clGetPlatformInfo, (cl_platform_id, cl_uint, size_t, void *, size_t *))
FN(cl_int, clGetDeviceIDs, (cl_platform_id, unsigned long long, cl_uint, cl_device_id *, cl_uint *))
FN(cl_int, clGetDeviceInfo, (cl_device_id, cl_uint, size_t, void *, size_t *))
FN(cl_context, clCreateContext, (void *, cl_uint, const cl_device_id *, void *, void *, cl_int *))
FN(cl_command_queue, clCreateCommandQueue, (cl_context, cl_device_id, unsigned long long, cl_int *))
FN(cl_mem, clCreateBuffer, (cl_context, unsigned long long, size_t, void *, cl_int *))
FN(cl_program, clCreateProgramWithSource, (cl_context, cl_uint, const char **, const size_t *, cl_int *))
FN(cl_int, clBuildProgram, (cl_program, cl_uint, const cl_device_id *, const char *, void *, void *))
FN(cl_kernel, clCreateKernel, (cl_program, const char *, cl_int *))
FN(cl_int, clSetKernelArg, (cl_kernel, cl_uint, size_t, const void *))
FN(cl_int, clEnqueueNDRangeKernel, (cl_command_queue, cl_kernel, cl_uint, const size_t *, const size_t *, const size_t *, cl_uint, void *, void *))
FN(cl_int, clEnqueueReadBuffer, (cl_command_queue, cl_mem, cl_uint, size_t, size_t, void *, cl_uint, void *, void *))
FN(cl_int, clFinish, (cl_command_queue))

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1) {
        HMODULE icd = LoadLibraryA(argv[1]);
        printf("[INFO] LoadLibrary(%s) = %p (err %lu)\n", argv[1], (void *)icd, icd ? 0 : GetLastError());
    }
    HMODULE cl = LoadLibraryA("OpenCL.dll");
    if (!cl) { printf("[FAIL] OpenCL.dll not loadable\n"); return 1; }
#define L(n) p_##n = (P_##n)(void *)GetProcAddress(cl, #n)
    L(clGetPlatformIDs); L(clGetPlatformInfo); L(clGetDeviceIDs); L(clGetDeviceInfo); L(clCreateContext);
    L(clCreateCommandQueue); L(clCreateBuffer); L(clCreateProgramWithSource); L(clBuildProgram); L(clCreateKernel);
    L(clSetKernelArg); L(clEnqueueNDRangeKernel); L(clEnqueueReadBuffer); L(clFinish);
    cl_platform_id plats[8];
    cl_uint np = 0;
    cl_int err = p_clGetPlatformIDs(8, plats, &np);
    printf("[INFO] clGetPlatformIDs err=%d platforms=%u\n", err, np);
    cl_device_id gpu = NULL;
    for (cl_uint i = 0; i < np; i++) {
        char name[256] = "";
        p_clGetPlatformInfo(plats[i], CL_PLATFORM_NAME, sizeof name, name, NULL);
        cl_device_id devs[8];
        cl_uint nd = 0;
        p_clGetDeviceIDs(plats[i], CL_DEVICE_TYPE_ALL, 8, devs, &nd);
        printf("[INFO] platform %u: %s, %u device(s)\n", i, name, nd);
        for (cl_uint j = 0; j < nd; j++) {
            char dn[256] = "";
            p_clGetDeviceInfo(devs[j], CL_DEVICE_NAME, sizeof dn, dn, NULL);
            printf("[INFO]   device %u: %s\n", j, dn);
        }
        /* CLTEST_DEVICE=<n> picks device n of the first platform instead of the first GPU */
        const char *pick = getenv("CLTEST_DEVICE");
        if (!gpu && pick && i == 0 && (cl_uint)atoi(pick) < nd)
            gpu = devs[atoi(pick)];
        if (!gpu && p_clGetDeviceIDs(plats[i], CL_DEVICE_TYPE_GPU, 1, &gpu, &nd) != 0)
            gpu = NULL;
    }
    if (!gpu) { printf("[FAIL] no GPU device\n"); return 1; }
    cl_context ctx = p_clCreateContext(NULL, 1, &gpu, NULL, NULL, &err);
    if (!ctx) { printf("[FAIL] clCreateContext %d\n", err); return 1; }
    cl_command_queue q = p_clCreateCommandQueue(ctx, gpu, 0, &err);
    if (!q) { printf("[FAIL] clCreateCommandQueue %d\n", err); return 1; }
    enum { N = 1 << 20 };
    static float a[N], b[N], c[N];
    for (int i = 0; i < N; i++) { a[i] = (float)i; b[i] = 2.0f * i; }
    cl_mem ma = p_clCreateBuffer(ctx, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR, sizeof a, a, &err);
    cl_mem mb = p_clCreateBuffer(ctx, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR, sizeof b, b, &err);
    cl_mem mc = p_clCreateBuffer(ctx, CL_MEM_READ_WRITE, sizeof c, NULL, &err);
    const char *src = "__kernel void add(__global const float *a, __global const float *b, __global float *c)"
                      "{ size_t i = get_global_id(0); c[i] = a[i] + b[i]; }";
    if (!ma || !mb || !mc) { printf("[FAIL] clCreateBuffer %d\n", err); return 1; }
    cl_program prog = p_clCreateProgramWithSource(ctx, 1, &src, NULL, &err);
    if (!prog) { printf("[FAIL] clCreateProgramWithSource %d\n", err); return 1; }
    err = p_clBuildProgram(prog, 1, &gpu, "", NULL, NULL);
    if (err) { printf("[FAIL] clBuildProgram %d\n", err); return 1; }
    cl_kernel k = p_clCreateKernel(prog, "add", &err);
    if (!k) { printf("[FAIL] clCreateKernel %d\n", err); return 1; }
    printf("[PASS] context, queue, buffers, program build, kernel\n");
    p_clSetKernelArg(k, 0, sizeof ma, &ma);
    p_clSetKernelArg(k, 1, sizeof mb, &mb);
    p_clSetKernelArg(k, 2, sizeof mc, &mc);
    size_t gs = N;
    err = p_clEnqueueNDRangeKernel(q, k, 1, NULL, &gs, NULL, 0, NULL, NULL);
    p_clEnqueueReadBuffer(q, mc, 1, 0, sizeof c, c, 0, NULL, NULL);
    p_clFinish(q);
    int bad = 0;
    for (int i = 0; i < N; i++)
        if (c[i] != 3.0f * i) { bad++; }
    printf("[%s] vector add of %d floats: %d mismatches (enqueue err %d)\n", bad ? "FAIL" : "PASS", N, bad, err);
    return bad ? 1 : 0;
}
