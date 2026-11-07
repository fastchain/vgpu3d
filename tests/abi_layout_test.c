/* Host-side check of the vgpu ABI layout (run: build/run_tests.sh). */
#define VGPU_ABI_STANDALONE 1
#include <stddef.h>
#include <stdio.h>
#include "vgpu_abi.h"
#define SZ(t, n) _Static_assert(sizeof(t) == (n), #t " size")
#define OFF(t, f, n) _Static_assert(offsetof(t, f) == (n), #t "." #f " offset")
SZ(VGPU_GETPARAM, 16);          OFF(VGPU_GETPARAM, value, 8);
SZ(VGPU_GET_CAPSET_INFO, 16);
SZ(VGPU_GET_CAPS, 16);
SZ(VGPU_CONTEXT_INIT, 88);      OFF(VGPU_CONTEXT_INIT, poll_rings_mask, 8); OFF(VGPU_CONTEXT_INIT, debug_name, 16); OFF(VGPU_CONTEXT_INIT, ctx_id, 80);
SZ(VGPU_RESOURCE_CREATE_BLOB, 32); OFF(VGPU_RESOURCE_CREATE_BLOB, blob_id, 8); OFF(VGPU_RESOURCE_CREATE_BLOB, size, 16); OFF(VGPU_RESOURCE_CREATE_BLOB, cmd_size, 24); OFF(VGPU_RESOURCE_CREATE_BLOB, handle, 28);
SZ(VGPU_RESOURCE_CREATE, 56);   OFF(VGPU_RESOURCE_CREATE, size, 40);
SZ(VGPU_RESOURCE_INFO, 24);
SZ(VGPU_MAP, 24);               OFF(VGPU_MAP, user_va, 8);
SZ(VGPU_HANDLE, 8);
SZ(VGPU_BOX, 24);
SZ(VGPU_TRANSFER, 48);          OFF(VGPU_TRANSFER, offset, 16); OFF(VGPU_TRANSFER, box, 24);
SZ(VGPU_EXECBUFFER, 24);        OFF(VGPU_EXECBUFFER, fence_id, 16);
SZ(VGPU_WAIT_FENCE, 24);        OFF(VGPU_WAIT_FENCE, fence_id, 8);
SZ(VGPU_QUERY_FENCE, 24);
int main(void)
{
    /* well-known value for FILE_DEVICE_UNKNOWN, METHOD_BUFFERED, RW, function 0x801 */
    if (IOCTL_VGPU_GETPARAM != 0x0022E004u) { printf("ioctl code mismatch %08x\n", IOCTL_VGPU_GETPARAM); return 1; }
    if (VGPU_EXECBUF_TOTAL(2, 100) != 24 + 8 + 100) return 2;
    printf("abi layout ok (ABI v%d)\n", VGPU_ABI_VERSION);
    return 0;
}
