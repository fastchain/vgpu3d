// dxrtest ray tracing library: raygen / miss / closest hit (compile: dxc -T lib_6_3)
RaytracingAccelerationStructure Scene : register(t0);
RWStructuredBuffer<uint2> Out : register(u0);

struct Payload { uint2 v; };

[shader("raygeneration")]
void RayGen()
{
    uint2 p = DispatchRaysIndex().xy;
    RayDesc r;
    r.Origin = float3(-1.0 + (p.x + 0.5) / 32.0, -1.0 + (p.y + 0.5) / 32.0, 0.0);
    r.Direction = float3(0, 0, 1);
    r.TMin = 0.001;
    r.TMax = 100.0;
    Payload pl;
    pl.v = uint2(0, 0);
    TraceRay(Scene, RAY_FLAG_FORCE_OPAQUE, 0xFF, 0, 1, 0, r, pl);
    Out[p.y * 64 + p.x] = pl.v;
}

[shader("miss")]
void Miss(inout Payload pl)
{
    pl.v = uint2(0xFFFF, 0);
}

[shader("closesthit")]
void Hit(inout Payload pl, BuiltInTriangleIntersectionAttributes a)
{
    pl.v = uint2(InstanceID(), asuint(RayTCurrent()));
}
