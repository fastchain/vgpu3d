import adsk.core, adsk.fusion

def run(_context):
    app = adsk.core.Application.get()
    app.documents.add(adsk.core.DocumentTypes.FusionDesignDocumentType)
    design = adsk.fusion.Design.cast(app.activeProduct)
    design.designType = adsk.fusion.DesignTypes.DirectDesignType
    root = design.rootComponent
    d = 'C:/Users/User/Downloads/models/'
    U = adsk.fusion.MeshUnits
    items = [('xyzrgb_dragon', U.CentimeterMeshUnit, i) for i in range(9)]
    items += [('lucy', U.MillimeterMeshUnit, 0), ('happy', U.MeterMeshUnit, 0), ('armadillo', U.CentimeterMeshUnit, 0)]
    for k, (name, unit, i) in enumerate(items):
        occ = root.occurrences.addNewComponent(adsk.core.Matrix3D.create())
        mb = occ.component.meshBodies.add(d + name + '.obj', unit).item(0)
        mb.name = '%s_%d' % (name, i)
        bb = mb.boundingBox
        if name == 'xyzrgb_dragon':
            gx, gy = (i % 3) * 260.0, (i // 3) * 180.0
        else:
            gx, gy = 900.0 + (k - 9) * 220.0, 0.0
        m = adsk.core.Matrix3D.create()
        m.translation = adsk.core.Vector3D.create(gx - bb.minPoint.x, gy - bb.minPoint.y, -bb.minPoint.z)
        occ.transform = m
    vp = app.activeViewport
    cam = vp.camera
    cam.viewOrientation = adsk.core.ViewOrientations.IsoTopRightViewOrientation
    cam.isFitView = True
    vp.camera = cam
    vp.refresh()
