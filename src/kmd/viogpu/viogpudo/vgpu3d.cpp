/*
 * vgpu3d.cpp - virtio-gpu 3D / blob / Venus control path (see vgpu3d.h).
 *
 * Threading model
 *   - IOCTLs run at PASSIVE_LEVEL in the caller's process context.
 *   - Control-queue completions run in the display DPC (DISPATCH_LEVEL) and only touch
 *     refcounted context objects, so they never see freed memory.
 *   - Per-handle state is protected by a FAST_MUTEX; per-context fence state by a spin lock.
 *
 * Things that must be verified on first run in a real guest are marked VERIFY.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <ntifs.h> /* KeStackAttachProcess / KAPC_STATE */
#include "viogpudo.h"
#include "vgpu3d.h"
#include "vgpu_abi.h"
#include <wdmsec.h>
#include <ntstrsafe.h>

#define V3D_TAG 'D3gV'
#define V3D_LOG(fmt, ...) DbgPrintEx(DPFLTR_IHVVIDEO_ID, DPFLTR_ERROR_LEVEL, "vgpu3d: " fmt, ##__VA_ARGS__)

/* control queue SG budget: cmd + data pages + resp must fit SGLIST_SIZE (256) */
#define V3D_MAX_EXEC_BYTES   (240u * PAGE_SIZE)
#define V3D_MAX_GUEST_BLOB   (256ull * 1024 * 1024)
#define V3D_MAX_SG_ENTRIES   ((200u * PAGE_SIZE) / sizeof(GPU_MEM_ENTRY))
#define V3D_MAX_CAPSETS      16
#define V3D_SYNC_TIMEOUT_MS  5000
#define V3D_FENCE_SLOTS      (VGPU_MAX_RINGS + 1)   /* last slot: global timeline */

/* {7f1c5b3e-8d2a-4c61-9a07-3e5d1b6f4a92} */
static const GUID VGPU_CLASS_GUID = {0x7f1c5b3e, 0x8d2a, 0x4c61, {0x9a, 0x07, 0x3e, 0x5d, 0x1b, 0x6f, 0x4a, 0x92}};

/* ------------------------------------------------------------------------------------ */
/* types                                                                                */
/* ------------------------------------------------------------------------------------ */

struct VGPU_WAITER
{
    LIST_ENTRY link;
    ULONG slot;
    ULONGLONG id;
    KEVENT ev;
    BOOLEAN signaled;
};

struct VGPU_CTX /* reference counted: completions hold a reference */
{
    volatile LONG refs;
    ULONG ctx_id;
    ULONG num_rings;
    KSPIN_LOCK lock;
    ULONGLONG submitted[V3D_FENCE_SLOTS];
    ULONGLONG completed[V3D_FENCE_SLOTS];
    LIST_ENTRY waiters;
    BOOLEAN lost; /* host reported an error on a fenced command */
};

struct VGPU_RES
{
    LIST_ENTRY link;
    ULONG id;
    ULONG blob_mem; /* 0 = legacy 3D resource */
    ULONG blob_flags;
    ULONGLONG size;
    PMDL guest_mdl;      /* guest backing pages, or NULL */
    PMDL io_mdl;         /* host-visible window MDL (HOST3D mappable), or NULL */
    BOOLEAN host_mapped; /* RESOURCE_MAP_BLOB issued */
    ULONGLONG shm_off;
    ULONG map_info;
    PVOID user_va; /* current user mapping */
    PMDL user_mdl; /* MDL that user_va maps */
    PEPROCESS user_proc;
    BOOLEAN ctx_attached;
};

struct VGPU_FILE
{
    LIST_ENTRY link; /* in g.files */
    FAST_MUTEX lock;
    LIST_ENTRY resources;
    VGPU_CTX *ctx;
    PEPROCESS owner;
    BOOLEAN dead; /* torn down */
};

struct VGPU_CAPSET
{
    ULONG id, max_version, max_size;
};

struct SHM_FREE
{
    LIST_ENTRY link;
    ULONGLONG off, len;
};

struct VGPU_GLOBALS
{
    VioGpuAdapter *adapter;
    CtrlQueue *q;
    VioGpuIdr *idr;
    PDRIVER_OBJECT drv;
    PDEVICE_OBJECT dev;
    BOOLEAN link_created;
    BOOLEAN accepting;
    PDRIVER_DISPATCH orig[IRP_MJ_MAXIMUM_FUNCTION + 1];
    BOOLEAN hooked;
    BOOLEAN dispatch_installed;
    FAST_MUTEX files_lock;
    LIST_ENTRY files;
    BOOLEAN virgl, blob, ctxinit;
    ULONG num_capsets;
    VGPU_CAPSET caps[V3D_MAX_CAPSETS];
    PHYSICAL_ADDRESS shm_pa;
    ULONGLONG shm_size;
    LIST_ENTRY shm_free;
    KSPIN_LOCK shm_lock;
    volatile LONG64 fence_counter;
    volatile LONG next_ctx;
    volatile LONG inflight; /* IOCTLs currently executing */
    /* Set while the adapter is stopping: the virtqueue no longer completes commands and the
     * coming device reset drops all host state, so teardown must not talk to the host. */
    volatile BOOLEAN host_gone;
};

static VGPU_GLOBALS g;

/* ------------------------------------------------------------------------------------ */
/* small helpers                                                                        */
/* ------------------------------------------------------------------------------------ */

/* Non-paged zeroed allocation that never straddles a page when it is DMA'd (the control queue
 * builds one SG element per page from the virtual address). */
static PVOID AllocNP(SIZE_T size)
{
    if (size >= 1024)
    {
        size = (size + PAGE_SIZE - 1) & ~(SIZE_T)(PAGE_SIZE - 1);
    }
    return ExAllocatePool2(POOL_FLAG_NON_PAGED, size, V3D_TAG);
}

static VOID FreeNP(PVOID p)
{
    if (p)
    {
        ExFreePoolWithTag(p, V3D_TAG);
    }
}

/* ---- host-visible window allocator (first fit, page granular) ---- */

static BOOLEAN ShmAlloc(ULONGLONG len, ULONGLONG *off)
{
    BOOLEAN ok = FALSE;
    KIRQL irql;
    len = (len + PAGE_SIZE - 1) & ~(ULONGLONG)(PAGE_SIZE - 1);
    KeAcquireSpinLock(&g.shm_lock, &irql);
    for (PLIST_ENTRY e = g.shm_free.Flink; e != &g.shm_free; e = e->Flink)
    {
        SHM_FREE *f = CONTAINING_RECORD(e, SHM_FREE, link);
        if (f->len >= len)
        {
            *off = f->off;
            f->off += len;
            f->len -= len;
            if (f->len == 0)
            {
                RemoveEntryList(&f->link);
                FreeNP(f);
            }
            ok = TRUE;
            break;
        }
    }
    KeReleaseSpinLock(&g.shm_lock, irql);
    return ok;
}

static VOID ShmFree(ULONGLONG off, ULONGLONG len)
{
    KIRQL irql;
    len = (len + PAGE_SIZE - 1) & ~(ULONGLONG)(PAGE_SIZE - 1);
    SHM_FREE *n = (SHM_FREE *)AllocNP(sizeof(SHM_FREE));
    if (!n)
    {
        return; /* leak the range rather than fail */
    }
    n->off = off;
    n->len = len;
    KeAcquireSpinLock(&g.shm_lock, &irql);
    PLIST_ENTRY pos = &g.shm_free; /* insert before first entry with larger offset */
    for (PLIST_ENTRY e = g.shm_free.Flink; e != &g.shm_free; e = e->Flink)
    {
        if (CONTAINING_RECORD(e, SHM_FREE, link)->off > off)
        {
            pos = e;
            break;
        }
    }
    InsertTailList(pos, &n->link);
    /* merge with next */
    if (n->link.Flink != &g.shm_free)
    {
        SHM_FREE *nx = CONTAINING_RECORD(n->link.Flink, SHM_FREE, link);
        if (n->off + n->len == nx->off)
        {
            n->len += nx->len;
            RemoveEntryList(&nx->link);
            FreeNP(nx);
        }
    }
    /* merge with previous */
    if (n->link.Blink != &g.shm_free)
    {
        SHM_FREE *pv = CONTAINING_RECORD(n->link.Blink, SHM_FREE, link);
        if (pv->off + pv->len == n->off)
        {
            pv->len += n->len;
            RemoveEntryList(&n->link);
            FreeNP(n);
        }
    }
    KeReleaseSpinLock(&g.shm_lock, irql);
}

/* ---- control-queue command plumbing ---- */

struct SYNC_OBJ
{
    KEVENT ev;
};

static VOID SyncCb(PVOID c)
{
    KeSetEvent(&((SYNC_OBJ *)c)->ev, IO_NO_INCREMENT, FALSE);
}

/* Queue vbuf and wait for the device.  On STATUS_IO_TIMEOUT the vbuf and the SYNC_OBJ are
 * intentionally leaked (the device may still write the response); the caller must not free
 * any buffer that was part of the command either. */
