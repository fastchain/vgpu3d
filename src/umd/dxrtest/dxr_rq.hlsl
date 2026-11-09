// dxrtest inline ray query (DXR 1.1; compile: dxc -T cs_6_5 -E main)
RaytracingAccelerationStructure Scene : register(t0);
RWStructuredBuffer<uint2> Out : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    RayDesc r;
    r.Origin = float3(-1.0 + (id.x + 0.5) / 32.0, -1.0 + (id.y + 0.5) / 32.0, 0.0);
    r.Direction = float3(0, 0, 1);
    r.TMin = 0.001;
    r.TMax = 100.0;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(Scene, 0, 0xFF, r);
    q.Proceed();
    uint2 v = uint2(0xFFFF, 0);
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
        v = uint2(q.CommittedInstanceID(), asuint(q.CommittedRayT()));
    Out[id.y * 64 + id.x] = v;
}
