# Bring the main window of a process to the foreground (Alt-key trick lifts the foreground lock).
param([string]$Name, [int]$TimeoutSec = 60)
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public static class FG {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr pid);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SystemParametersInfo(uint a, uint b, IntPtr c, uint d);
}
"@
$t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt $TimeoutSec) {
  $p = Get-Process $Name -EA 0 | ? { $_.MainWindowHandle -ne 0 } | select -First 1
  if ($p) {
    [FG]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero); [FG]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)
    [FG]::SystemParametersInfo(0x2001, 0, [IntPtr]::Zero, 2) | Out-Null  # SPI_SETFOREGROUNDLOCKTIMEOUT = 0
    $fgt = [FG]::GetWindowThreadProcessId([FG]::GetForegroundWindow(), [IntPtr]::Zero)
    $me = [FG]::GetCurrentThreadId()
    [FG]::AttachThreadInput($me, $fgt, $true) | Out-Null
    [FG]::ShowWindow($p.MainWindowHandle, 5) | Out-Null
    [FG]::BringWindowToTop($p.MainWindowHandle) | Out-Null
    $r = [FG]::SetForegroundWindow($p.MainWindowHandle)
    [FG]::AttachThreadInput($me, $fgt, $false) | Out-Null
    "foreground $Name -> $r"
    return
  }
  Start-Sleep -Milliseconds 200
}
"no window for $Name"