static NTSTATUS SyncSend(PGPU_VBUFFER vbuf, ULONG ms, ULONG *resp_type)
{
    vbuf->no_free_bufs = true; /* we own data_buf / external resp_buf */
    if (g.host_gone)
    {
        g.q->ReleaseBuffer(vbuf);
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    SYNC_OBJ *s = (SYNC_OBJ *)AllocNP(sizeof(SYNC_OBJ));
    if (!s)
    {
        g.q->ReleaseBuffer(vbuf);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    KeInitializeEvent(&s->ev, NotificationEvent, FALSE);
    vbuf->complete_cb = SyncCb;
    vbuf->complete_ctx = s;
    vbuf->auto_release = false;

    if ((INT)g.q->QueueBuffer(vbuf) != 0)
    {
        FreeNP(s);
        g.q->ReleaseBuffer(vbuf);
        return STATUS_DEVICE_BUSY;
    }
    LARGE_INTEGER to;
    to.QuadPart = -(LONGLONG)ms * 10000;
    NTSTATUS st = KeWaitForSingleObject(&s->ev, Executive, KernelMode, FALSE, &to);
    if (st == STATUS_TIMEOUT)
    {
        V3D_LOG("command 0x%x timed out\n", ((PGPU_CTRL_HDR)vbuf->buf)->type);
        return STATUS_IO_TIMEOUT;
    }
    *resp_type = ((PGPU_CTRL_HDR)vbuf->resp_buf)->type;
    FreeNP(s);
    return STATUS_SUCCESS;
}

static NTSTATUS RespToStatus(ULONG resp, ULONG expect)
{
    if (resp == expect)
    {
        return STATUS_SUCCESS;
    }
    V3D_LOG("device error response 0x%x (expected 0x%x)\n", resp, expect);
    switch (resp)
    {
        case VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY:
            return STATUS_INSUFFICIENT_RESOURCES;
        case VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID:
        case VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID:
        case VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER:
            return STATUS_INVALID_PARAMETER;
        default:
            return STATUS_DEVICE_PROTOCOL_ERROR;
    }
}

/* Send a command that has no response payload and wait for OK_NODATA. */
static NTSTATUS SyncSimple(PGPU_VBUFFER vbuf)
{
    ULONG resp = 0;
    NTSTATUS st = SyncSend(vbuf, V3D_SYNC_TIMEOUT_MS, &resp);
    if (st == STATUS_IO_TIMEOUT || !NT_SUCCESS(st))
    {
        return st;
    }
    st = RespToStatus(resp, VIRTIO_GPU_RESP_OK_NODATA);
    g.q->ReleaseBuffer(vbuf);
    return st;
}

/* ---- context object ---- */

static VOID CtxRef(VGPU_CTX *c)
{
    InterlockedIncrement(&c->refs);
}

static VOID CtxDeref(VGPU_CTX *c)
{
    if (InterlockedDecrement(&c->refs) == 0)
    {
        FreeNP(c);
    }
}

static VOID CtxFenceComplete(VGPU_CTX *c, ULONG slot, ULONGLONG id, BOOLEAN error)
{
    KIRQL irql;
    KeAcquireSpinLock(&c->lock, &irql);
    if (id > c->completed[slot])
    {
        c->completed[slot] = id;
    }
    if (error)
    {
        c->lost = TRUE;
    }
    for (PLIST_ENTRY e = c->waiters.Flink; e != &c->waiters;)
    {
        PLIST_ENTRY next = e->Flink;
        VGPU_WAITER *w = CONTAINING_RECORD(e, VGPU_WAITER, link);
        if (w->slot == slot && c->completed[slot] >= w->id)
        {
            w->signaled = TRUE;
            RemoveEntryList(&w->link);
            InitializeListHead(&w->link);
            KeSetEvent(&w->ev, IO_NO_INCREMENT, FALSE);
        }
        e = next;
    }
    KeReleaseSpinLock(&c->lock, irql);
}

/* returns STATUS_SUCCESS when signaled, STATUS_TIMEOUT otherwise */
static NTSTATUS CtxWait(VGPU_CTX *c, ULONG slot, ULONGLONG id, ULONG timeout_ms)
{
    KIRQL irql;
    VGPU_WAITER w;
    KeAcquireSpinLock(&c->lock, &irql);
    if (c->completed[slot] >= id)
    {
        KeReleaseSpinLock(&c->lock, irql);
        return c->lost ? STATUS_DEVICE_PROTOCOL_ERROR : STATUS_SUCCESS;
    }
    if (timeout_ms == 0)
    {
        KeReleaseSpinLock(&c->lock, irql);
        return STATUS_TIMEOUT;
    }
    w.slot = slot;
    w.id = id;
    w.signaled = FALSE;
    KeInitializeEvent(&w.ev, NotificationEvent, FALSE);
    InsertTailList(&c->waiters, &w.link);
    KeReleaseSpinLock(&c->lock, irql);

    LARGE_INTEGER to;
    to.QuadPart = -(LONGLONG)timeout_ms * 10000;
    NTSTATUS st = KeWaitForSingleObject(&w.ev, Executive, UserMode, FALSE, &to);

    KeAcquireSpinLock(&c->lock, &irql);
    if (!w.signaled)
    {
        RemoveEntryList(&w.link); /* completer removes + re-inits when it signals */
    }
    BOOLEAN done = (c->completed[slot] >= id);
    BOOLEAN lost = c->lost;
    KeReleaseSpinLock(&c->lock, irql);

    if (done)
    {
        return lost ? STATUS_DEVICE_PROTOCOL_ERROR : STATUS_SUCCESS;
    }
    return (st == STATUS_TIMEOUT) ? STATUS_TIMEOUT : (NT_SUCCESS(st) ? STATUS_TIMEOUT : st);
}

/* ---- asynchronous submit tracking ---- */

struct TRACK
{
    VGPU_CTX *ctx;   /* reference held, may be NULL */
    PGPU_VBUFFER vbuf;
    PVOID payload;   /* freed on completion */
    BOOLEAN fenced;
    ULONG slot;
    ULONGLONG fence;
};

static VOID TrackCb(PVOID c)
{
    TRACK *t = (TRACK *)c;
    ULONG resp = ((PGPU_CTRL_HDR)t->vbuf->resp_buf)->type;
    BOOLEAN err = (resp >= VIRTIO_GPU_RESP_ERR_UNSPEC);
    if (t->ctx)
    {
        if (t->fenced)
        {
            CtxFenceComplete(t->ctx, t->slot, t->fence, err);
        }
        else if (err)
        {
            t->ctx->lost = TRUE;
        }
        CtxDeref(t->ctx);
    }
    FreeNP(t->payload);
    FreeNP(t);
}

/* Takes ownership of vbuf and payload.  Takes its own reference on ctx. */
static NTSTATUS SendAsync(PGPU_VBUFFER vbuf,
                          PVOID payload,
                          ULONG payload_size,
                          VGPU_CTX *ctx,
                          BOOLEAN fenced,
                          ULONG slot,
                          ULONGLONG fence)
{
    vbuf->no_free_bufs = true;
    if (g.host_gone)
    {
        FreeNP(payload);
        g.q->ReleaseBuffer(vbuf);
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    TRACK *t = (TRACK *)AllocNP(sizeof(TRACK));
    if (!t)
    {
        FreeNP(payload);
        g.q->ReleaseBuffer(vbuf);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    t->ctx = ctx;
    if (ctx)
    {
        CtxRef(ctx);
    }
    t->vbuf = vbuf;
    t->payload = payload;
    t->fenced = fenced;
    t->slot = slot;
    t->fence = fence;
    vbuf->data_buf = payload;
    vbuf->data_size = payload_size;
    vbuf->complete_cb = TrackCb;
    vbuf->complete_ctx = t;
    vbuf->auto_release = true;
    if ((INT)g.q->QueueBuffer(vbuf) != 0)
    {
        if (ctx)
        {
            CtxDeref(ctx);
        }
        FreeNP(payload);
        FreeNP(t);
        g.q->ReleaseBuffer(vbuf);
        return STATUS_DEVICE_BUSY;
    }
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------------------------ */
/* guest memory                                                                         */
/* ------------------------------------------------------------------------------------ */

static PMDL AllocGuestPages(SIZE_T bytes)
{
    PHYSICAL_ADDRESS lo = {0}, hi, skip = {0};
    hi.QuadPart = -1;
    bytes = (bytes + PAGE_SIZE - 1) & ~(SIZE_T)(PAGE_SIZE - 1);
    PMDL m = MmAllocatePagesForMdlEx(lo, hi, skip, bytes, MmCached, MM_ALLOCATE_FULLY_REQUIRED);
    if (!m)
    {
        return NULL;
    }
    if (MmGetMdlByteCount(m) < bytes)
    {
        MmFreePagesFromMdl(m);
        ExFreePool(m);
        return NULL;
    }
    /* pages are not zeroed by the memory manager: never expose stale kernel data to user mode */
    PVOID va = MmMapLockedPagesSpecifyCache(m, KernelMode, MmCached, NULL, FALSE, NormalPagePriority | MdlMappingNoExecute);
    if (!va)
    {
        MmFreePagesFromMdl(m);
        ExFreePool(m);
        return NULL;
    }
    RtlZeroMemory(va, bytes);
    MmUnmapLockedPages(va, m);
    return m;
}

static VOID FreeGuestPages(PMDL m)
{
    if (m)
    {
        MmFreePagesFromMdl(m);
        ExFreePool(m);
    }
}

/* Build virtio_gpu_mem_entry[] from the MDL's PFN array, coalescing contiguous runs. */
static GPU_MEM_ENTRY *BuildMemEntries(PMDL m, ULONG *count)
{
    ULONG pages = (ULONG)(MmGetMdlByteCount(m) >> PAGE_SHIFT);
    PPFN_NUMBER pfn = MmGetMdlPfnArray(m);
    ULONG n = 0;
    for (ULONG i = 0; i < pages;)
    {
        ULONG j = i + 1;
        while (j < pages && pfn[j] == pfn[j - 1] + 1 && ((j - i) << PAGE_SHIFT) < 0x40000000u)
        {
            ++j;
        }
        ++n;
        i = j;
    }
    if (n > V3D_MAX_SG_ENTRIES)
    {
        V3D_LOG("backing too fragmented: %lu entries\n", n);
        return NULL;
    }
    GPU_MEM_ENTRY *e = (GPU_MEM_ENTRY *)AllocNP(n * sizeof(GPU_MEM_ENTRY));
    if (!e)
    {
        return NULL;
    }
    ULONG k = 0;
    for (ULONG i = 0; i < pages;)
    {
        ULONG j = i + 1;
        while (j < pages && pfn[j] == pfn[j - 1] + 1 && ((j - i) << PAGE_SHIFT) < 0x40000000u)
        {
            ++j;
        }
        e[k].addr = (ULONGLONG)pfn[i] << PAGE_SHIFT;
        e[k].length = (j - i) << PAGE_SHIFT;
        e[k].padding = 0;
        ++k;
        i = j;
    }
    *count = n;
    return e;
}

/* ------------------------------------------------------------------------------------ */
/* user mappings                                                                        */
/* ------------------------------------------------------------------------------------ */

static VOID UnmapUser(VGPU_RES *r)
{
    if (!r->user_va)
    {
        return;
    }
    KAPC_STATE apc;
    BOOLEAN attached = FALSE;
    if (r->user_proc && IoGetCurrentProcess() != r->user_proc && PsGetProcessExitStatus(r->user_proc) != STATUS_PENDING)
    {
        /* owner already exited: its address space (and this mapping) is gone, nothing to unmap */
        r->user_va = NULL;
        r->user_mdl = NULL;
        ObDereferenceObject(r->user_proc);
        r->user_proc = NULL;
        return;
    }
    if (r->user_proc && IoGetCurrentProcess() != r->user_proc)
    {
        KeStackAttachProcess((PRKPROCESS)r->user_proc, &apc);
        attached = TRUE;
    }
    MmUnmapLockedPages(r->user_va, r->user_mdl);
    if (attached)
    {
        KeUnstackDetachProcess(&apc);
    }
    r->user_va = NULL;
    r->user_mdl = NULL;
    if (r->user_proc)
    {
        ObDereferenceObject(r->user_proc);
        r->user_proc = NULL;
    }
}

/* VERIFY: MmAllocateMdlForIoSpace + user-mode map of a BAR window, and that IoSpace MDLs are
 * released with ExFreePool. */
static NTSTATUS MapIntoUser(VGPU_RES *r, PMDL mdl, MEMORY_CACHING_TYPE cache, PVOID *va_out)
{
    PVOID va = NULL;
    __try
    {
        va = MmMapLockedPagesSpecifyCache(mdl, UserMode, cache, NULL, FALSE, NormalPagePriority | MdlMappingNoExecute);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        va = NULL;
    }
    if (!va)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    r->user_va = va;
    r->user_mdl = mdl;
    r->user_proc = IoGetCurrentProcess();
    ObReferenceObject(r->user_proc);
    *va_out = va;
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------------------------ */
/* resources                                                                            */
/* ------------------------------------------------------------------------------------ */

static VGPU_RES *FindRes(VGPU_FILE *f, ULONG id)
{
    for (PLIST_ENTRY e = f->resources.Flink; e != &f->resources; e = e->Flink)
    {
        VGPU_RES *r = CONTAINING_RECORD(e, VGPU_RES, link);
        if (r->id == id)
        {
            return r;
        }
    }
    return NULL;
}

static NTSTATUS SendCtxResource(ULONG type, ULONG ctx_id, ULONG res_id)
{
    PGPU_VBUFFER vbuf;
    PGPU_CTX_RESOURCE cmd = (PGPU_CTX_RESOURCE)g.q->AllocCmd(&vbuf, sizeof(*cmd));
    if (!cmd)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(cmd, sizeof(*cmd));
    cmd->hdr.type = type;
    cmd->hdr.ctx_id = ctx_id;
    cmd->resource_id = res_id;
    return SendAsync(vbuf, NULL, 0, NULL, FALSE, 0, 0);
}

/* Full teardown of one resource.  Caller holds the file lock and has unlinked r. */
static VOID ResDestroy(VGPU_FILE *f, VGPU_RES *r)
{
    BOOLEAN device_ok = TRUE;
    UnmapUser(r);

    if (r->host_mapped)
    {
        PGPU_VBUFFER vbuf;
        PGPU_RES_UNMAP_BLOB cmd = (PGPU_RES_UNMAP_BLOB)g.q->AllocCmd(&vbuf, sizeof(*cmd));
        if (cmd)
        {
            RtlZeroMemory(cmd, sizeof(*cmd));
            cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB;
            cmd->resource_id = r->id;
            NTSTATUS st = SyncSimple(vbuf);
            if (st == STATUS_IO_TIMEOUT)
            {
                device_ok = FALSE;
            }
        }
        if (device_ok)
        {
            ShmFree(r->shm_off, r->size);
        }
        r->host_mapped = FALSE;
    }
    if (r->io_mdl && device_ok)
    {
        ExFreePool(r->io_mdl);
    }
    r->io_mdl = NULL;

    if (r->ctx_attached && f->ctx)
    {
        SendCtxResource(VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE, f->ctx->ctx_id, r->id);
    }

    PGPU_VBUFFER vbuf;
    PGPU_RES_UNREF cmd = (PGPU_RES_UNREF)g.q->AllocCmd(&vbuf, sizeof(*cmd));
    if (!cmd)
    {
        device_ok = FALSE; /* cannot tell the host to drop the backing: do not free the pages */
    }
    else if (device_ok)
    {
        RtlZeroMemory(cmd, sizeof(*cmd));
        cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
        cmd->resource_id = r->id;
        NTSTATUS st = SyncSimple(vbuf); /* host must be done with guest pages before we free them */
        if (st == STATUS_IO_TIMEOUT)
        {
            device_ok = FALSE;
        }
    }
    if (device_ok)
    {
        FreeGuestPages(r->guest_mdl);
        g.idr->PutId(r->id);
    } /* else: leak pages and id; the device is not answering */
    FreeNP(r);
}

static VOID ResDestroyAll(VGPU_FILE *f)
{
    while (!IsListEmpty(&f->resources))
    {
        PLIST_ENTRY e = RemoveHeadList(&f->resources);
        ResDestroy(f, CONTAINING_RECORD(e, VGPU_RES, link));
    }
}

/* ------------------------------------------------------------------------------------ */
/* capsets                                                                              */
/* ------------------------------------------------------------------------------------ */

static const VGPU_CAPSET *FindCapset(ULONG id)
{
    for (ULONG i = 0; i < g.num_capsets; ++i)
    {
        if (g.caps[i].id == id)
        {
            return &g.caps[i];
        }
    }
    return NULL;
}

static VOID QueryCapsets(ULONG n)
{
    if (n > V3D_MAX_CAPSETS)
    {
        n = V3D_MAX_CAPSETS;
    }
    for (ULONG i = 0; i < n; ++i)
    {
        PVOID page = AllocNP(PAGE_SIZE);
        if (!page)
        {
            return;
        }
        PGPU_VBUFFER vbuf;
        PGPU_GET_CAPSET_INFO cmd =
            (PGPU_GET_CAPSET_INFO)g.q->AllocCmdResp(&vbuf, sizeof(*cmd), page, sizeof(GPU_RESP_CAPSET_INFO));
        if (!cmd)
        {
            FreeNP(page);
            return;
        }
        RtlZeroMemory(cmd, sizeof(*cmd));
        cmd->hdr.type = VIRTIO_GPU_CMD_GET_CAPSET_INFO;
        cmd->capset_index = i;
        ULONG resp = 0;
        NTSTATUS st = SyncSend(vbuf, V3D_SYNC_TIMEOUT_MS, &resp);
        if (st == STATUS_IO_TIMEOUT)
        {
            return; /* leaked on purpose */
        }
        if (NT_SUCCESS(st))
        {
            PGPU_RESP_CAPSET_INFO ri = (PGPU_RESP_CAPSET_INFO)page;
            if (ri->hdr.type == VIRTIO_GPU_RESP_OK_CAPSET_INFO)
            {
                g.caps[g.num_capsets].id = ri->capset_id;
                g.caps[g.num_capsets].max_version = ri->capset_max_version;
                g.caps[g.num_capsets].max_size = ri->capset_max_size;
                V3D_LOG("capset[%lu] id=%lu max_version=%lu max_size=%lu\n",
                        i,
                        ri->capset_id,
                        ri->capset_max_version,
                        ri->capset_max_size);
                ++g.num_capsets;
            }
            g.q->ReleaseBuffer(vbuf);
        }
        FreeNP(page);
    }
}

/* ------------------------------------------------------------------------------------ */
/* host-visible shared memory region (virtio PCI cap type 8, id 1)                      */
/* ------------------------------------------------------------------------------------ */

static BOOLEAN FindHostVisibleShm(VioGpuAdapter *a, PHYSICAL_ADDRESS *pa, ULONGLONG *size)
{
    PDXGKRNL_INTERFACE dx = a->GetDxgkInterface();
    PCI_COMMON_HEADER hdr;
    ULONG got = 0;
    RtlZeroMemory(&hdr, sizeof(hdr));
    if (!NT_SUCCESS(dx->DxgkCbReadDeviceSpace(dx->DeviceHandle, DXGK_WHICHSPACE_CONFIG, &hdr, 0, sizeof(hdr), &got)) ||
        got != sizeof(hdr))
    {
        return FALSE;
    }
    if (!(hdr.Status & PCI_STATUS_CAPABILITIES_LIST))
    {
        return FALSE;
    }
    ULONG off = hdr.u.type0.CapabilitiesPtr;
    for (int guard = 0; off >= 0x40 && off + 24 <= 256 && guard < 48; ++guard)
    {
        UCHAR cap[24] = {0};
        if (!NT_SUCCESS(dx->DxgkCbReadDeviceSpace(dx->DeviceHandle, DXGK_WHICHSPACE_CONFIG, cap, off, sizeof(cap), &got)) ||
            got != sizeof(cap))
        {
            return FALSE;
        }
        /* cap[0]=vendor id (9), cap[1]=next, cap[3]=cfg_type, cap[4]=bar, cap[5]=shm id */
        if (cap[0] == 0x09 && cap[3] == VIRTIO_PCI_CAP_SHARED_MEMORY_CFG && cap[5] == VIRTIO_GPU_SHM_ID_HOST_VISIBLE)
        {
            ULONG bar = cap[4];
            ULONGLONG o = *(ULONG *)&cap[8] | ((ULONGLONG) * (ULONG *)&cap[16] << 32);
            ULONGLONG l = *(ULONG *)&cap[12] | ((ULONGLONG) * (ULONG *)&cap[20] << 32);
            if (bar >= PCI_TYPE0_ADDRESSES)
            {
                return FALSE;
            }
            PHYSICAL_ADDRESS base = a->GetPciResources()->GetPciBar(bar)->GetPA();
            if (base.QuadPart == 0 || l == 0)
            {
                V3D_LOG("shm cap found (bar %lu) but BAR has no address\n", bar);
                return FALSE;
            }
            pa->QuadPart = base.QuadPart + (LONGLONG)o;
            if (pa->QuadPart & (PAGE_SIZE - 1))
            {
                V3D_LOG("shm window not page aligned\n");
                return FALSE;
            }
            *size = l;
            return TRUE;
        }
        off = cap[1];
    }
    return FALSE;
}

/* ------------------------------------------------------------------------------------ */
/* IOCTL handlers                                                                       */
/* ------------------------------------------------------------------------------------ */

static NTSTATUS DoGetParam(VGPU_GETPARAM *p)
{
    switch (p->param)
    {
        case VGPU_PARAM_3D_FEATURES:
            p->value = g.virgl;
            break;
        case VGPU_PARAM_CAPSET_QUERY_FIX:
            p->value = 1;
            break;
        case VGPU_PARAM_RESOURCE_BLOB:
            p->value = g.blob;
            break;
        case VGPU_PARAM_HOST_VISIBLE:
            p->value = (g.shm_size != 0);
            break;
        case VGPU_PARAM_CROSS_DEVICE:
            p->value = 0;
            break;
        case VGPU_PARAM_CONTEXT_INIT:
            p->value = g.ctxinit;
            break;
        case VGPU_PARAM_SUPPORTED_CAPSET_IDS:
            {
                ULONGLONG m = 0;
                for (ULONG i = 0; i < g.num_capsets; ++i)
                {
                    if (g.caps[i].id < 64)
                    {
                        m |= 1ull << g.caps[i].id;
                    }
                }
                p->value = m;
                break;
            }
        case VGPU_PARAM_EXPLICIT_DEBUG_NAME:
            p->value = 1;
            break;
        case VGPU_PARAM_ABI_VERSION:
            p->value = VGPU_ABI_VERSION;
            break;
        case VGPU_PARAM_HOST_VISIBLE_SIZE:
            p->value = g.shm_size;
            break;
        case VGPU_PARAM_MAX_EXECBUF_SIZE:
            p->value = V3D_MAX_EXEC_BYTES;
            break;
        case VGPU_PARAM_NUM_CAPSETS:
            p->value = g.num_capsets;
            break;
        default:
            return STATUS_INVALID_PARAMETER;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS DoGetCaps(VGPU_GET_CAPS *p, ULONG avail, ULONG *written)
{
    const VGPU_CAPSET *cs = FindCapset(p->cap_set_id);
    if (!cs || p->cap_set_ver > cs->max_version) /* Venus uses version 0 */
    {
        return STATUS_NOT_FOUND;
    }
    ULONG want = cs->max_size;
    if (want == 0 || (ULONGLONG)want + sizeof(GPU_CTRL_HDR) > PAGE_SIZE)
    {
        return STATUS_NOT_SUPPORTED; /* single-page response buffer */
    }
    PVOID page = AllocNP(PAGE_SIZE);
    if (!page)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    PGPU_VBUFFER vbuf;
    PGPU_GET_CAPSET cmd =
        (PGPU_GET_CAPSET)g.q->AllocCmdResp(&vbuf, sizeof(*cmd), page, sizeof(GPU_CTRL_HDR) + want);
    if (!cmd)
    {
        FreeNP(page);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(cmd, sizeof(*cmd));
    cmd->hdr.type = VIRTIO_GPU_CMD_GET_CAPSET;
    cmd->capset_id = p->cap_set_id;
    cmd->capset_version = p->cap_set_ver;
    ULONG resp = 0;
    NTSTATUS st = SyncSend(vbuf, V3D_SYNC_TIMEOUT_MS, &resp);
    if (st == STATUS_IO_TIMEOUT)
    {
        return st;
    }
    if (NT_SUCCESS(st))
    {
        st = RespToStatus(resp, VIRTIO_GPU_RESP_OK_CAPSET);
        if (NT_SUCCESS(st))
        {
            ULONG n = min(avail, want);
            RtlCopyMemory((PUCHAR)(p + 1), ((PGPU_RESP_CAPSET)page)->capset_data, n);
            p->size = n;
            *written = n;
        }
        g.q->ReleaseBuffer(vbuf);
    }
    FreeNP(page);
    return st;
}

static NTSTATUS DoContextInit(VGPU_FILE *f, VGPU_CONTEXT_INIT *p)
{
    if (!g.ctxinit && p->capset_id != 0)
    {
        return STATUS_NOT_SUPPORTED;
    }
    if (f->ctx)
    {
        return STATUS_ALREADY_REGISTERED;
    }
    if (p->num_rings > VGPU_MAX_RINGS || p->capset_id > VIRTIO_GPU_CONTEXT_INIT_CAPSET_ID_MASK)
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (p->capset_id && !FindCapset(p->capset_id))
    {
        return STATUS_NOT_FOUND;
    }
    p->debug_name[sizeof(p->debug_name) - 1] = 0;

    VGPU_CTX *c = (VGPU_CTX *)AllocNP(sizeof(VGPU_CTX));
    if (!c)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    c->refs = 1;
    c->ctx_id = (ULONG)InterlockedIncrement(&g.next_ctx);
    c->num_rings = p->num_rings;
    KeInitializeSpinLock(&c->lock);
    InitializeListHead(&c->waiters);

    PGPU_VBUFFER vbuf;
    PGPU_CTX_CREATE cmd = (PGPU_CTX_CREATE)g.q->AllocCmd(&vbuf, sizeof(*cmd));
    if (!cmd)
    {
        FreeNP(c);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(cmd, sizeof(*cmd));
    cmd->hdr.type = VIRTIO_GPU_CMD_CTX_CREATE;
    cmd->hdr.ctx_id = c->ctx_id;
    size_t nlen = 0;
    RtlStringCbLengthA(p->debug_name, sizeof(p->debug_name), &nlen);
    cmd->nlen = (ULONG)nlen;
    cmd->context_init = p->capset_id & VIRTIO_GPU_CONTEXT_INIT_CAPSET_ID_MASK;
    RtlCopyMemory(cmd->debug_name, p->debug_name, nlen);

    NTSTATUS st = SyncSimple(vbuf);
    if (!NT_SUCCESS(st))
    {
        FreeNP(c);
        return st;
    }
    f->ctx = c;
    p->ctx_id = c->ctx_id;
    V3D_LOG("context %lu created (capset %lu, rings %lu)\n", c->ctx_id, p->capset_id, p->num_rings);
    return STATUS_SUCCESS;
}

static VOID CtxDestroy(VGPU_FILE *f)
{
    if (!f->ctx)
    {
        return;
    }
    PGPU_VBUFFER vbuf;
    PGPU_CTRL_HDR cmd = (PGPU_CTRL_HDR)g.q->AllocCmd(&vbuf, sizeof(*cmd));
    if (cmd)
    {
        RtlZeroMemory(cmd, sizeof(*cmd));
        cmd->type = VIRTIO_GPU_CMD_CTX_DESTROY;
        cmd->ctx_id = f->ctx->ctx_id;
        SyncSimple(vbuf);
    }
    /* wake anyone still waiting and drop our reference; in-flight completions keep it alive */
    for (ULONG s = 0; s < V3D_FENCE_SLOTS; ++s)
    {
        CtxFenceComplete(f->ctx, s, MAXULONG64, FALSE);
    }
    CtxDeref(f->ctx);
    f->ctx = NULL;
}

static NTSTATUS DoCreateBlob(VGPU_FILE *f, VGPU_RESOURCE_CREATE_BLOB *p, ULONG in_len)
{
    if (!g.blob)
    {
        return STATUS_NOT_SUPPORTED;
    }
    if (p->blob_mem < VGPU_BLOB_MEM_GUEST || p->blob_mem > VGPU_BLOB_MEM_HOST3D_GUEST || p->size == 0 ||
        (p->blob_flags & ~(ULONG)(VGPU_BLOB_FLAG_USE_MAPPABLE | VGPU_BLOB_FLAG_USE_SHAREABLE | VGPU_BLOB_FLAG_USE_CROSS_DEVICE)))
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (p->blob_mem != VGPU_BLOB_MEM_GUEST && !f->ctx)
    {
        return STATUS_INVALID_DEVICE_STATE; /* host objects need a context */
    }
    if (p->cmd_size > V3D_MAX_EXEC_BYTES || (ULONGLONG)sizeof(*p) + p->cmd_size > in_len)
    {
        return STATUS_INVALID_PARAMETER;
    }
    BOOLEAN guest_backed = (p->blob_mem != VGPU_BLOB_MEM_HOST3D);
    if (guest_backed && p->size > V3D_MAX_GUEST_BLOB)
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (p->size >= (1ull << 40))
    {
        return STATUS_INVALID_PARAMETER;
    }

    VGPU_RES *r = (VGPU_RES *)AllocNP(sizeof(VGPU_RES));
    if (!r)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    r->blob_mem = p->blob_mem;
    r->blob_flags = p->blob_flags;
    r->size = p->size;

    GPU_MEM_ENTRY *ents = NULL;
    ULONG nents = 0;
    NTSTATUS st = STATUS_SUCCESS;
    if (guest_backed)
    {
        r->guest_mdl = AllocGuestPages((SIZE_T)p->size);
        if (!r->guest_mdl)
        {
            FreeNP(r);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        ents = BuildMemEntries(r->guest_mdl, &nents);
        if (!ents)
        {
            FreeGuestPages(r->guest_mdl);
            FreeNP(r);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    r->id = g.idr->GetId();

    /* host object creation command first, exactly like Linux virtgpu */
    if (p->cmd_size)
    {
        PGPU_VBUFFER sv;
        PGPU_CMD_SUBMIT sc = (PGPU_CMD_SUBMIT)g.q->AllocCmd(&sv, sizeof(*sc));
        PVOID payload = AllocNP(p->cmd_size);
        if (!sc || !payload)
        {
            st = STATUS_INSUFFICIENT_RESOURCES;
            if (sc)
            {
                g.q->ReleaseBuffer(sv);
            }
            FreeNP(payload);
        }
        else
        {
            RtlCopyMemory(payload, p + 1, p->cmd_size);
            RtlZeroMemory(sc, sizeof(*sc));
            sc->hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D;
            sc->hdr.ctx_id = f->ctx ? f->ctx->ctx_id : 0;
            sc->size = p->cmd_size;
            st = SendAsync(sv, payload, p->cmd_size, f->ctx, FALSE, 0, 0);
        }
    }

    if (NT_SUCCESS(st))
    {
        PGPU_VBUFFER vbuf;
        PGPU_RES_CREATE_BLOB cmd = (PGPU_RES_CREATE_BLOB)g.q->AllocCmd(&vbuf, sizeof(*cmd));
        if (!cmd)
        {
            st = STATUS_INSUFFICIENT_RESOURCES;
        }
        else
        {
            RtlZeroMemory(cmd, sizeof(*cmd));
            cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB;
            cmd->hdr.ctx_id = f->ctx ? f->ctx->ctx_id : 0;
            cmd->resource_id = r->id;
            cmd->blob_mem = p->blob_mem;
            cmd->blob_flags = p->blob_flags;
            cmd->nr_entries = nents;
            cmd->blob_id = p->blob_id;
            cmd->size = p->size;
            vbuf->data_buf = ents;
            vbuf->data_size = nents * sizeof(GPU_MEM_ENTRY);
            st = SyncSimple(vbuf);
            if (st == STATUS_IO_TIMEOUT)
            {
                return st; /* leak everything: device may still read the entries */
            }
        }
    }

    if (!NT_SUCCESS(st))
    {
        FreeNP(ents);
        FreeGuestPages(r->guest_mdl);
        g.idr->PutId(r->id);
        FreeNP(r);
        return st;
    }
    FreeNP(ents);
    InsertTailList(&f->resources, &r->link);
    p->handle = r->id;
    return STATUS_SUCCESS;
}

static NTSTATUS DoCreateRes3D(VGPU_FILE *f, VGPU_RESOURCE_CREATE *p)
{
    UNREFERENCED_PARAMETER(f);
    if (!g.virgl)
    {
        return STATUS_NOT_SUPPORTED;
    }
    if (p->size > V3D_MAX_GUEST_BLOB)
    {
        return STATUS_INVALID_PARAMETER;
    }
    VGPU_RES *r = (VGPU_RES *)AllocNP(sizeof(VGPU_RES));
    if (!r)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    r->size = p->size;
    r->id = g.idr->GetId();

    PGPU_VBUFFER vbuf;
    PGPU_RES_CREATE_3D cmd = (PGPU_RES_CREATE_3D)g.q->AllocCmd(&vbuf, sizeof(*cmd));
    if (!cmd)
    {
        g.idr->PutId(r->id);
        FreeNP(r);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(cmd, sizeof(*cmd));
    cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    cmd->resource_id = r->id;
    cmd->target = p->target;
    cmd->format = p->format;
    cmd->bind = p->bind;
    cmd->width = p->width;
    cmd->height = p->height;
    cmd->depth = p->depth;
    cmd->array_size = p->array_size;
    cmd->last_level = p->last_level;
    cmd->nr_samples = p->nr_samples;
    cmd->flags = p->flags;
    NTSTATUS st = SyncSimple(vbuf);
    if (!NT_SUCCESS(st))
    {
        g.idr->PutId(r->id);
        FreeNP(r);
        return st;
    }

    if (p->size)
    {
        GPU_MEM_ENTRY *ents = NULL;
        ULONG nents = 0;
        r->guest_mdl = AllocGuestPages((SIZE_T)p->size);
        if (r->guest_mdl)
        {
            ents = BuildMemEntries(r->guest_mdl, &nents);
        }
        PGPU_RES_ATTACH_BACKING ab = NULL;
        PGPU_VBUFFER av = NULL;
        if (ents)
        {
            ab = (PGPU_RES_ATTACH_BACKING)g.q->AllocCmd(&av, sizeof(*ab));
        }
        if (!ab)
        {
            FreeNP(ents);
            ResDestroy(f, r); /* unrefs the host resource and frees the id */
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlZeroMemory(ab, sizeof(*ab));
        ab->hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
        ab->resource_id = r->id;
        ab->nr_entries = nents;
        av->data_buf = ents;
        av->data_size = nents * sizeof(GPU_MEM_ENTRY);
        st = SyncSimple(av);
        if (st == STATUS_IO_TIMEOUT)
        {
            return st;
        }
        FreeNP(ents);
        if (!NT_SUCCESS(st))
        {
            ResDestroy(f, r);
            return st;
        }
    }
    if (f->ctx)
    {
        /* Like Linux virtgpu: a 3D resource is attached to its creating context so that transfers work. */
        st = SendCtxResource(VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE, f->ctx->ctx_id, r->id);
        if (!NT_SUCCESS(st))
        {
            ResDestroy(f, r);
            return st;
        }
        r->ctx_attached = TRUE;
    }
    InsertTailList(&f->resources, &r->link);
    p->handle = r->id;
    return STATUS_SUCCESS;
}

static NTSTATUS DoMap(VGPU_FILE *f, VGPU_MAP *p)
{
    VGPU_RES *r = FindRes(f, p->handle);
    if (!r)
    {
        return STATUS_INVALID_HANDLE;
    }
    if (r->user_va)
    {
        if (r->user_proc != IoGetCurrentProcess())
        {
            return STATUS_ACCESS_DENIED; /* mapped in another process */
        }
        p->user_va = (ULONGLONG)r->user_va;
        p->size = r->size;
        return STATUS_SUCCESS;
    }

    PVOID va = NULL;
    NTSTATUS st;
    if (r->guest_mdl)
    {
        st = MapIntoUser(r, r->guest_mdl, MmCached, &va);
    }
    else if (r->blob_mem == VGPU_BLOB_MEM_HOST3D && (r->blob_flags & VGPU_BLOB_FLAG_USE_MAPPABLE))
    {
        if (!g.shm_size)
        {
            return STATUS_NOT_SUPPORTED;
        }
        if (!r->host_mapped)
        {
            ULONGLONG off;
            if (!ShmAlloc(r->size, &off))
            {
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            PGPU_VBUFFER vbuf;
            PVOID page = AllocNP(PAGE_SIZE);
            PGPU_RES_MAP_BLOB cmd = page ? (PGPU_RES_MAP_BLOB)g.q->AllocCmdResp(&vbuf, sizeof(*cmd), page, sizeof(GPU_RESP_MAP_INFO)) : NULL;
            if (!cmd)
            {
                FreeNP(page);
                ShmFree(off, r->size);
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            RtlZeroMemory(cmd, sizeof(*cmd));
            cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB;
            cmd->hdr.ctx_id = f->ctx ? f->ctx->ctx_id : 0;
            cmd->resource_id = r->id;
            cmd->offset = off;
            ULONG resp = 0;
            st = SyncSend(vbuf, V3D_SYNC_TIMEOUT_MS, &resp);
            if (st == STATUS_IO_TIMEOUT)
            {
                return st;
            }
            if (NT_SUCCESS(st))
            {
                st = RespToStatus(resp, VIRTIO_GPU_RESP_OK_MAP_INFO);
                if (NT_SUCCESS(st))
                {
                    r->map_info = ((PGPU_RESP_MAP_INFO)page)->map_info;
                }
                g.q->ReleaseBuffer(vbuf);
            }
            FreeNP(page);
            if (!NT_SUCCESS(st))
            {
                ShmFree(off, r->size);
                return st;
            }
            r->host_mapped = TRUE;
            r->shm_off = off;
        }
        if (!r->io_mdl)
        {
            MM_PHYSICAL_ADDRESS_LIST pl;
            pl.PhysicalAddress.QuadPart = g.shm_pa.QuadPart + (LONGLONG)r->shm_off;
            pl.NumberOfBytes = (SIZE_T)((r->size + PAGE_SIZE - 1) & ~(ULONGLONG)(PAGE_SIZE - 1));
            st = MmAllocateMdlForIoSpace(&pl, 1, &r->io_mdl);
            if (!NT_SUCCESS(st))
            {
                return st;
            }
        }
        MEMORY_CACHING_TYPE ct = MmNonCached;
        switch (r->map_info & VIRTIO_GPU_MAP_CACHE_MASK)
        {
            case VIRTIO_GPU_MAP_CACHE_CACHED:
                ct = MmCached;
                break;
            case VIRTIO_GPU_MAP_CACHE_WC:
                ct = MmWriteCombined;
                break;
            default:
                ct = MmNonCached;
                break;
        }
        st = MapIntoUser(r, r->io_mdl, ct, &va);
    }
    else
    {
        return STATUS_NOT_SUPPORTED; /* host-only memory that was not created mappable */
    }
    if (NT_SUCCESS(st))
    {
        p->user_va = (ULONGLONG)va;
        p->size = r->size;
    }
    return st;
}

static NTSTATUS DoTransfer(VGPU_FILE *f, VGPU_TRANSFER *p, BOOLEAN to_host)
{
    VGPU_RES *r = FindRes(f, p->handle);
    if (!r)
    {
        return STATUS_INVALID_HANDLE;
    }
    PGPU_VBUFFER vbuf;
    PGPU_TRANSFER_HOST_3D cmd = (PGPU_TRANSFER_HOST_3D)g.q->AllocCmd(&vbuf, sizeof(*cmd));
    if (!cmd)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(cmd, sizeof(*cmd));
    cmd->hdr.type = to_host ? VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D : VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D;
    cmd->hdr.ctx_id = f->ctx ? f->ctx->ctx_id : 0;
    cmd->box.x = p->box.x;
    cmd->box.y = p->box.y;
    cmd->box.z = p->box.z;
    cmd->box.w = p->box.w;
    cmd->box.h = p->box.h;
    cmd->box.d = p->box.d;
    cmd->offset = p->offset;
    cmd->resource_id = r->id;
    cmd->level = p->level;
    cmd->stride = p->stride;
    cmd->layer_stride = p->layer_stride;
    return SyncSimple(vbuf);
}

static NTSTATUS DoExecbuf(VGPU_FILE *f, VGPU_EXECBUFFER *eb, ULONG in_len)
{
    if (!f->ctx)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }
    ULONG nb = eb->num_bo_handles;
    ULONG sz = eb->size;
    if (nb > 4096 || sz > V3D_MAX_EXEC_BYTES)
    {
        return STATUS_INVALID_PARAMETER;
    }
    if ((ULONGLONG)sizeof(*eb) + 4ull * nb + sz > in_len)
    {
        return STATUS_BUFFER_TOO_SMALL;
    }
    ULONG slot = VGPU_MAX_RINGS;
    BOOLEAN ring = (eb->flags & VGPU_EXECBUF_RING_IDX) != 0;
    if (ring)
    {
        if (eb->ring_idx >= f->ctx->num_rings)
        {
            return STATUS_INVALID_PARAMETER;
        }
        slot = eb->ring_idx;
    }
    BOOLEAN fenced = (eb->flags & VGPU_EXECBUF_FENCE) != 0;

    UINT32 *handles = VGPU_EXECBUF_HANDLES(eb);
    for (ULONG i = 0; i < nb; ++i)
    {
        VGPU_RES *r = FindRes(f, handles[i]);
        if (!r)
        {
            return STATUS_INVALID_HANDLE;
        }
        if (!r->ctx_attached)
        {
            NTSTATUS st = SendCtxResource(VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE, f->ctx->ctx_id, r->id);
            if (!NT_SUCCESS(st))
            {
                return st;
            }
            r->ctx_attached = TRUE;
        }
    }

    PVOID payload = NULL;
    if (sz)
    {
        payload = AllocNP(sz);
        if (!payload)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlCopyMemory(payload, VGPU_EXECBUF_CMD(eb), sz);
    }
    PGPU_VBUFFER vbuf;
    PGPU_CMD_SUBMIT cmd = (PGPU_CMD_SUBMIT)g.q->AllocCmd(&vbuf, sizeof(*cmd));
    if (!cmd)
    {
        FreeNP(payload);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(cmd, sizeof(*cmd));
    cmd->hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D;
    cmd->hdr.ctx_id = f->ctx->ctx_id;
    cmd->size = sz;

    ULONGLONG fence = 0;
    if (fenced)
    {
        fence = (ULONGLONG)InterlockedIncrement64(&g.fence_counter);
        cmd->hdr.flags |= VIRTIO_GPU_FLAG_FENCE;
        cmd->hdr.fence_id = fence;
        if (ring)
        {
            cmd->hdr.flags |= VIRTIO_GPU_FLAG_INFO_RING_IDX;
            cmd->hdr.ring_idx = (UCHAR)slot;
        }
        KIRQL irql;
        KeAcquireSpinLock(&f->ctx->lock, &irql);
        f->ctx->submitted[slot] = fence;
        KeReleaseSpinLock(&f->ctx->lock, irql);
    }
    NTSTATUS st = SendAsync(vbuf, payload, sz, f->ctx, fenced, slot, fence);
    if (NT_SUCCESS(st))
    {
        eb->fence_id = fence;
    }
    return st;
}

/* ------------------------------------------------------------------------------------ */
/* per-handle lifetime                                                                  */
/* ------------------------------------------------------------------------------------ */

static VOID FileTeardown(VGPU_FILE *f)
{
    ExAcquireFastMutex(&f->lock);
    if (!f->dead)
    {
        f->dead = TRUE;
        ResDestroyAll(f);
        CtxDestroy(f);
    }
    ExReleaseFastMutex(&f->lock);
}

static NTSTATUS Complete(PIRP irp, NTSTATUS st, ULONG_PTR info)
{
    irp->IoStatus.Status = st;
    irp->IoStatus.Information = info;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return st;
}

static NTSTATUS V3dCreate(PIRP irp, PIO_STACK_LOCATION sp)
{
    if (!g.accepting)
    {
        return Complete(irp, STATUS_DEVICE_NOT_CONNECTED, 0);
    }
    VGPU_FILE *f = (VGPU_FILE *)AllocNP(sizeof(VGPU_FILE));
    if (!f)
    {
        return Complete(irp, STATUS_INSUFFICIENT_RESOURCES, 0);
    }
    ExInitializeFastMutex(&f->lock);
    InitializeListHead(&f->resources);
    f->owner = IoGetCurrentProcess();
    sp->FileObject->FsContext = f;
    ExAcquireFastMutex(&g.files_lock);
    if (!g.accepting)
    {
        ExReleaseFastMutex(&g.files_lock);
        FreeNP(f);
        return Complete(irp, STATUS_DEVICE_NOT_CONNECTED, 0);
    }
    InsertTailList(&g.files, &f->link);
    ExReleaseFastMutex(&g.files_lock);
    return Complete(irp, STATUS_SUCCESS, 0);
}

static NTSTATUS V3dCleanup(PIRP irp, PIO_STACK_LOCATION sp)
{
    VGPU_FILE *f = (VGPU_FILE *)sp->FileObject->FsContext;
    if (f)
    {
        FileTeardown(f);
    }
    return Complete(irp, STATUS_SUCCESS, 0);
}

static NTSTATUS V3dClose(PIRP irp, PIO_STACK_LOCATION sp)
{
    VGPU_FILE *f = (VGPU_FILE *)sp->FileObject->FsContext;
    if (f)
    {
        FileTeardown(f); /* no-op if cleanup already ran */
        ExAcquireFastMutex(&g.files_lock);
        RemoveEntryList(&f->link);
        ExReleaseFastMutex(&g.files_lock);
        sp->FileObject->FsContext = NULL;
        FreeNP(f);
    }
    return Complete(irp, STATUS_SUCCESS, 0);
}

static NTSTATUS V3dIoctl(PIRP irp, PIO_STACK_LOCATION sp)
{
    VGPU_FILE *f = (VGPU_FILE *)sp->FileObject->FsContext;
    ULONG code = sp->Parameters.DeviceIoControl.IoControlCode;
    ULONG in_len = sp->Parameters.DeviceIoControl.InputBufferLength;
    ULONG out_len = sp->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID buf = irp->AssociatedIrp.SystemBuffer;
    ULONG_PTR info = 0;
    NTSTATUS st = STATUS_SUCCESS;

    if (!g.accepting || !f)
    {
        return Complete(irp, STATUS_DEVICE_NOT_CONNECTED, 0);
    }
    InterlockedIncrement(&g.inflight);
    if (!g.accepting || f->dead)
    {
        InterlockedDecrement(&g.inflight);
        return Complete(irp, STATUS_DEVICE_NOT_CONNECTED, 0);
    }
    if (!buf)
    {
        InterlockedDecrement(&g.inflight);
        return Complete(irp, STATUS_INVALID_PARAMETER, 0);
    }

#define NEED(T)                                                                                                        \
    if (in_len < sizeof(T) || out_len < sizeof(T))                                                                     \
    {                                                                                                                  \
        st = STATUS_BUFFER_TOO_SMALL;                                                                                  \
        break;                                                                                                         \
    }                                                                                                                  \
    T *p = (T *)buf;                                                                                                   \
    info = sizeof(T)

    BOOLEAN file_locked = FALSE;
    if (code != IOCTL_VGPU_WAIT_FENCE)
    {
        ExAcquireFastMutex(&f->lock);
        file_locked = TRUE;
        if (f->dead) /* re-check under the lock: cleanup may have run while we waited */
        {
            ExReleaseFastMutex(&f->lock);
            InterlockedDecrement(&g.inflight);
            return Complete(irp, STATUS_DEVICE_NOT_CONNECTED, 0);
        }
    }

    switch (code)
    {
        case IOCTL_VGPU_GETPARAM:
            {
                NEED(VGPU_GETPARAM);
                st = DoGetParam(p);
                break;
            }
        case IOCTL_VGPU_GET_CAPSET_INFO:
            {
                NEED(VGPU_GET_CAPSET_INFO);
                if (p->index >= g.num_capsets)
                {
                    st = STATUS_NO_MORE_ENTRIES;
                    break;
                }
                p->capset_id = g.caps[p->index].id;
                p->max_version = g.caps[p->index].max_version;
                p->max_size = g.caps[p->index].max_size;
                break;
            }
        case IOCTL_VGPU_GET_CAPS:
            {
                NEED(VGPU_GET_CAPS);
                ULONG avail = min(p->size, out_len - (ULONG)sizeof(*p));
                ULONG written = 0;
                st = DoGetCaps(p, avail, &written);
                info = sizeof(*p) + written;
                break;
            }
        case IOCTL_VGPU_CONTEXT_INIT:
            {
                NEED(VGPU_CONTEXT_INIT);
                st = DoContextInit(f, p);
                break;
            }
        case IOCTL_VGPU_RESOURCE_CREATE_BLOB:
            {
                NEED(VGPU_RESOURCE_CREATE_BLOB);
                st = DoCreateBlob(f, p, in_len);
                break;
            }
        case IOCTL_VGPU_RESOURCE_CREATE:
            {
                NEED(VGPU_RESOURCE_CREATE);
                st = DoCreateRes3D(f, p);
                break;
            }
        case IOCTL_VGPU_RESOURCE_INFO:
            {
                NEED(VGPU_RESOURCE_INFO);
                VGPU_RES *r = FindRes(f, p->handle);
                if (r)
                {
                    p->res_handle = r->id;
                    p->size = r->size;
                    p->blob_mem = r->blob_mem;
                }
                else
                {
                    st = STATUS_INVALID_HANDLE;
                }
                break;
            }
        case IOCTL_VGPU_MAP:
            {
                NEED(VGPU_MAP);
                st = DoMap(f, p);
                break;
            }
        case IOCTL_VGPU_UNMAP:
            {
                NEED(VGPU_HANDLE);
                VGPU_RES *r = FindRes(f, p->handle);
                if (r && r->user_va && r->user_proc != IoGetCurrentProcess())
                {
                    st = STATUS_ACCESS_DENIED;
                }
                else if (r)
                {
                    UnmapUser(r);
                }
                else
                {
                    st = STATUS_INVALID_HANDLE;
                }
                break;
            }
        case IOCTL_VGPU_RESOURCE_CLOSE:
            {
                NEED(VGPU_HANDLE);
                VGPU_RES *r = FindRes(f, p->handle);
                if (r)
                {
                    RemoveEntryList(&r->link);
                    ResDestroy(f, r);
                }
                else
                {
                    st = STATUS_INVALID_HANDLE;
                }
                break;
            }
        case IOCTL_VGPU_TRANSFER_TO_HOST:
        case IOCTL_VGPU_TRANSFER_FROM_HOST:
            {
                NEED(VGPU_TRANSFER);
                st = DoTransfer(f, p, code == IOCTL_VGPU_TRANSFER_TO_HOST);
                break;
            }
        case IOCTL_VGPU_EXECBUFFER:
            {
                NEED(VGPU_EXECBUFFER);
                st = DoExecbuf(f, p, in_len);
                break;
            }
        case IOCTL_VGPU_WAIT_FENCE:
            {
                NEED(VGPU_WAIT_FENCE);
                VGPU_CTX *c = NULL;
                ExAcquireFastMutex(&f->lock);
                if (f->ctx && !f->dead)
                {
                    c = f->ctx;
                    CtxRef(c);
                }
                ExReleaseFastMutex(&f->lock);
                if (!c)
                {
                    st = STATUS_INVALID_DEVICE_STATE;
                    break;
                }
                ULONG slot = (p->ring_idx == VGPU_RING_NONE) ? VGPU_MAX_RINGS : p->ring_idx;
                if (slot > VGPU_MAX_RINGS)
                {
                    CtxDeref(c);
                    st = STATUS_INVALID_PARAMETER;
                    break;
                }
                ULONG ms = (p->timeout_ms == 0xFFFFFFFFu || p->timeout_ms > 60000) ? 60000 : p->timeout_ms;
                NTSTATUS w = CtxWait(c, slot, p->fence_id, ms);
                p->signaled = (w == STATUS_SUCCESS);
                st = (w == STATUS_TIMEOUT) ? STATUS_SUCCESS : w;
                CtxDeref(c);
                break;
            }
        case IOCTL_VGPU_QUERY_FENCE:
            {
                NEED(VGPU_QUERY_FENCE);
                VGPU_CTX *c = f->ctx;
                ULONG slot = (p->ring_idx == VGPU_RING_NONE) ? VGPU_MAX_RINGS : p->ring_idx;
                if (!c)
                {
                    st = STATUS_INVALID_DEVICE_STATE;
                }
                else if (slot > VGPU_MAX_RINGS)
                {
                    st = STATUS_INVALID_PARAMETER;
                }
                else
                {
                    KIRQL irql;
                    KeAcquireSpinLock(&c->lock, &irql);
                    p->submitted = c->submitted[slot];
                    p->completed = c->completed[slot];
                    KeReleaseSpinLock(&c->lock, irql);
                }
                break;
            }
        default:
            st = STATUS_INVALID_DEVICE_REQUEST;
            break;
    }
#undef NEED
    if (file_locked)
    {
        ExReleaseFastMutex(&f->lock);
    }
    InterlockedDecrement(&g.inflight);
    return Complete(irp, st, NT_SUCCESS(st) ? info : 0);
}

/* ------------------------------------------------------------------------------------ */
/* dispatch hook (the display miniport framework owns the driver's dispatch table)       */
/* ------------------------------------------------------------------------------------ */

static NTSTATUS V3dDispatch(PDEVICE_OBJECT dev, PIRP irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(irp);
    UCHAR mj = sp->MajorFunction;

    if (dev != g.dev || g.dev == NULL)
    {
        PDRIVER_DISPATCH orig = (mj <= IRP_MJ_MAXIMUM_FUNCTION) ? g.orig[mj] : NULL;
        if (orig)
        {
            return orig(dev, irp);
        }
        return Complete(irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
    switch (mj)
    {
        case IRP_MJ_CREATE:
            return V3dCreate(irp, sp);
        case IRP_MJ_CLEANUP:
            return V3dCleanup(irp, sp);
        case IRP_MJ_CLOSE:
            return V3dClose(irp, sp);
        case IRP_MJ_DEVICE_CONTROL:
            return V3dIoctl(irp, sp);
        default:
            return Complete(irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
}

/* DriverEntry: only record the driver object and initialise state.  The dispatch table is
 * patched later (InstallDispatch, from VGpu3DInit) so that dxgkrnl has certainly finished
 * populating it first. */
extern "C" VOID VGpu3DHookDispatch(_In_ PDRIVER_OBJECT DriverObject)
{
    if (g.hooked)
    {
        return;
    }
    RtlZeroMemory(&g, sizeof(g));
    ExInitializeFastMutex(&g.files_lock);
    InitializeListHead(&g.files);
    InitializeListHead(&g.shm_free);
    KeInitializeSpinLock(&g.shm_lock);
    g.drv = DriverObject;
    g.hooked = TRUE;
}

static VOID InstallDispatch(VOID)
{
    static const UCHAR hook[] = {IRP_MJ_CREATE, IRP_MJ_CLOSE, IRP_MJ_CLEANUP, IRP_MJ_DEVICE_CONTROL};
    if (g.dispatch_installed)
    {
        return;
    }
    for (ULONG i = 0; i < ARRAYSIZE(hook); ++i)
    {
        PDRIVER_DISPATCH cur = g.drv->MajorFunction[hook[i]];
        if (cur != V3dDispatch)
        {
            g.orig[hook[i]] = cur;
            g.drv->MajorFunction[hook[i]] = V3dDispatch;
        }
    }
    g.dispatch_installed = TRUE;
}

/* ------------------------------------------------------------------------------------ */
/* init / shutdown                                                                      */
/* ------------------------------------------------------------------------------------ */

NTSTATUS VGpu3DInit(_In_ VioGpuAdapter *Adapter)
{
    if (!g.hooked || g.accepting)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }
    UINT64 feat = Adapter->GetGuestFeatures();
    g.virgl = virtio_is_feature_enabled(feat, VIRTIO_GPU_F_VIRGL) ? TRUE : FALSE;
    g.blob = virtio_is_feature_enabled(feat, VIRTIO_GPU_F_RESOURCE_BLOB) ? TRUE : FALSE;
    g.ctxinit = virtio_is_feature_enabled(feat, VIRTIO_GPU_F_CONTEXT_INIT) ? TRUE : FALSE;
    V3D_LOG("features: virgl=%d blob=%d context_init=%d\n", g.virgl, g.blob, g.ctxinit);
    if (!g.virgl)
    {
        V3D_LOG("host does not offer VIRGL: 3D control device not created\n");
        return STATUS_NOT_SUPPORTED;
    }
    g.adapter = Adapter;
    g.q = Adapter->GetCtrlQueue();
    g.idr = Adapter->GetIdr();
    g.host_gone = FALSE;

    g.num_capsets = 0;
    QueryCapsets(Adapter->GetNumCapsets());

    if (g.blob && FindHostVisibleShm(Adapter, &g.shm_pa, &g.shm_size))
    {
        SHM_FREE *n = (SHM_FREE *)AllocNP(sizeof(SHM_FREE));
        if (n)
        {
            n->off = 0;
            n->len = g.shm_size & ~(ULONGLONG)(PAGE_SIZE - 1);
            InsertTailList(&g.shm_free, &n->link);
        }
        V3D_LOG("host visible region: pa=0x%llx size=0x%llx\n", g.shm_pa.QuadPart, g.shm_size);
    }
    else
    {
        g.shm_size = 0;
        V3D_LOG("no host visible region (blob=%d)\n", g.blob);
    }

    if (g.dev)
    {
        /* restart after a stop: the control device lives for the driver's lifetime */
        g.accepting = TRUE;
        V3D_LOG("control device re-attached (\\\\.\\VGpu3D), %lu capsets\n", g.num_capsets);
        return STATUS_SUCCESS;
    }

    UNICODE_STRING name = RTL_CONSTANT_STRING(VGPU_DEVICE_NT_NAME);
    UNICODE_STRING link = RTL_CONSTANT_STRING(VGPU_DEVICE_DOS_NAME);
    UNICODE_STRING sddl = RTL_CONSTANT_STRING(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;WD)(A;;GRGW;;;AC)");
    NTSTATUS st = IoCreateDeviceSecure(g.drv,
                                       0,
                                       &name,
                                       FILE_DEVICE_UNKNOWN,
                                       FILE_DEVICE_SECURE_OPEN,
                                       FALSE,
                                       &sddl,
                                       &VGPU_CLASS_GUID,
                                       &g.dev);
    if (!NT_SUCCESS(st))
    {
        V3D_LOG("IoCreateDeviceSecure failed 0x%x\n", st);
        g.dev = NULL;
        return st;
    }
    g.dev->Flags |= DO_BUFFERED_IO;
    st = IoCreateSymbolicLink(&link, &name);
    if (!NT_SUCCESS(st))
    {
        V3D_LOG("IoCreateSymbolicLink failed 0x%x\n", st);
        IoDeleteDevice(g.dev);
        g.dev = NULL;
        return st;
    }
    g.link_created = TRUE;
    InstallDispatch();
    g.accepting = TRUE;
    g.dev->Flags &= ~DO_DEVICE_INITIALIZING;
    V3D_LOG("control device ready (\\\\.\\VGpu3D), %lu capsets\n", g.num_capsets);
    return STATUS_SUCCESS;
}

VOID VGpu3DShutdown(VOID)
{
    if (!g.dev || !g.accepting)
    {
        return;
    }
    /* stop new work, then let running IOCTLs drain before touching shared state */
    ExAcquireFastMutex(&g.files_lock);
    g.accepting = FALSE;
    ExReleaseFastMutex(&g.files_lock);
    {
        LARGE_INTEGER d;
        d.QuadPart = -10000; /* 1 ms */
        for (int i = 0; i < 20000 && g.inflight != 0; ++i)
        {
            KeDelayExecutionThread(KernelMode, FALSE, &d);
        }
    }
    /* the device reset that follows drops every host context/resource: free guest-side state
     * only (sending UNMAP/UNREF/CTX_DESTROY now just times out, 5 s each) */
    g.host_gone = TRUE;

    /* Tear down handles that are still open (their processes are alive: attach to unmap).
     * files_lock is held throughout so V3dClose cannot free a file under us.  The handles stay
     * valid but dead: every further IOCTL fails with STATUS_DEVICE_NOT_CONNECTED. */
    ExAcquireFastMutex(&g.files_lock);
    for (PLIST_ENTRY e = g.files.Flink; e != &g.files; e = e->Flink)
    {
        FileTeardown(CONTAINING_RECORD(e, VGPU_FILE, link));
    }
    ExReleaseFastMutex(&g.files_lock);

    /* \Device\VGpu3D and its link stay for the driver's lifetime (VGpu3DUnload): deleting
     * them while handles are open made the name unusable for the next start. */
    while (!IsListEmpty(&g.shm_free))
    {
        FreeNP(CONTAINING_RECORD(RemoveHeadList(&g.shm_free), SHM_FREE, link));
    }
    g.adapter = NULL;
    g.q = NULL;
    g.idr = NULL;
}

VOID VGpu3DUnload(VOID)
{
    VGpu3DShutdown();
    if (g.link_created)
    {
        UNICODE_STRING link = RTL_CONSTANT_STRING(VGPU_DEVICE_DOS_NAME);
        IoDeleteSymbolicLink(&link);
        g.link_created = FALSE;
    }
    if (g.dev)
    {
        IoDeleteDevice(g.dev); /* deferred by the I/O manager while handles remain */
        g.dev = NULL;
    }
}
