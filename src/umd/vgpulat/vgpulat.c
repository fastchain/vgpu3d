/* vgpulat - fence round-trip latency of the vgpu control device (no Vulkan).
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <windows.h>
#include "vgpu_abi.h"

static int ioctl_(HANDLE h, DWORD code, void *b, DWORD n)
{
    DWORD r;
    return DeviceIoControl(h, code, b, n, b, n, &r, NULL);
}

static void run(HANDLE h, UINT32 capset, UINT32 ring, const char *name)
{
    LARGE_INTEGER f, t0, t1, a, b;
    double mn = 1e9, mx = 0;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < 100; i++) {
        VGPU_EXECBUFFER eb = { 0 };
        eb.flags = VGPU_EXECBUF_FENCE | (ring != VGPU_RING_NONE ? VGPU_EXECBUF_RING_IDX : 0);
        eb.ring_idx = ring == VGPU_RING_NONE ? 0 : ring;
        QueryPerformanceCounter(&a);
        if (!ioctl_(h, IOCTL_VGPU_EXECBUFFER, &eb, sizeof eb)) { printf("execbuffer failed %lu\n", GetLastError()); return; }
        VGPU_WAIT_FENCE w = { 0 };
        w.ring_idx = ring; w.fence_id = eb.fence_id; w.timeout_ms = 1000;
        if (!ioctl_(h, IOCTL_VGPU_WAIT_FENCE, &w, sizeof w) || !w.signaled) { printf("wait failed %lu\n", GetLastError()); return; }
        QueryPerformanceCounter(&b);
        double ms = (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart;
        if (ms < mn) mn = ms;
        if (ms > mx) mx = ms;
    }
    QueryPerformanceCounter(&t1);
    printf("%-28s capset %u: avg %.3f ms  min %.3f  max %.3f\n", name, capset,
           (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart / 100, mn, mx);
}

int main(void)
{
    UINT32 caps[2] = { VGPU_CAPSET_VIRGL2, VGPU_CAPSET_VENUS };
    for (int c = 0; c < 2; c++) {
        HANDLE h = CreateFileW(VGPU_DEVICE_USER_PATH, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) { printf("open failed %lu\n", GetLastError()); return 1; }
        VGPU_CONTEXT_INIT ci = { 0 };
        ci.capset_id = caps[c]; ci.num_rings = caps[c] == VGPU_CAPSET_VENUS ? 4 : 0;
        if (!ioctl_(h, IOCTL_VGPU_CONTEXT_INIT, &ci, sizeof ci)) { printf("context init failed %lu\n", GetLastError()); return 1; }
        run(h, caps[c], VGPU_RING_NONE, "global timeline");
        if (caps[c] == VGPU_CAPSET_VENUS) { run(h, caps[c], 0, "venus ring 0 (cpu)"); run(h, caps[c], 1, "venus ring 1 (queue)"); }
        CloseHandle(h);
    }
    return 0;
}
