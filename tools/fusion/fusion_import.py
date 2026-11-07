import adsk.core, adsk.fusion

def run(_context):
    app = adsk.core.Application.get()
    doc = app.documents.add(adsk.core.DocumentTypes.FusionDesignDocumentType)
    design = adsk.fusion.Design.cast(app.activeProduct)
    design.designType = adsk.fusion.DesignTypes.DirectDesignType
    root = design.rootComponent
    d = 'C:/Users/User/Downloads/models/'
    x = 0.0
    for name in ('xyzrgb_dragon', 'lucy', 'happy', 'armadillo'):
        occ = root.occurrences.addNewComponent(adsk.core.Matrix3D.create())
        comp = occ.component
        meshes = comp.meshBodies.add(d + name + '.obj', adsk.fusion.MeshUnits.CentimeterMeshUnit)
        mb = meshes.item(0)
        mb.name = name
        bb = mb.boundingBox
        sx = bb.maxPoint.x - bb.minPoint.x
        sz = bb.maxPoint.z - bb.minPoint.z
        # rigid move only: stand on z=0, line up along x
        m = adsk.core.Matrix3D.create()
        m.translation = adsk.core.Vector3D.create(x - bb.minPoint.x, -bb.minPoint.y, -bb.minPoint.z)
        occ.transform = m
        x += sx + 0.2 * sx
    vp = app.activeViewport
    cam = vp.camera
    cam.viewOrientation = adsk.core.ViewOrientations.IsoTopRightViewOrientation
    cam.isFitView = True
    vp.camera = cam
    vp.refresh()
