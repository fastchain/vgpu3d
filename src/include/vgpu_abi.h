/*
 * vgpu_abi.h - user/kernel ABI of the vgpu kernel driver control device.
 *
 * The control device mirrors the Linux DRM virtgpu ioctl set
 * (include/uapi/drm/virtgpu_drm.h) so Mesa's Venus/virgl winsys logic ports with
 * minimal change.  One open file handle == one virtio-gpu context (like a DRM file).
 * Everything a handle owns (context, resources, user mappings) is torn down on
 * IRP_MJ_CLEANUP, so a crashing process cannot leak GPU state.
 *
 * Shared by the kernel driver (C++), the test tool and the Mesa winsys (C).
 * Version 1.  All structures are 8-byte aligned; pointers are never passed (variable
 * payloads are appended to the structure inside the same METHOD_BUFFERED buffer).
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#pragma once

#if defined(VGPU_ABI_STANDALONE)
/* No Windows headers at all (CRT-less tools, cross builds without the MSVC headers). */
typedef unsigned char      UINT8;
typedef unsigned short     UINT16;
typedef unsigned int       UINT32;
typedef unsigned long long UINT64;
#define FILE_DEVICE_UNKNOWN 0x00000022
#define METHOD_BUFFERED     0
#define FILE_READ_ACCESS    0x0001
#define FILE_WRITE_ACCESS   0x0002
#define CTL_CODE(t, f, m, a) (((t) << 16) | ((a) << 14) | ((f) << 2) | (m))
#elif defined(_KERNEL_MODE)
#include <ntddk.h>      /* CTL_CODE, UINT32, UINT64, UINT8 */
#else
#include <windows.h>
#include <winioctl.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define VGPU_ABI_VERSION 1

/* \\.\VGpu3D */
#define VGPU_DEVICE_NT_NAME  L"\\Device\\VGpu3D"
#define VGPU_DEVICE_DOS_NAME L"\\DosDevices\\VGpu3D"
#define VGPU_DEVICE_USER_PATH L"\\\\.\\VGpu3D"

/* ---- GETPARAM (numbers match VIRTGPU_PARAM_* in Linux) ---------------------------- */
#define VGPU_PARAM_3D_FEATURES          1 /* VIRTIO_GPU_F_VIRGL negotiated               */
#define VGPU_PARAM_CAPSET_QUERY_FIX     2 /* always 1                                    */
#define VGPU_PARAM_RESOURCE_BLOB        3 /* VIRTIO_GPU_F_RESOURCE_BLOB negotiated       */
#define VGPU_PARAM_HOST_VISIBLE         4 /* host-visible shared memory region present   */
#define VGPU_PARAM_CROSS_DEVICE         5 /* always 0                                    */
#define VGPU_PARAM_CONTEXT_INIT         6 /* VIRTIO_GPU_F_CONTEXT_INIT negotiated        */
#define VGPU_PARAM_SUPPORTED_CAPSET_IDS 7 /* bitmask of capset ids (ids < 64)            */
#define VGPU_PARAM_EXPLICIT_DEBUG_NAME  8 /* always 1                                    */
/* driver specific */
#define VGPU_PARAM_ABI_VERSION          0x100
#define VGPU_PARAM_HOST_VISIBLE_SIZE    0x101 /* bytes                                   */
#define VGPU_PARAM_MAX_EXECBUF_SIZE     0x102 /* largest accepted EXECBUFFER cmd payload */
#define VGPU_PARAM_NUM_CAPSETS          0x103

/* ---- capsets ---------------------------------------------------------------------- */
#define VGPU_CAPSET_VIRGL  1
#define VGPU_CAPSET_VIRGL2 2
#define VGPU_CAPSET_VENUS  4
#define VGPU_CAPSET_DRM    6

/* ---- blob (numbers match VIRTGPU_BLOB_* in Linux) --------------------------------- */
#define VGPU_BLOB_MEM_GUEST        0x0001
#define VGPU_BLOB_MEM_HOST3D       0x0002
#define VGPU_BLOB_MEM_HOST3D_GUEST 0x0003
#define VGPU_BLOB_FLAG_USE_MAPPABLE  0x0001
#define VGPU_BLOB_FLAG_USE_SHAREABLE 0x0002
#define VGPU_BLOB_FLAG_USE_CROSS_DEVICE 0x0004

