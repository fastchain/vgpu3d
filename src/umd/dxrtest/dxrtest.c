/* dxrtest: DirectX Raytracing (DXR 1.0 and 1.1) test through the system D3D12 runtime.
 *
 * Same scene and rays as rttest (Vulkan): a BLAS with one quad, a TLAS with two instances of it at
 * different depths, a 64x64 grid of rays along +Z.  Two paths, each checked ray by ray:
 *   1. a ray tracing pipeline state object (DXIL library, hit group, shader/pipeline config, global root
 *      signature, and an empty local root signature associated with the hit group) + DispatchRays;
 *   2. DXR 1.1 inline RayQuery in a compute shader, the TLAS bound through an acceleration-structure SRV.
 *
 * usage: dxrtest.exe    exit 0 = both correct, 1 = mismatch, 2 = setup failure / DXR not supported
 */
#define COBJMACROS
#define INITGUID
#define WIDL_C_INLINE_WRAPPERS
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <d3d12.h>

#include "dxrtest_dxil.h"

#define CHECK(x)                                                                 \
    do {                                                                         \
        HRESULT hr_ = (x);                                                       \
        if (FAILED(hr_)) {                                                       \
            printf("[FAIL] %s = 0x%08lx (line %d)\n", #x, hr_, __LINE__);        \
            exit(2);                                                             \
        }                                                                        \
    } while (0)

#define N 64

static ID3D12Device5 *dev;

static const float inst_off[2][3] = { { -0.4f, -0.4f, 1.0f }, { 0.4f, 0.4f, 2.0f } };
static const UINT inst_id[2] = { 7, 9 };

static ID3D12Resource *
make_buf(UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp = { heap };
    D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, size, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 },
                               D3D12_TEXTURE_LAYOUT_ROW_MAJOR, flags };
    ID3D12Resource *r;
    CHECK(ID3D12Device5_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, state, NULL,
                                                &IID_ID3D12Resource, (void **)&r));
    return r;
}

static void *
map(ID3D12Resource *r)
{
    void *p;
    D3D12_RANGE none = { 0, 0 };
    CHECK(ID3D12Resource_Map(r, 0, &none, &p));
    return p;
}

static ID3D12RootSignature *
root_sig(const D3D12_ROOT_SIGNATURE_DESC *d)
{
    ID3DBlob *blob, *err = NULL;
    HRESULT hr = D3D12SerializeRootSignature(d, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    if (FAILED(hr)) {
        printf("[FAIL] serialize root signature: %s\n", err ? (char *)ID3D10Blob_GetBufferPointer(err) : "?");
        exit(2);
    }
    ID3D12RootSignature *rs;
    CHECK(ID3D12Device5_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob),
                                            ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature,
                                            (void **)&rs));
    ID3D10Blob_Release(blob);
    return rs;
}

static void
uav_barrier(ID3D12GraphicsCommandList4 *cl, ID3D12Resource *r)
{
    D3D12_RESOURCE_BARRIER b = { D3D12_RESOURCE_BARRIER_TYPE_UAV };
    b.UAV.pResource = r;
    ID3D12GraphicsCommandList4_ResourceBarrier(cl, 1, &b);
}

static void
transition(ID3D12GraphicsCommandList4 *cl, ID3D12Resource *r, D3D12_RESOURCE_STATES from,
           D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    ID3D12GraphicsCommandList4_ResourceBarrier(cl, 1, &b);
}

static ID3D12CommandQueue *queue;
static ID3D12CommandAllocator *alloc;
static ID3D12GraphicsCommandList4 *cl;
static ID3D12Fence *fence;
static UINT64 fence_value;

static void
submit_and_wait(void)
{
    CHECK(ID3D12GraphicsCommandList4_Close(cl));
    ID3D12CommandList *lists[] = { (ID3D12CommandList *)cl };
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    CHECK(ID3D12CommandQueue_Signal(queue, fence, ++fence_value));
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    CHECK(ID3D12Fence_SetEventOnCompletion(fence, fence_value, ev));
    if (WaitForSingleObject(ev, 10000) != WAIT_OBJECT_0) {
        printf("[FAIL] GPU timeout (device removed reason 0x%08lx)\n", ID3D12Device5_GetDeviceRemovedReason(dev));
        exit(2);
    }
    CloseHandle(ev);
    CHECK(ID3D12CommandAllocator_Reset(alloc));
    CHECK(ID3D12GraphicsCommandList4_Reset(cl, alloc, NULL));
}

