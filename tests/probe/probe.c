/* Minimal kernel driver to bisect cross-toolchain image-load problems. */
#include <ntddk.h>
DRIVER_INITIALIZE DriverEntry;
static VOID Unload(PDRIVER_OBJECT d) { UNREFERENCED_PARAMETER(d); }
NTSTATUS DriverEntry(PDRIVER_OBJECT d, PUNICODE_STRING reg)
{
    ULONG v = 0x600D;
    RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, reg->Buffer, L"ProbeRan", REG_DWORD, &v, sizeof(v));
    DbgPrintEx(DPFLTR_IHVVIDEO_ID, DPFLTR_ERROR_LEVEL, "probe: DriverEntry ran\n");
    d->DriverUnload = Unload;
    return STATUS_SUCCESS;
}