/* ---- execbuffer flags ------------------------------------------------------------- */
#define VGPU_EXECBUF_RING_IDX 0x04   /* ring_idx field is valid (same value as Linux)   */
#define VGPU_EXECBUF_FENCE    0x100  /* request a fence; id returned in fence_id        */

#define VGPU_MAX_RINGS 64
#define VGPU_RING_NONE 0xFFFFFFFFu   /* ring_idx value meaning "global timeline"        */

#pragma pack(push, 8)

typedef struct _VGPU_GETPARAM {
    UINT64 param;
    UINT64 value;                 /* out */
} VGPU_GETPARAM;

typedef struct _VGPU_GET_CAPSET_INFO {
    UINT32 index;                 /* in:  0 .. VGPU_PARAM_NUM_CAPSETS-1 */
    UINT32 capset_id;             /* out */
    UINT32 max_version;           /* out */
    UINT32 max_size;              /* out */
} VGPU_GET_CAPSET_INFO;

typedef struct _VGPU_GET_CAPS {
    UINT32 cap_set_id;            /* in  */
    UINT32 cap_set_ver;           /* in  */
    UINT32 size;                  /* in: bytes available after this header; out: bytes written */
    UINT32 reserved;
    /* UINT8 data[size] follows */
} VGPU_GET_CAPS;

typedef struct _VGPU_CONTEXT_INIT {
    UINT32 capset_id;             /* in: 0 = legacy virgl context w/o capset */
    UINT32 num_rings;             /* in: 0..VGPU_MAX_RINGS                   */
    UINT64 poll_rings_mask;       /* in: informational (Venus)               */
    char   debug_name[64];        /* in: NUL terminated, may be empty        */
    UINT32 ctx_id;                /* out */
    UINT32 reserved;
} VGPU_CONTEXT_INIT;

typedef struct _VGPU_RESOURCE_CREATE_BLOB {
    UINT32 blob_mem;              /* in: VGPU_BLOB_MEM_*   */
    UINT32 blob_flags;            /* in: VGPU_BLOB_FLAG_*  */
    UINT64 blob_id;               /* in: host object id for HOST3D*            */
    UINT64 size;                  /* in: bytes                                  */
    UINT32 cmd_size;              /* in: bytes of a SUBMIT_3D payload sent first */
    UINT32 handle;                /* out: resource handle (== virtio resource_id) */
    /* UINT8 cmd[cmd_size] follows */
} VGPU_RESOURCE_CREATE_BLOB;

typedef struct _VGPU_RESOURCE_CREATE {   /* legacy virgl 3D resource, guest backed */
    UINT32 target;
    UINT32 format;
    UINT32 bind;
    UINT32 width;
    UINT32 height;
    UINT32 depth;
    UINT32 array_size;
    UINT32 last_level;
    UINT32 nr_samples;
    UINT32 flags;
    UINT64 size;                  /* in: guest backing bytes (0 = none) */
    UINT32 stride;                /* in: unused by the kernel, kept for parity */
    UINT32 handle;                /* out */
} VGPU_RESOURCE_CREATE;

typedef struct _VGPU_RESOURCE_INFO {
    UINT32 handle;                /* in  */
    UINT32 res_handle;            /* out (== handle) */
    UINT64 size;                  /* out */
    UINT32 blob_mem;              /* out: 0 for non-blob */
    UINT32 reserved;
} VGPU_RESOURCE_INFO;

typedef struct _VGPU_MAP {
    UINT32 handle;                /* in  */
    UINT32 flags;                 /* in: reserved, 0 */
    UINT64 user_va;               /* out: address in the calling process */
    UINT64 size;                  /* out */
} VGPU_MAP;

typedef struct _VGPU_HANDLE {     /* UNMAP, RESOURCE_CLOSE */
    UINT32 handle;
    UINT32 reserved;
} VGPU_HANDLE;

typedef struct _VGPU_BOX { UINT32 x, y, z, w, h, d; } VGPU_BOX;

typedef struct _VGPU_TRANSFER {   /* TRANSFER_TO_HOST / TRANSFER_FROM_HOST, synchronous */
    UINT32   handle;
    UINT32   level;
    UINT32   stride;
    UINT32   layer_stride;
    UINT64   offset;
    VGPU_BOX box;
} VGPU_TRANSFER;

