/*
 * vgputest - smoke test for the vgpu kernel driver's \\.\VGpu3D control device.
 *
 * Freestanding (no CRT): only kernel32.  Prints PASS/FAIL/INFO per step and exits with the
 * number of failed steps.  Run in the Windows guest after installing vgpu.sys.
 *
 *   vgputest.exe            run everything
 *   vgputest.exe -v         also hex-dump the capset blobs
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#define VGPU_ABI_STANDALONE 1
#include "vgpu_abi.h"

typedef void *HANDLE; typedef unsigned long DWORD; typedef int BOOL; typedef unsigned long long UINT_PTR;
typedef const void *LPCVOID; typedef void *LPVOID; typedef char *LPSTR; typedef const char *LPCSTR;
typedef unsigned long long size_t;
#define NULL ((void *)0)
#define TRUE 1
#define FALSE 0
#define INVALID_HANDLE_VALUE ((HANDLE)(long long)-1)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define OPEN_EXISTING 3
#define DECL __declspec(dllimport) __stdcall
HANDLE DECL CreateFileW(const unsigned short *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
BOOL DECL DeviceIoControl(HANDLE, DWORD, LPVOID, DWORD, LPVOID, DWORD, DWORD *, void *);
BOOL DECL CloseHandle(HANDLE);
HANDLE DECL GetStdHandle(DWORD);
BOOL DECL WriteFile(HANDLE, LPCVOID, DWORD, DWORD *, void *);
void DECL ExitProcess(unsigned int);
LPSTR DECL GetCommandLineA(void);
DWORD DECL GetLastError(void);

#pragma function(memset, memcpy)
void *memset(void *d, int c, size_t n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }

static HANDLE g_out;
static int g_fail;
static int g_verbose;

static void put(const char *s)
{
    DWORD n; DWORD len = 0; while (s[len]) len++;
    WriteFile(g_out, s, len, &n, NULL);
}
static void putu(unsigned long long v)
{
    char b[24]; int i = 23; b[i] = 0;
    if (!v) b[--i] = '0';
    while (v) { b[--i] = (char)('0' + v % 10); v /= 10; }
    put(&b[i]);
}
static void puth(unsigned long long v, int digits)
{
    char b[20]; int i = 19; b[i] = 0;
    while (digits-- > 0 || v) { b[--i] = "0123456789abcdef"[v & 15]; v >>= 4; }
    put(&b[i]);
}
static void line(const char *tag, const char *msg) { put(tag); put(" "); put(msg); put("\r\n"); }
static void check(int ok, const char *msg)
{
    if (!ok) g_fail++;
    line(ok ? "[PASS]" : "[FAIL]", msg);
}
static void errcode(const char *what, DWORD e)
{
    put("       "); put(what); put(" -> Win32 error "); putu(e); put(" (0x"); puth(e, 1); put(")\r\n");
}

static BOOL ioctl(HANDLE h, DWORD code, void *buf, DWORD in_len, DWORD out_len, DWORD *ret)
{
    DWORD r = 0;
    BOOL ok = DeviceIoControl(h, code, buf, in_len, buf, out_len, &r, NULL);
    if (ret) *ret = r;
    return ok;
}

static void hexdump(const unsigned char *p, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        if (i % 16 == 0) { put("       "); puth(i, 4); put(": "); }
        puth(p[i], 2); put((i % 16 == 15 || i + 1 == n) ? "\r\n" : " ");
    }
}

void __stdcall start(void)
{
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    {
        char *cl = GetCommandLineA();
        for (; *cl; cl++) if (cl[0] == '-' && cl[1] == 'v') g_verbose = 1;
    }
    put("vgputest - VGpu3D control device smoke test (ABI "); putu(VGPU_ABI_VERSION); put(")\r\n");

    HANDLE h = CreateFileW((const unsigned short *)VGPU_DEVICE_USER_PATH, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        check(0, "open \\\\.\\VGpu3D");
        errcode("CreateFile", GetLastError());
        put("       Is vgpu.sys installed on the virtio-gpu adapter, and does the host offer VIRGL (virtio-gpu-gl)?\r\n");
        ExitProcess(1);
    }
    check(1, "open \\\\.\\VGpu3D");

    /* ---- parameters ---- */
    static const struct { UINT64 id; const char *name; } params[] = {
        { VGPU_PARAM_ABI_VERSION, "ABI_VERSION" }, { VGPU_PARAM_3D_FEATURES, "3D_FEATURES" },
        { VGPU_PARAM_RESOURCE_BLOB, "RESOURCE_BLOB" }, { VGPU_PARAM_HOST_VISIBLE, "HOST_VISIBLE" },
        { VGPU_PARAM_CONTEXT_INIT, "CONTEXT_INIT" }, { VGPU_PARAM_SUPPORTED_CAPSET_IDS, "SUPPORTED_CAPSET_IDS" },
        { VGPU_PARAM_HOST_VISIBLE_SIZE, "HOST_VISIBLE_SIZE" }, { VGPU_PARAM_MAX_EXECBUF_SIZE, "MAX_EXECBUF_SIZE" },
        { VGPU_PARAM_NUM_CAPSETS, "NUM_CAPSETS" },
    };
    UINT64 pv[9] = {0};
    int params_ok = 1;
    for (int i = 0; i < 9; i++) {
        VGPU_GETPARAM p; p.param = params[i].id; p.value = 0;
        if (!ioctl(h, IOCTL_VGPU_GETPARAM, &p, sizeof p, sizeof p, NULL)) { params_ok = 0; errcode(params[i].name, GetLastError()); continue; }
        pv[i] = p.value;
        put("[INFO] "); put(params[i].name); put(" = "); putu(p.value); put("\r\n");
    }
    check(params_ok && pv[0] == VGPU_ABI_VERSION, "GETPARAM / ABI version matches");
    int have_blob = (int)pv[2], have_hostvis = (int)pv[3], have_ctxinit = (int)pv[4];
    unsigned ncap = (unsigned)pv[8];
    UINT64 maxexec = pv[7];

    /* ---- capsets ---- */
    unsigned venus_ver = 0, venus_size = 0, virgl2_ver = 0, virgl2_size = 0, virgl1_ver = 0;
    check(ncap > 0, "host advertises at least one capset");
    for (unsigned i = 0; i < ncap; i++) {
        VGPU_GET_CAPSET_INFO ci; ci.index = i; ci.capset_id = ci.max_version = ci.max_size = 0;
        if (!ioctl(h, IOCTL_VGPU_GET_CAPSET_INFO, &ci, sizeof ci, sizeof ci, NULL)) { check(0, "GET_CAPSET_INFO"); errcode("index", GetLastError()); continue; }
        put("[INFO] capset "); putu(ci.capset_id); put(": max_version "); putu(ci.max_version); put(", max_size "); putu(ci.max_size);
        put(ci.capset_id == VGPU_CAPSET_VENUS ? "  (Venus / Vulkan)" : ci.capset_id == VGPU_CAPSET_VIRGL ? "  (virgl)" : ci.capset_id == VGPU_CAPSET_VIRGL2 ? "  (virgl2)" : "");
        put("\r\n");
        if (ci.capset_id == VGPU_CAPSET_VENUS) { venus_ver = ci.max_version; venus_size = ci.max_size; }
        if (ci.capset_id == VGPU_CAPSET_VIRGL2) { virgl2_ver = ci.max_version; virgl2_size = ci.max_size; }
        if (ci.capset_id == VGPU_CAPSET_VIRGL) { virgl1_ver = ci.max_version; }
        {
            static unsigned char cb[sizeof(VGPU_GET_CAPS) + 4096];
            VGPU_GET_CAPS *gc = (VGPU_GET_CAPS *)cb; DWORD ret = 0;
            memset(cb, 0, sizeof cb);
            gc->cap_set_id = ci.capset_id; gc->cap_set_ver = ci.max_version; /* Venus advertises version 0 */ gc->size = 4096;
            if (ioctl(h, IOCTL_VGPU_GET_CAPS, gc, sizeof *gc + 4096, sizeof *gc + 4096, &ret)) {
                check(gc->size > 0, "GET_CAPS returned data");
                put("       bytes: "); putu(gc->size); put("\r\n");
                if (g_verbose) hexdump((unsigned char *)(gc + 1), gc->size > 128 ? 128 : gc->size);
            } else { check(0, "GET_CAPS"); errcode("capset", GetLastError()); }
        }
    }

    /* ---- context ---- */
    UINT32 capset = venus_size ? VGPU_CAPSET_VENUS : virgl2_ver ? VGPU_CAPSET_VIRGL2 : 0; /* Venus max_version is 0 */
    VGPU_CONTEXT_INIT cx; memset(&cx, 0, sizeof cx);
    cx.capset_id = capset; cx.num_rings = venus_size ? 4 : 0;
    { const char *n = "vgputest"; for (int i = 0; n[i]; i++) cx.debug_name[i] = n[i]; }
    int have_ctx = ioctl(h, IOCTL_VGPU_CONTEXT_INIT, &cx, sizeof cx, sizeof cx, NULL);
    check(have_ctx, "CONTEXT_INIT");
    if (have_ctx) { put("       ctx_id "); putu(cx.ctx_id); put(", capset "); putu(capset); put("\r\n"); }
    else { errcode("CONTEXT_INIT", GetLastError()); }

    /* ---- guest blob: create, map, write/read, unmap, close ---- */
    if (!have_blob) {
        line("[SKIP]", "guest blob (host has no RESOURCE_BLOB)");
    } else {
        union { VGPU_RESOURCE_CREATE_BLOB b; unsigned char raw[sizeof(VGPU_RESOURCE_CREATE_BLOB)]; } u;
        memset(&u, 0, sizeof u);
        u.b.blob_mem = VGPU_BLOB_MEM_GUEST; u.b.blob_flags = VGPU_BLOB_FLAG_USE_MAPPABLE; u.b.size = 1 << 20;
        int ok = ioctl(h, IOCTL_VGPU_RESOURCE_CREATE_BLOB, &u, sizeof u.b, sizeof u.b, NULL);
        check(ok, "RESOURCE_CREATE_BLOB (1 MiB guest memory)");
        if (!ok) errcode("create blob", GetLastError());
        else {
            UINT32 handle = u.b.handle;
            VGPU_MAP m; memset(&m, 0, sizeof m); m.handle = handle;
            int mok = ioctl(h, IOCTL_VGPU_MAP, &m, sizeof m, sizeof m, NULL);
            check(mok && m.user_va, "MAP guest blob into this process");
            if (mok && m.user_va) {
                volatile UINT32 *p = (volatile UINT32 *)(UINT_PTR)m.user_va;
                int zero = 1, good = 1;
                for (unsigned i = 0; i < (1 << 20) / 4; i += 997) if (p[i] != 0) zero = 0;
                check(zero, "mapped pages are zero-filled (no stale kernel data)");
                for (unsigned i = 0; i < (1 << 20) / 4; i++) p[i] = 0xA5000000u ^ i;
                for (unsigned i = 0; i < (1 << 20) / 4; i++) if (p[i] != (0xA5000000u ^ i)) { good = 0; break; }
                check(good, "write/read pattern through user mapping");
                VGPU_HANDLE hh = { handle, 0 };
                check(ioctl(h, IOCTL_VGPU_UNMAP, &hh, sizeof hh, sizeof hh, NULL), "UNMAP");
            } else if (!mok) errcode("map", GetLastError());
            VGPU_HANDLE hh = { handle, 0 };
            check(ioctl(h, IOCTL_VGPU_RESOURCE_CLOSE, &hh, sizeof hh, sizeof hh, NULL), "RESOURCE_CLOSE (host UNREF completed)");
        }
    }

    /* ---- fence round trip: empty fenced submit on the global timeline ---- */
    if (!have_ctx) {
        line("[SKIP]", "fence test (no context)");
    } else {
        VGPU_EXECBUFFER eb; memset(&eb, 0, sizeof eb);
        eb.flags = VGPU_EXECBUF_FENCE; eb.size = 0;
        int ok = ioctl(h, IOCTL_VGPU_EXECBUFFER, &eb, sizeof eb, sizeof eb, NULL);
        check(ok, "EXECBUFFER (empty, fenced, global timeline)");
        if (ok) {
            VGPU_WAIT_FENCE w; memset(&w, 0, sizeof w);
            w.ring_idx = VGPU_RING_NONE; w.fence_id = eb.fence_id; w.timeout_ms = 3000;
            int wok = ioctl(h, IOCTL_VGPU_WAIT_FENCE, &w, sizeof w, sizeof w, NULL);
            put("       fence_id "); putu(eb.fence_id); put(", wait "); put(wok ? "ok" : "error"); put(", signaled "); putu(w.signaled); put("\r\n");
            if (!wok) errcode("wait", GetLastError());
            check(wok && w.signaled, "fence signaled by the host");
        } else errcode("execbuffer", GetLastError());
        if (capset == VGPU_CAPSET_VENUS) {
            /* per-ring timeline: Venus contexts fence on ring_idx */
            VGPU_EXECBUFFER er; memset(&er, 0, sizeof er);
            er.flags = VGPU_EXECBUF_FENCE | VGPU_EXECBUF_RING_IDX; er.ring_idx = 1; er.size = 0;
            int rok = ioctl(h, IOCTL_VGPU_EXECBUFFER, &er, sizeof er, sizeof er, NULL);
            check(rok, "EXECBUFFER (empty, fenced, Venus ring 1)");
            if (rok) {
                VGPU_WAIT_FENCE w; memset(&w, 0, sizeof w);
                w.ring_idx = 1; w.fence_id = er.fence_id; w.timeout_ms = 3000;
                int wok = ioctl(h, IOCTL_VGPU_WAIT_FENCE, &w, sizeof w, sizeof w, NULL);
                put("       fence_id "); putu(er.fence_id); put(", wait "); put(wok ? "ok" : "error"); put(", signaled "); putu(w.signaled); put("\r\n");
                if (!wok) errcode("wait", GetLastError());
                check(wok && w.signaled, "ring fence signaled by the host");
            } else errcode("execbuffer", GetLastError());
        }
    }

    /* ---- virgl path: 3D resource + transfer round trip (second handle, own context) ---- */
    if (virgl2_ver || virgl1_ver) {
        HANDLE h2 = CreateFileW((const unsigned short *)VGPU_DEVICE_USER_PATH, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h2 == INVALID_HANDLE_VALUE) { check(0, "open second handle for virgl test"); }
        else {
            VGPU_CONTEXT_INIT c2; memset(&c2, 0, sizeof c2);
            c2.capset_id = virgl2_ver ? VGPU_CAPSET_VIRGL2 : VGPU_CAPSET_VIRGL;
            int cok = ioctl(h2, IOCTL_VGPU_CONTEXT_INIT, &c2, sizeof c2, sizeof c2, NULL);
            check(cok, "virgl CONTEXT_INIT on second handle");
            if (!cok) errcode("CONTEXT_INIT", GetLastError());
            VGPU_RESOURCE_CREATE rc; memset(&rc, 0, sizeof rc);
            rc.target = 2 /* PIPE_TEXTURE_2D */; rc.format = 1 /* B8G8R8A8_UNORM */; rc.bind = 0x0A /* SAMPLER_VIEW|RENDER_TARGET */;
            rc.width = 64; rc.height = 64; rc.depth = 1; rc.array_size = 1; rc.size = 64 * 64 * 4; rc.stride = 64 * 4;
            int rok = cok && ioctl(h2, IOCTL_VGPU_RESOURCE_CREATE, &rc, sizeof rc, sizeof rc, NULL);
            check(rok, "RESOURCE_CREATE virgl 64x64 BGRA texture");
            if (cok && !rok) errcode("RESOURCE_CREATE", GetLastError());
            if (rok) {
                VGPU_MAP m; memset(&m, 0, sizeof m); m.handle = rc.handle;
                int mok = ioctl(h2, IOCTL_VGPU_MAP, &m, sizeof m, sizeof m, NULL);
                check(mok && m.user_va, "MAP virgl resource backing");
                if (mok && m.user_va) {
                    volatile UINT32 *px = (volatile UINT32 *)(UINT_PTR)m.user_va;
                    VGPU_TRANSFER t; memset(&t, 0, sizeof t);
                    t.handle = rc.handle; t.stride = 64 * 4; t.box.w = 64; t.box.h = 64; t.box.d = 1;
                    for (int i = 0; i < 64 * 64; i++) px[i] = 0xFF000000u | (unsigned)i;
                    int tok = ioctl(h2, IOCTL_VGPU_TRANSFER_TO_HOST, &t, sizeof t, sizeof t, NULL);
                    check(tok, "TRANSFER_TO_HOST (guest -> host texture)");
                    if (!tok) errcode("transfer to host", GetLastError());
                    for (int i = 0; i < 64 * 64; i++) px[i] = 0;
                    tok = tok && ioctl(h2, IOCTL_VGPU_TRANSFER_FROM_HOST, &t, sizeof t, sizeof t, NULL);
                    check(tok, "TRANSFER_FROM_HOST (host texture -> guest)");
                    if (tok) {
                        int same = 1;
                        for (int i = 0; i < 64 * 64; i++) if (px[i] != (0xFF000000u | (unsigned)i)) { same = 0; break; }
                        check(same, "pixels survived the host round trip");
                    }
                }
                VGPU_HANDLE hh = { rc.handle, 0 };
                check(ioctl(h2, IOCTL_VGPU_RESOURCE_CLOSE, &hh, sizeof hh, sizeof hh, NULL), "RESOURCE_CLOSE virgl resource");
            }
            CloseHandle(h2);
        }
    } else {
        line("[SKIP]", "virgl round trip (no virgl capset)");
    }

    /* ---- host-visible Venus-style blob (needs a host object id: informational only) ---- */
    if (have_blob && have_hostvis && venus_ver)
        line("[INFO]", "host-visible region present: HOST3D mappable blobs are possible (needs Venus-created object ids, tested by the Mesa winsys)");
    (void)have_ctxinit; (void)maxexec; (void)venus_size; (void)virgl2_size;

    CloseHandle(h);
    put(g_fail ? "\r\nRESULT: " : "\r\nRESULT: all checks passed\r\n");
    if (g_fail) { putu(g_fail); put(" check(s) failed\r\n"); }
    ExitProcess(g_fail);
}
