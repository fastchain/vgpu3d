/*
 * vgpu3d - virtio-gpu 3D / blob / Venus control path for the vgpu display driver.
 *
 * Adds a "\\.\VGpu3D" control device on top of the display-only (DOD) miniport.  One open
 * handle owns one virtio-gpu context plus its resources and user-mode mappings; everything
 * is released on IRP_MJ_CLEANUP.  The IOCTL set is declared in vgpu_abi.h.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#pragma once

class VioGpuAdapter;

/* Install IRP dispatch wrappers.  Call once from DriverEntry, after DxgkInitialize*. */
extern "C" VOID VGpu3DHookDispatch(_In_ PDRIVER_OBJECT DriverObject);

/* Create the control device.  Call after the virtio device is ready (end of HWInit).
 * Failure is non-fatal for the display path. */
NTSTATUS VGpu3DInit(_In_ VioGpuAdapter *Adapter);

/* Detach from the stopping adapter: open handles become dead, host state is not touched.
 * The control device itself survives for the next start.  Call from HWClose. */
VOID VGpu3DShutdown(VOID);

/* Delete the control device.  Call from DxgkDdiUnload. */
VOID VGpu3DUnload(VOID);