typedef struct _VGPU_EXECBUFFER {
    UINT32 flags;                 /* VGPU_EXECBUF_* */
    UINT32 size;                  /* bytes of command stream */
    UINT32 ring_idx;              /* valid if VGPU_EXECBUF_RING_IDX */
    UINT32 num_bo_handles;        /* resources to attach to the context first */
    UINT64 fence_id;              /* out: valid if VGPU_EXECBUF_FENCE */
    /* UINT32 bo_handles[num_bo_handles]; then UINT8 cmd[size] (see VGPU_EXECBUF_CMD) */
} VGPU_EXECBUFFER;
#define VGPU_EXECBUF_CMD(p) \
    ((UINT8 *)((UINT32 *)((VGPU_EXECBUFFER *)(p) + 1) + ((VGPU_EXECBUFFER *)(p))->num_bo_handles))
#define VGPU_EXECBUF_HANDLES(p) ((UINT32 *)((VGPU_EXECBUFFER *)(p) + 1))
#define VGPU_EXECBUF_TOTAL(num_bo, cmd_size) \
    (sizeof(VGPU_EXECBUFFER) + (num_bo) * sizeof(UINT32) + (cmd_size))

typedef struct _VGPU_WAIT_FENCE {
    UINT32 ring_idx;              /* VGPU_RING_NONE = global timeline */
    UINT32 flags;                 /* reserved */
    UINT64 fence_id;              /* in */
    UINT32 timeout_ms;            /* in: 0 = poll, 0xFFFFFFFF = infinite (capped at 60s per call) */
    UINT32 signaled;              /* out: 1 if fence_id completed */
} VGPU_WAIT_FENCE;

typedef struct _VGPU_QUERY_FENCE {
    UINT32 ring_idx;              /* in */
    UINT32 reserved;
    UINT64 submitted;             /* out: last fence id issued on this ring  */
    UINT64 completed;             /* out: last fence id completed            */
} VGPU_QUERY_FENCE;

#pragma pack(pop)

/* ---- IOCTLs ----------------------------------------------------------------------- */
#define VGPU_IOCTL(n) CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800 + (n), METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

#define IOCTL_VGPU_GETPARAM            VGPU_IOCTL(0x01)  /* VGPU_GETPARAM            */
#define IOCTL_VGPU_GET_CAPSET_INFO     VGPU_IOCTL(0x02)  /* VGPU_GET_CAPSET_INFO     */
#define IOCTL_VGPU_GET_CAPS            VGPU_IOCTL(0x03)  /* VGPU_GET_CAPS + data     */
#define IOCTL_VGPU_CONTEXT_INIT        VGPU_IOCTL(0x04)  /* VGPU_CONTEXT_INIT        */
#define IOCTL_VGPU_RESOURCE_CREATE_BLOB VGPU_IOCTL(0x05) /* VGPU_RESOURCE_CREATE_BLOB + cmd */
#define IOCTL_VGPU_RESOURCE_CREATE     VGPU_IOCTL(0x06)  /* VGPU_RESOURCE_CREATE     */
#define IOCTL_VGPU_RESOURCE_INFO       VGPU_IOCTL(0x07)  /* VGPU_RESOURCE_INFO       */
#define IOCTL_VGPU_MAP                 VGPU_IOCTL(0x08)  /* VGPU_MAP                 */
#define IOCTL_VGPU_UNMAP               VGPU_IOCTL(0x09)  /* VGPU_HANDLE              */
#define IOCTL_VGPU_RESOURCE_CLOSE      VGPU_IOCTL(0x0A)  /* VGPU_HANDLE              */
#define IOCTL_VGPU_TRANSFER_TO_HOST    VGPU_IOCTL(0x0B)  /* VGPU_TRANSFER            */
#define IOCTL_VGPU_TRANSFER_FROM_HOST  VGPU_IOCTL(0x0C)  /* VGPU_TRANSFER            */
#define IOCTL_VGPU_EXECBUFFER          VGPU_IOCTL(0x0D)  /* VGPU_EXECBUFFER + handles + cmd */
#define IOCTL_VGPU_WAIT_FENCE          VGPU_IOCTL(0x0E)  /* VGPU_WAIT_FENCE          */
#define IOCTL_VGPU_QUERY_FENCE         VGPU_IOCTL(0x0F)  /* VGPU_QUERY_FENCE         */

#ifdef __cplusplus
}
#endif
