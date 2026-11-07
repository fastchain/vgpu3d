import adsk.core, adsk.fusion, math, time

def run(_context):
    app = adsk.core.Application.get()
    vp = app.activeViewport
    cam = vp.camera
    t, e = cam.target, cam.eye
    dx, dy, dz = e.x - t.x, e.y - t.y, e.z - t.z
    r = math.hypot(dx, dy)
    a0 = math.atan2(dy, dx)
    n = 72
    times = []
    for i in range(1, n + 1):
        a = a0 + 2 * math.pi * i / n
        c = vp.camera
        c.isSmoothTransition = False
        c.eye = adsk.core.Point3D.create(t.x + r * math.cos(a), t.y + r * math.sin(a), t.z + dz)
        c.target = t
        c.upVector = adsk.core.Vector3D.create(0, 0, 1)
        s = time.perf_counter()
        vp.camera = c
        vp.refresh()
        adsk.doEvents()
        times.append(time.perf_counter() - s)
    tot = sum(times)
    times.sort()
    raise Exception('ORBIT frames=%d total=%.2fs avg=%.1fms median=%.1fms p90=%.1fms fps~%.1f' % (n, tot, 1000 * tot / n, 1000 * times[n // 2], 1000 * times[int(n * 0.9)], n / tot))
