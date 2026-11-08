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
#define CL_MAP_READ 1
#define CL_MAP_WRITE 2

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
FN(cl_int, clEnqueueWriteBuffer, (cl_command_queue, cl_mem, cl_uint, size_t, size_t, const void *, cl_uint, void *, void *))
FN(void *, clEnqueueMapBuffer, (cl_command_queue, cl_mem, cl_uint, unsigned long long, size_t, size_t, cl_uint, void *, void *, cl_int *))
FN(cl_int, clEnqueueUnmapMemObject, (cl_command_queue, cl_mem, void *, cl_uint, void *, void *))
FN(cl_int, clEnqueueFillBuffer, (cl_command_queue, cl_mem, const void *, size_t, size_t, size_t, cl_uint, void *, void *))
FN(cl_int, clEnqueueCopyBuffer, (cl_command_queue, cl_mem, cl_mem, size_t, size_t, size_t, cl_uint, void *, void *))
FN(cl_mem, clCreateSubBuffer, (cl_mem, unsigned long long, cl_uint, const void *, cl_int *))

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
    L(clEnqueueWriteBuffer); L(clEnqueueMapBuffer); L(clEnqueueUnmapMemObject); L(clEnqueueFillBuffer);
    L(clEnqueueCopyBuffer); L(clCreateSubBuffer);
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
    int fails = bad != 0;

    /* host <-> device consistency paths (device-local buffer mirrors keep two copies in sync) */
#define CHECK_C(name, expr)                                                                  \
    do {                                                                                     \
        p_clEnqueueReadBuffer(q, mc, 1, 0, sizeof c, c, 0, NULL, NULL);                      \
        int nbad = 0;                                                                        \
        for (int i = 0; i < N; i++)                                                          \
            if (c[i] != (expr)) nbad++;                                                      \
        printf("[%s] %s: %d mismatches\n", nbad ? "FAIL" : "PASS", name, nbad);             \
        fails += nbad != 0;                                                                  \
    } while (0)
    /* 1: rewrite an input with WriteBuffer, run again */
    for (int i = 0; i < N; i++) b[i] = 5.0f;
    p_clEnqueueWriteBuffer(q, mb, 1, 0, sizeof b, b, 0, NULL, NULL);
    p_clEnqueueNDRangeKernel(q, k, 1, NULL, &gs, NULL, 0, NULL, NULL);
    CHECK_C("WriteBuffer -> kernel -> ReadBuffer", (float)i + 5.0f);
    /* 2: modify an input through a map */
    cl_int merr;
    float *pa = (float *)p_clEnqueueMapBuffer(q, ma, 1, CL_MAP_READ | CL_MAP_WRITE, 0, sizeof a, 0, NULL, NULL, &merr);
    if (pa) {
        int mbad = 0;
        for (int i = 0; i < N; i++) { if (pa[i] != (float)i) mbad++; pa[i] = 2.0f; }
        p_clEnqueueUnmapMemObject(q, ma, pa, 0, NULL, NULL);
        printf("[%s] MapBuffer read of an input: %d mismatches\n", mbad ? "FAIL" : "PASS", mbad);
        fails += mbad != 0;
    } else {
        printf("[FAIL] clEnqueueMapBuffer %d\n", merr); fails++;
    }
    p_clEnqueueNDRangeKernel(q, k, 1, NULL, &gs, NULL, 0, NULL, NULL);
    CHECK_C("Map write -> kernel", 7.0f);
    /* 3: fill an input */
    float one = 1.0f;
    p_clEnqueueFillBuffer(q, ma, &one, sizeof one, 0, sizeof a, 0, NULL, NULL);
    p_clEnqueueNDRangeKernel(q, k, 1, NULL, &gs, NULL, 0, NULL, NULL);
    CHECK_C("FillBuffer -> kernel", 6.0f);
    /* 4: kernel output copied into an input, chained kernel */
    p_clEnqueueCopyBuffer(q, mc, ma, 0, 0, sizeof a, 0, NULL, NULL); /* a = 6 */
    p_clEnqueueNDRangeKernel(q, k, 1, NULL, &gs, NULL, 0, NULL, NULL);
    CHECK_C("kernel -> CopyBuffer -> kernel", 11.0f);
    /* 5: kernel on sub-buffers (second half of each buffer) */
    struct { size_t origin, size; } half = { sizeof a / 2, sizeof a / 2 };
    cl_mem sa = p_clCreateSubBuffer(ma, CL_MEM_READ_WRITE, 0x1220 /* CL_BUFFER_CREATE_TYPE_REGION */, &half, &merr);
    cl_mem sb = p_clCreateSubBuffer(mb, CL_MEM_READ_WRITE, 0x1220, &half, &merr);
    cl_mem sc = p_clCreateSubBuffer(mc, CL_MEM_READ_WRITE, 0x1220, &half, &merr);
    if (sa && sb && sc) {
        p_clSetKernelArg(k, 0, sizeof sa, &sa);
        p_clSetKernelArg(k, 1, sizeof sb, &sb);
        p_clSetKernelArg(k, 2, sizeof sc, &sc);
        size_t hs = N / 2;
        for (int i = 0; i < N; i++) b[i] = 100.0f;
        p_clEnqueueWriteBuffer(q, mb, 1, 0, sizeof b, b, 0, NULL, NULL);
        p_clEnqueueNDRangeKernel(q, k, 1, NULL, &hs, NULL, 0, NULL, NULL);
        CHECK_C("sub-buffer kernel (second half only)", i < N / 2 ? 11.0f : 106.0f);
    } else {
        printf("[FAIL] clCreateSubBuffer %d\n", merr); fails++;
    }
    p_clFinish(q);
    printf("[%s] consistency checks\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
