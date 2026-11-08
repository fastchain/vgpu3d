/*
 * dmltest - DirectML smoke test: creates a D3D12 device on the default adapter, a DirectML device on
 * it (System32 DirectML.dll), runs one element-wise add of two float tensors and checks the result.
 * Build: build/build_tests.sh (MinGW, DirectML.h from the Windows SDK in toolchain/).
 * SPDX-License-Identifier: MIT
 */
#define DML_TARGET_VERSION 0x6000
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
/* SAL annotations DirectML.h uses that MinGW's sal.h lacks */
#ifndef _Maybenull_
#define _Maybenull_
#endif
#include <DirectML.h>
#include <stdio.h>
#include <stdint.h>

/* MinGW's __uuidof needs the interface IDs declared; values from DirectML.h */
__CRT_UUID_DECL(IDMLDevice, 0x6dbd6437, 0x96fd, 0x423f, 0xa9, 0x8c, 0xae, 0x5e, 0x7c, 0x2a, 0x57, 0x3f)
__CRT_UUID_DECL(IDMLOperator, 0x26caae7a, 0x3081, 0x4633, 0x95, 0x81, 0x22, 0x6f, 0xbe, 0x57, 0x69, 0x5d)
__CRT_UUID_DECL(IDMLCompiledOperator, 0x6b15e56a, 0xbf5c, 0x4902, 0x92, 0xd8, 0xda, 0x3a, 0x65, 0x0a, 0xfe, 0xa4)
__CRT_UUID_DECL(IDMLOperatorInitializer, 0x427c1113, 0x435c, 0x469c, 0x86, 0x76, 0x4d, 0x5d, 0xd0, 0x72, 0xf8, 0x13)
__CRT_UUID_DECL(IDMLBindingTable, 0x29c687dc, 0xde74, 0x4e3b, 0xab, 0x00, 0x11, 0x68, 0xf2, 0xfc, 0x3c, 0xfc)
__CRT_UUID_DECL(IDMLCommandRecorder, 0xe6857a76, 0x2e3e, 0x4fdd, 0xbf, 0xf4, 0x5d, 0x2b, 0xa1, 0x0f, 0xb4, 0x53)

#define CHECK(x)                                                                    \
    do {                                                                            \
        HRESULT hr_ = (x);                                                          \
        if (FAILED(hr_)) {                                                          \
            printf("[FAIL] %s = 0x%08lx (line %d)\n", #x, (unsigned long)hr_, __LINE__); \
            return 1;                                                               \
        }                                                                           \
    } while (0)

/* GetBindingProperties returns a 24-byte struct: MSVC passes the result pointer after `this`, MinGW's g++
 * before it, so call the vtable slot (IUnknown 3 + IDMLObject 4 + IDMLDeviceChild 1 = index 8) directly. */
static DML_BINDING_PROPERTIES
binding_properties(IDMLDispatchable *d)
{
    typedef DML_BINDING_PROPERTIES *(STDMETHODCALLTYPE * PFN)(IDMLDispatchable *, DML_BINDING_PROPERTIES *);
    DML_BINDING_PROPERTIES r = {};
    ((PFN)(*(void ***)d)[8])(d, &r);
    return r;
}

typedef HRESULT(WINAPI *PFN_DMLCreateDevice)(ID3D12Device *, DML_CREATE_DEVICE_FLAGS, REFIID, void **);

static ID3D12Resource *
buffer(ID3D12Device *dev, D3D12_HEAP_TYPE heap, UINT64 size, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = heap;
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = flags;
    ID3D12Resource *r = NULL;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, NULL, IID_PPV_ARGS(&r))))
        return NULL;
    return r;
}

static void
barrier(ID3D12GraphicsCommandList *cl, ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    cl->ResourceBarrier(1, &b);
}

