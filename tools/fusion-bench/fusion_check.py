import adsk.core, adsk.fusion

def run(_context):
    app = adsk.core.Application.get()
    design = adsk.fusion.Design.cast(app.activeProduct)
    out = ['doc=' + app.activeDocument.name]
    total = 0
    for occ in design.rootComponent.allOccurrences:
        for mb in occ.component.meshBodies:
            n = mb.displayMesh.triangleCount
            total += n
            bb = mb.boundingBox
            out.append('%s: %d tris, size %.1f x %.1f x %.1f cm' % (mb.name, n, bb.maxPoint.x - bb.minPoint.x, bb.maxPoint.y - bb.minPoint.y, bb.maxPoint.z - bb.minPoint.z))
    out.append('total triangles: %d' % total)
    raise Exception(' | '.join(out))