static int
verify(const char *name, const UINT *px)
{
    int bad = 0;
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            float ox = -1.0f + (x + 0.5f) / 32.0f, oy = -1.0f + (y + 0.5f) / 32.0f;
            UINT want_id = 0xFFFF;
            float want_t = 0.0f;
            for (int i = 0; i < 2; i++)
                if (fabsf(ox - inst_off[i][0]) <= 0.5f && fabsf(oy - inst_off[i][1]) <= 0.5f) {
                    want_id = inst_id[i];
                    want_t = inst_off[i][2];
                    break;
                }
            UINT id = px[(y * N + x) * 2];
            float t;
            memcpy(&t, &px[(y * N + x) * 2 + 1], sizeof(t));
            if (!(id == want_id && (want_id == 0xFFFF || fabsf(t - want_t) < 1e-3f)) && bad++ < 5)
                printf("  %s ray (%d,%d): got id %u t %g, want id %u t %g\n", name, x, y, id, t, want_id, want_t);
        }
    printf("[%s] %s: %d of %d rays correct\n", bad ? "FAIL" : "PASS", name, N * N - bad, N * N);
    return bad;
}

int
main(void)
{
    CHECK(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_12_0, &IID_ID3D12Device5, (void **)&dev));
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5 = { 0 };
    ID3D12Device5_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5));
    printf("RaytracingTier = 0x%x\n", o5.RaytracingTier);
    if (o5.RaytracingTier < D3D12_RAYTRACING_TIER_1_0) {
        printf("[FAIL] DXR not supported\n");
        return 2;
    }

    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    CHECK(ID3D12Device5_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue));
    CHECK(ID3D12Device5_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator,
                                               (void **)&alloc));
    CHECK(ID3D12Device5_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL,
                                          &IID_ID3D12GraphicsCommandList4, (void **)&cl));
    CHECK(ID3D12Device5_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence));

    /* --- acceleration structures --- */
    const float verts[] = { -0.5f, -0.5f, 0, 0.5f, -0.5f, 0, 0.5f, 0.5f, 0,
                            -0.5f, -0.5f, 0, 0.5f, 0.5f,  0, -0.5f, 0.5f, 0 };
    ID3D12Resource *vb = make_buf(sizeof(verts), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                  D3D12_RESOURCE_STATE_GENERIC_READ);
    memcpy(map(vb), verts, sizeof(verts));

    D3D12_RAYTRACING_GEOMETRY_DESC geo = { D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES,
                                           D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE };
    geo.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geo.Triangles.VertexCount = 6;
    geo.Triangles.VertexBuffer.StartAddress = ID3D12Resource_GetGPUVirtualAddress(vb);
    geo.Triangles.VertexBuffer.StrideInBytes = 12;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bin = {
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL,
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE, 1, D3D12_ELEMENTS_LAYOUT_ARRAY
    };
    bin.pGeometryDescs = &geo;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO bpi;
    ID3D12Device5_GetRaytracingAccelerationStructurePrebuildInfo(dev, &bin, &bpi);
    printf("BLAS prebuild: result %llu, scratch %llu\n", (unsigned long long)bpi.ResultDataMaxSizeInBytes,
           (unsigned long long)bpi.ScratchDataSizeInBytes);
    if (!bpi.ResultDataMaxSizeInBytes) {
        printf("[FAIL] empty prebuild info\n");
        return 2;
    }
    ID3D12Resource *blas = make_buf(bpi.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                    D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

    D3D12_RAYTRACING_INSTANCE_DESC insts[2];
    memset(insts, 0, sizeof(insts));
    for (int i = 0; i < 2; i++) {
        insts[i].Transform[0][0] = insts[i].Transform[1][1] = insts[i].Transform[2][2] = 1.0f;
        insts[i].Transform[0][3] = inst_off[i][0];
        insts[i].Transform[1][3] = inst_off[i][1];
        insts[i].Transform[2][3] = inst_off[i][2];
        insts[i].InstanceID = inst_id[i];
        insts[i].InstanceMask = 0xFF;
        insts[i].Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
        insts[i].AccelerationStructure = ID3D12Resource_GetGPUVirtualAddress(blas);
    }
    ID3D12Resource *ib = make_buf(sizeof(insts), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                  D3D12_RESOURCE_STATE_GENERIC_READ);
    memcpy(map(ib), insts, sizeof(insts));
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tin = bin;
    tin.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tin.NumDescs = 2;
    tin.InstanceDescs = ID3D12Resource_GetGPUVirtualAddress(ib);
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tpi;
    ID3D12Device5_GetRaytracingAccelerationStructurePrebuildInfo(dev, &tin, &tpi);
    ID3D12Resource *tlas = make_buf(tpi.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                    D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    UINT64 scratch_size = bpi.ScratchDataSizeInBytes > tpi.ScratchDataSizeInBytes ? bpi.ScratchDataSizeInBytes
                                                                                   : tpi.ScratchDataSizeInBytes;
    ID3D12Resource *scratch = make_buf(scratch_size, D3D12_HEAP_TYPE_DEFAULT,
                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd = { ID3D12Resource_GetGPUVirtualAddress(blas), bin, 0,
                                                              ID3D12Resource_GetGPUVirtualAddress(scratch) };
    ID3D12GraphicsCommandList4_BuildRaytracingAccelerationStructure(cl, &bd, 0, NULL);
    uav_barrier(cl, blas);
    uav_barrier(cl, scratch);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC td = { ID3D12Resource_GetGPUVirtualAddress(tlas), tin, 0,
                                                              ID3D12Resource_GetGPUVirtualAddress(scratch) };
    ID3D12GraphicsCommandList4_BuildRaytracingAccelerationStructure(cl, &td, 0, NULL);
    uav_barrier(cl, tlas);
    submit_and_wait();
    printf("acceleration structures built\n");

    /* --- output + readback --- */
    const UINT64 out_size = N * N * 8;
    ID3D12Resource *out = make_buf(out_size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource *rb = make_buf(out_size, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                                  D3D12_RESOURCE_STATE_COPY_DEST);
    UINT *rbp;
    D3D12_RANGE all = { 0, out_size };
    CHECK(ID3D12Resource_Map(rb, 0, &all, (void **)&rbp));

    /* global root signature: t0 = TLAS (root SRV), u0 = output (root UAV) */
    D3D12_ROOT_PARAMETER gp[2];
    memset(gp, 0, sizeof(gp));
    gp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    gp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    gp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    gp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC grd = { 2, gp };
    ID3D12RootSignature *grs = root_sig(&grd);
    D3D12_ROOT_SIGNATURE_DESC lrd = { 0, NULL, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE };
    ID3D12RootSignature *lrs = root_sig(&lrd);

    int bad = 0;

    /* --- 1: DXR pipeline --- */
    D3D12_DXIL_LIBRARY_DESC lib = { { dxr_lib_cso, sizeof(dxr_lib_cso) }, 0, NULL };
    D3D12_HIT_GROUP_DESC hg = { L"HitGroup", D3D12_HIT_GROUP_TYPE_TRIANGLES, NULL, L"Hit", NULL };
    D3D12_RAYTRACING_SHADER_CONFIG sc = { 8, 8 };
    D3D12_RAYTRACING_PIPELINE_CONFIG pc = { 1 };
    D3D12_GLOBAL_ROOT_SIGNATURE gsub = { grs };
    D3D12_LOCAL_ROOT_SIGNATURE lsub = { lrs };
    D3D12_STATE_SUBOBJECT sub[7] = {
        { D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &lib },
        { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hg },
        { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &sc },
        { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pc },
        { D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &gsub },
        { D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &lsub },
    };
    LPCWSTR hg_export = L"HitGroup";
    D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION assoc = { &sub[5], 1, &hg_export };
    sub[6].Type = D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION;
    sub[6].pDesc = &assoc;
    D3D12_STATE_OBJECT_DESC sod = { D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, 7, sub };
    ID3D12StateObject *so;
    CHECK(ID3D12Device5_CreateStateObject(dev, &sod, &IID_ID3D12StateObject, (void **)&so));
    ID3D12StateObjectProperties *props;
    CHECK(ID3D12StateObject_QueryInterface(so, &IID_ID3D12StateObjectProperties, (void **)&props));
    printf("state object created, pipeline stack size %llu\n",
           (unsigned long long)ID3D12StateObjectProperties_GetPipelineStackSize(props));

    /* shader table: raygen | miss | hit group, one 64-byte-aligned record each */
    const UINT rec = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;
    ID3D12Resource *sbt = make_buf(3 * rec, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                   D3D12_RESOURCE_STATE_GENERIC_READ);
    unsigned char *sbtp = map(sbt);
    LPCWSTR names[3] = { L"RayGen", L"Miss", L"HitGroup" };
    for (int i = 0; i < 3; i++) {
        void *id = ID3D12StateObjectProperties_GetShaderIdentifier(props, names[i]);
        if (!id) {
            printf("[FAIL] no shader identifier for %ls\n", names[i]);
            return 2;
        }
        memcpy(sbtp + i * rec, id, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    }
    D3D12_GPU_VIRTUAL_ADDRESS sbt_va = ID3D12Resource_GetGPUVirtualAddress(sbt);
    D3D12_DISPATCH_RAYS_DESC dr;
    memset(&dr, 0, sizeof(dr));
    dr.RayGenerationShaderRecord.StartAddress = sbt_va;
    dr.RayGenerationShaderRecord.SizeInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
    dr.MissShaderTable.StartAddress = sbt_va + rec;
    dr.MissShaderTable.SizeInBytes = rec;
    dr.MissShaderTable.StrideInBytes = rec;
    dr.HitGroupTable.StartAddress = sbt_va + 2 * rec;
    dr.HitGroupTable.SizeInBytes = rec;
    dr.HitGroupTable.StrideInBytes = rec;
    dr.Width = N;
    dr.Height = N;
    dr.Depth = 1;

    ID3D12GraphicsCommandList4_SetComputeRootSignature(cl, grs);
    ID3D12GraphicsCommandList4_SetComputeRootShaderResourceView(cl, 0, ID3D12Resource_GetGPUVirtualAddress(tlas));
    ID3D12GraphicsCommandList4_SetComputeRootUnorderedAccessView(cl, 1, ID3D12Resource_GetGPUVirtualAddress(out));
    ID3D12GraphicsCommandList4_SetPipelineState1(cl, so);
    ID3D12GraphicsCommandList4_DispatchRays(cl, &dr);
    transition(cl, out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12GraphicsCommandList4_CopyResource(cl, rb, out);
    transition(cl, out, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    submit_and_wait();
    bad += verify("DXR pipeline (DispatchRays)", rbp);

    /* --- 2: inline RayQuery, TLAS through a descriptor table --- */
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1,
                                      D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12DescriptorHeap *heap;
    CHECK(ID3D12Device5_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&heap));
    D3D12_SHADER_RESOURCE_VIEW_DESC srv;
    memset(&srv, 0, sizeof(srv));
    srv.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.RaytracingAccelerationStructure.Location = ID3D12Resource_GetGPUVirtualAddress(tlas);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu;
    cpu = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap);
    gpu = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(heap);
    ID3D12Device5_CreateShaderResourceView(dev, NULL, &srv, cpu);

    D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
    D3D12_ROOT_PARAMETER cp[2];
    memset(cp, 0, sizeof(cp));
    cp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    cp[0].DescriptorTable.NumDescriptorRanges = 1;
    cp[0].DescriptorTable.pDescriptorRanges = &range;
    cp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC crd = { 2, cp };
    ID3D12RootSignature *crs = root_sig(&crd);
    D3D12_COMPUTE_PIPELINE_STATE_DESC cpd;
    memset(&cpd, 0, sizeof(cpd));
    cpd.pRootSignature = crs;
    cpd.CS.pShaderBytecode = dxr_rq_cso;
    cpd.CS.BytecodeLength = sizeof(dxr_rq_cso);
    ID3D12PipelineState *pso;
    CHECK(ID3D12Device5_CreateComputePipelineState(dev, &cpd, &IID_ID3D12PipelineState, (void **)&pso));

    memset(rbp, 0xAB, out_size);
    ID3D12GraphicsCommandList4_SetDescriptorHeaps(cl, 1, &heap);
    ID3D12GraphicsCommandList4_SetComputeRootSignature(cl, crs);
    ID3D12GraphicsCommandList4_SetComputeRootDescriptorTable(cl, 0, gpu);
    ID3D12GraphicsCommandList4_SetComputeRootUnorderedAccessView(cl, 1, ID3D12Resource_GetGPUVirtualAddress(out));
    ID3D12GraphicsCommandList4_SetPipelineState(cl, pso);
    ID3D12GraphicsCommandList4_Dispatch(cl, N / 8, N / 8, 1);
    transition(cl, out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12GraphicsCommandList4_CopyResource(cl, rb, out);
    submit_and_wait();
    if (o5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1)
        bad += verify("DXR 1.1 inline RayQuery (compute)", rbp);
    else
        printf("[SKIP] inline RayQuery: tier 1.0\n");

    /* --- 3: compaction (post-build info + compacting copy), then trace again --- */
    {
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS cin = bin;
        cin.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO cpi;
        ID3D12Device5_GetRaytracingAccelerationStructurePrebuildInfo(dev, &cin, &cpi);
        ID3D12Resource *big = make_buf(cpi.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                       D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
        ID3D12Resource *info = make_buf(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12Resource *info_rb = make_buf(256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                                           D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC pb = {
            ID3D12Resource_GetGPUVirtualAddress(info),
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE
        };
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC cd = { ID3D12Resource_GetGPUVirtualAddress(big), cin, 0,
                                                                  ID3D12Resource_GetGPUVirtualAddress(scratch) };
        ID3D12GraphicsCommandList4_BuildRaytracingAccelerationStructure(cl, &cd, 1, &pb);
        uav_barrier(cl, big);
        /* the same size again through the separate Emit call, 8 bytes later */
        D3D12_GPU_VIRTUAL_ADDRESS src = ID3D12Resource_GetGPUVirtualAddress(big);
        pb.DestBuffer += 8;
        ID3D12GraphicsCommandList4_EmitRaytracingAccelerationStructurePostbuildInfo(cl, &pb, 1, &src);
        transition(cl, info, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        ID3D12GraphicsCommandList4_CopyResource(cl, info_rb, info);
        submit_and_wait();
        UINT64 *sizes;
        D3D12_RANGE r16 = { 0, 16 };
        CHECK(ID3D12Resource_Map(info_rb, 0, &r16, (void **)&sizes));
        printf("compaction: max %llu, compacted %llu (build) / %llu (emit)\n",
               (unsigned long long)cpi.ResultDataMaxSizeInBytes, (unsigned long long)sizes[0],
               (unsigned long long)sizes[1]);
        if (!sizes[0] || sizes[0] > cpi.ResultDataMaxSizeInBytes || sizes[0] != sizes[1]) {
            printf("[FAIL] compacted size\n");
            bad++;
        } else {
            ID3D12Resource *small = make_buf(sizes[0], D3D12_HEAP_TYPE_DEFAULT,
                                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                             D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
            ID3D12GraphicsCommandList4_CopyRaytracingAccelerationStructure(
                cl, ID3D12Resource_GetGPUVirtualAddress(small), src,
                D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT);
            uav_barrier(cl, small);
            /* TLAS over the compacted BLAS */
            ID3D12Resource *ib2 = make_buf(sizeof(insts), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                           D3D12_RESOURCE_STATE_GENERIC_READ);
            for (int i = 0; i < 2; i++)
                insts[i].AccelerationStructure = ID3D12Resource_GetGPUVirtualAddress(small);
            memcpy(map(ib2), insts, sizeof(insts));
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tin2 = tin;
            tin2.InstanceDescs = ID3D12Resource_GetGPUVirtualAddress(ib2);
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC td2 = { ID3D12Resource_GetGPUVirtualAddress(tlas), tin2,
                                                                       0, ID3D12Resource_GetGPUVirtualAddress(scratch) };
            ID3D12GraphicsCommandList4_BuildRaytracingAccelerationStructure(cl, &td2, 0, NULL);
            uav_barrier(cl, tlas);
            transition(cl, out, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ID3D12GraphicsCommandList4_SetComputeRootSignature(cl, grs);
            ID3D12GraphicsCommandList4_SetComputeRootShaderResourceView(cl, 0, ID3D12Resource_GetGPUVirtualAddress(tlas));
            ID3D12GraphicsCommandList4_SetComputeRootUnorderedAccessView(cl, 1, ID3D12Resource_GetGPUVirtualAddress(out));
            ID3D12GraphicsCommandList4_SetPipelineState1(cl, so);
            ID3D12GraphicsCommandList4_DispatchRays(cl, &dr);
            transition(cl, out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            memset(rbp, 0xAB, out_size);
            ID3D12GraphicsCommandList4_CopyResource(cl, rb, out);
            submit_and_wait();
            bad += verify("compacted BLAS (CopyRaytracingAccelerationStructure)", rbp);
        }
    }

    /* --- 4: AddToStateObject: raygen + miss first, hit group added later --- */
    {
        D3D12_STATE_OBJECT_CONFIG soc = { D3D12_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS };
        D3D12_EXPORT_DESC base_ex[2] = { { L"RayGen" }, { L"Miss" } };
        D3D12_DXIL_LIBRARY_DESC lib1 = { { dxr_lib_cso, sizeof(dxr_lib_cso) }, 2, base_ex };
        D3D12_STATE_SUBOBJECT s1[5] = {
            { D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &soc },
            { D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &lib1 },
            { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &sc },
            { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pc },
            { D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &gsub },
        };
        D3D12_STATE_OBJECT_DESC d1 = { D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, 5, s1 };
        ID3D12StateObject *so1, *so2;
        CHECK(ID3D12Device5_CreateStateObject(dev, &d1, &IID_ID3D12StateObject, (void **)&so1));
        D3D12_EXPORT_DESC hit_ex[1] = { { L"Hit" } };
        D3D12_DXIL_LIBRARY_DESC lib2 = { { dxr_lib_cso, sizeof(dxr_lib_cso) }, 1, hit_ex };
        D3D12_STATE_SUBOBJECT s2[6] = {
            { D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &soc },
            { D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &lib2 },
            { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hg },
            { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &sc },
            { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pc },
            { D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &gsub },
        };
        D3D12_STATE_OBJECT_DESC d2 = { D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, 6, s2 };
        ID3D12Device7 *dev7;
        CHECK(ID3D12Device5_QueryInterface(dev, &IID_ID3D12Device7, (void **)&dev7));
        HRESULT ahr = ID3D12Device7_AddToStateObject(dev7, &d2, so1, &IID_ID3D12StateObject, (void **)&so2);
        if (FAILED(ahr)) {
            printf("[FAIL] AddToStateObject = 0x%08lx\n", ahr);
            bad++;
        } else {
            ID3D12StateObjectProperties *p2;
            CHECK(ID3D12StateObject_QueryInterface(so2, &IID_ID3D12StateObjectProperties, (void **)&p2));
            for (int i = 0; i < 3; i++) {
                void *id = ID3D12StateObjectProperties_GetShaderIdentifier(p2, names[i]);
                if (!id) {
                    printf("[FAIL] grown state object: no identifier for %ls\n", names[i]);
                    return 1;
                }
                memcpy(sbtp + i * rec, id, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
            }
            transition(cl, out, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ID3D12GraphicsCommandList4_SetComputeRootSignature(cl, grs);
            ID3D12GraphicsCommandList4_SetComputeRootShaderResourceView(cl, 0, ID3D12Resource_GetGPUVirtualAddress(tlas));
            ID3D12GraphicsCommandList4_SetComputeRootUnorderedAccessView(cl, 1, ID3D12Resource_GetGPUVirtualAddress(out));
            ID3D12GraphicsCommandList4_SetPipelineState1(cl, so2);
            ID3D12GraphicsCommandList4_DispatchRays(cl, &dr);
            transition(cl, out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            memset(rbp, 0xAB, out_size);
            ID3D12GraphicsCommandList4_CopyResource(cl, rb, out);
            submit_and_wait();
            bad += verify("AddToStateObject (hit group added later)", rbp);
        }
    }

    printf("%s\n", bad ? "RESULT: FAIL" : "RESULT: PASS");
    return bad ? 1 : 0;
}