int
main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    enum { N = 1 << 16 };
    const UINT64 bytes = N * sizeof(float);

    ID3D12Device *dev = NULL;
    CHECK(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)));
    LUID luid = dev->GetAdapterLuid();
    IDXGIFactory4 *fac = NULL;
    IDXGIAdapter1 *ad = NULL;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&fac))) && SUCCEEDED(fac->EnumAdapterByLuid(luid, IID_PPV_ARGS(&ad)))) {
        DXGI_ADAPTER_DESC1 ds;
        ad->GetDesc1(&ds);
        printf("[INFO] D3D12 adapter: %ls\n", ds.Description);
    }

    HMODULE dml = LoadLibraryA("DirectML.dll");
    PFN_DMLCreateDevice create = dml ? (PFN_DMLCreateDevice)GetProcAddress(dml, "DMLCreateDevice") : NULL;
    if (!create) {
        printf("[FAIL] DirectML.dll / DMLCreateDevice not found\n");
        return 1;
    }
    IDMLDevice *dmlDev = NULL;
    CHECK(create(dev, DML_CREATE_DEVICE_FLAG_NONE, IID_PPV_ARGS(&dmlDev)));
    printf("[PASS] DMLCreateDevice\n");

    /* A + B -> C, float32 tensors of shape [1,1,1,N] */
    UINT sizes[4] = { 1, 1, 1, N };
    DML_BUFFER_TENSOR_DESC btd = {};
    btd.DataType = DML_TENSOR_DATA_TYPE_FLOAT32;
    btd.DimensionCount = 4;
    btd.Sizes = sizes;
    btd.TotalTensorSizeInBytes = bytes;
    DML_TENSOR_DESC td = { DML_TENSOR_TYPE_BUFFER, &btd };
    DML_ELEMENT_WISE_ADD_OPERATOR_DESC add = { &td, &td, &td };
    DML_OPERATOR_DESC od = { DML_OPERATOR_ELEMENT_WISE_ADD, &add };
    IDMLOperator *op = NULL;
    CHECK(dmlDev->CreateOperator(&od, IID_PPV_ARGS(&op)));
    IDMLCompiledOperator *cop = NULL;
    CHECK(dmlDev->CompileOperator(op, DML_EXECUTION_FLAG_NONE, IID_PPV_ARGS(&cop)));
    IDMLOperatorInitializer *init = NULL;
    IDMLCompiledOperator *ops[] = { cop };
    CHECK(dmlDev->CreateOperatorInitializer(1, ops, IID_PPV_ARGS(&init)));

    DML_BINDING_PROPERTIES ip = binding_properties(init), ep = binding_properties(cop);
    UINT descs = ip.RequiredDescriptorCount > ep.RequiredDescriptorCount ? ip.RequiredDescriptorCount : ep.RequiredDescriptorCount;
    UINT64 temp = ip.TemporaryResourceSize > ep.TemporaryResourceSize ? ip.TemporaryResourceSize : ep.TemporaryResourceSize;
    UINT64 persist = ep.PersistentResourceSize;
    printf("[INFO] descriptors=%u temp=%llu persistent=%llu\n", descs, (unsigned long long)temp, (unsigned long long)persist);

    ID3D12CommandQueue *q = NULL;
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    CHECK(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)));
    ID3D12CommandAllocator *alloc = NULL;
    CHECK(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)));
    ID3D12GraphicsCommandList *cl = NULL;
    CHECK(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, IID_PPV_ARGS(&cl)));
    ID3D12DescriptorHeap *heap = NULL;
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = descs ? descs : 1;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    CHECK(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));

    const D3D12_RESOURCE_FLAGS uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource *a = buffer(dev, D3D12_HEAP_TYPE_DEFAULT, bytes, uav, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *b = buffer(dev, D3D12_HEAP_TYPE_DEFAULT, bytes, uav, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *c = buffer(dev, D3D12_HEAP_TYPE_DEFAULT, bytes, uav, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource *up = buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 2 * bytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *rb = buffer(dev, D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *tmp = temp ? buffer(dev, D3D12_HEAP_TYPE_DEFAULT, temp, uav, D3D12_RESOURCE_STATE_UNORDERED_ACCESS) : NULL;
    ID3D12Resource *per = persist ? buffer(dev, D3D12_HEAP_TYPE_DEFAULT, persist, uav, D3D12_RESOURCE_STATE_UNORDERED_ACCESS) : NULL;
    if (!a || !b || !c || !up || !rb || (temp && !tmp) || (persist && !per)) {
        printf("[FAIL] buffer creation\n");
        return 1;
    }
    float *p = NULL;
    CHECK(up->Map(0, NULL, (void **)&p));
    for (int i = 0; i < N; i++) {
        p[i] = (float)i;
        p[N + i] = 0.5f * i;
    }
    up->Unmap(0, NULL);
    cl->CopyBufferRegion(a, 0, up, 0, bytes);
    cl->CopyBufferRegion(b, 0, up, bytes, bytes);
    barrier(cl, a, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    barrier(cl, b, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ID3D12DescriptorHeap *heaps[] = { heap };
    cl->SetDescriptorHeaps(1, heaps);
    IDMLCommandRecorder *rec = NULL;
    CHECK(dmlDev->CreateCommandRecorder(IID_PPV_ARGS(&rec)));

    DML_BINDING_TABLE_DESC tdsc = { init, heap->GetCPUDescriptorHandleForHeapStart(),
                                    heap->GetGPUDescriptorHandleForHeapStart(), descs };
    IDMLBindingTable *bt = NULL;
    CHECK(dmlDev->CreateBindingTable(&tdsc, IID_PPV_ARGS(&bt)));
    DML_BUFFER_BINDING tmpb = { tmp, 0, temp }, perb = { per, 0, persist };
    DML_BINDING_DESC tmpd = { DML_BINDING_TYPE_BUFFER, &tmpb }, perd = { DML_BINDING_TYPE_BUFFER, &perb };
    if (ip.TemporaryResourceSize)
        bt->BindTemporaryResource(&tmpd);
    if (persist)
        bt->BindOutputs(1, &perd); /* the initializer's output is the persistent resource */
    rec->RecordDispatch(cl, init, bt);

    D3D12_RESOURCE_BARRIER ub = {};
    ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    cl->ResourceBarrier(1, &ub);

    tdsc.Dispatchable = cop;
    CHECK(bt->Reset(&tdsc));
    DML_BUFFER_BINDING ab = { a, 0, bytes }, bb = { b, 0, bytes }, cb = { c, 0, bytes };
    DML_BINDING_DESC in[2] = { { DML_BINDING_TYPE_BUFFER, &ab }, { DML_BINDING_TYPE_BUFFER, &bb } };
    DML_BINDING_DESC out = { DML_BINDING_TYPE_BUFFER, &cb };
    bt->BindInputs(2, in);
    bt->BindOutputs(1, &out);
    if (ep.TemporaryResourceSize)
        bt->BindTemporaryResource(&tmpd);
    if (persist)
        bt->BindPersistentResource(&perd);
    rec->RecordDispatch(cl, cop, bt);

    cl->ResourceBarrier(1, &ub);
    barrier(cl, c, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cl->CopyBufferRegion(rb, 0, c, 0, bytes);
    CHECK(cl->Close());

    ID3D12Fence *fence = NULL;
    CHECK(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    ID3D12CommandList *lists[] = { cl };
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    q->ExecuteCommandLists(1, lists);
    CHECK(q->Signal(fence, 1));
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    CHECK(fence->SetEventOnCompletion(1, ev));
    if (WaitForSingleObject(ev, 10000) != WAIT_OBJECT_0) {
        printf("[FAIL] GPU work did not finish in 10 s (device removed: 0x%08lx)\n", (unsigned long)dev->GetDeviceRemovedReason());
        return 1;
    }
    QueryPerformanceCounter(&t1);

    float *r = NULL;
    D3D12_RANGE range = { 0, (SIZE_T)bytes };
    CHECK(rb->Map(0, &range, (void **)&r));
    int bad = 0;
    for (int i = 0; i < N; i++)
        if (r[i] != 1.5f * i && bad++ < 3)
            printf("[INFO] c[%d] = %f, expected %f\n", i, r[i], 1.5f * i);
    rb->Unmap(0, NULL);
    printf("[%s] DirectML element-wise add of %d floats: %d mismatches (%.2f ms)\n", bad ? "FAIL" : "PASS", N, bad,
           (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
    return bad ? 1 : 0;
}
