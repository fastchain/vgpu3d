/*
 * dcompshim - per-application dcomp.dll proxy for DXVK.
 *
 * DirectComposition only works with Windows' own D3D11/DXGI objects.  Apps that hand it a
 * DXVK device (e.g. Chromium/Qt WebEngine inside Autodesk Fusion) crash inside dcomp.dll.
 * Placed next to the app's .exe together with dcomp_sys.dll (a copy of
 * C:\Windows\System32\dcomp.dll), this proxy makes DCompositionCreateDevice* fail so callers
 * fall back to plain swapchains; every other export is forwarded to the real DLL.
 *
 * SPDX-License-Identifier: MIT
 */
#include <windows.h>

HRESULT WINAPI DCompositionCreateDevice(IUnknown *dxgiDevice, REFIID iid, void **dcompositionDevice)
{
    if (dcompositionDevice)
        *dcompositionDevice = NULL;
    return E_NOTIMPL;
}

HRESULT WINAPI DCompositionCreateDevice2(IUnknown *renderingDevice, REFIID iid, void **dcompositionDevice)
{
    if (dcompositionDevice)
        *dcompositionDevice = NULL;
    return E_NOTIMPL;
}

HRESULT WINAPI DCompositionCreateDevice3(IUnknown *renderingDevice, REFIID iid, void **dcompositionDevice)
{
    if (dcompositionDevice)
        *dcompositionDevice = NULL;
    return E_NOTIMPL;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    return TRUE;
}
