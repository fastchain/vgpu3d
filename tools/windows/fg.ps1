# Keep a process's window in the foreground without synthesizing input (for fullscreen tests started by the
# vgpu-run task, whose windows cannot take the foreground after a reboot).
# The old Alt-key trick (keybd_event VK_MENU) counted as user input: GravityMark then rendered with its
# window never shown (inflated scores), and exclusive-fullscreen apps saw an Alt press.
# Watches for -TimeoutSec: whenever the foreground window is not the process's, raises its largest visible
# top-level window (apps like Superposition open a console first and the render window later).
param([string]$Name, [int]$TimeoutSec = 60)
Add-Type -TypeDefinition @"
using System; using System.Collections.Generic; using System.Runtime.InteropServices;
public static class FG {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int l, t, r, b; }
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern void SwitchToThisWindow(IntPtr h, bool altTab);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SystemParametersInfo(uint a, uint b, IntPtr c, uint d);
  public static IntPtr Largest(HashSet<uint> pids) {
    IntPtr best = IntPtr.Zero; long area = 0;
    EnumWindows((h, l) => {
      uint pid; GetWindowThreadProcessId(h, out pid);
      RECT r;
      if (pids.Contains(pid) && IsWindowVisible(h) && GetWindowRect(h, out r)) {
        long a = (long)(r.r - r.l) * (r.b - r.t);
        if (a > area) { area = a; best = h; }
      }
      return true;
    }, IntPtr.Zero);
    return best;
  }
}
"@
[FG]::SystemParametersInfo(0x2001, 0, [IntPtr]::Zero, 2) | Out-Null  # SPI_SETFOREGROUNDLOCKTIMEOUT = 0
$t0 = Get-Date; $raised = 0; $last = [IntPtr]::Zero
while (((Get-Date) - $t0).TotalSeconds -lt $TimeoutSec) {
  $pids = New-Object 'System.Collections.Generic.HashSet[uint32]'
  Get-Process $Name -EA 0 | % { [void]$pids.Add([uint32]$_.Id) }
  if ($pids.Count -eq 0 -and $raised) { break }   # the app exited
  $fgw = [FG]::GetForegroundWindow(); $fpid = [uint32]0
  [void][FG]::GetWindowThreadProcessId($fgw, [ref]$fpid)
  if ($pids.Count -and -not $pids.Contains($fpid)) {
    $h = [FG]::Largest($pids)
    if ($h -ne [IntPtr]::Zero) {
      $fgt = [FG]::GetWindowThreadProcessId($fgw, [ref]$fpid); $me = [FG]::GetCurrentThreadId()
      [FG]::AttachThreadInput($me, $fgt, $true) | Out-Null
      [FG]::BringWindowToTop($h) | Out-Null
      [FG]::SwitchToThisWindow($h, $true)
      $r = [FG]::SetForegroundWindow($h)
      [FG]::AttachThreadInput($me, $fgt, $false) | Out-Null
      $raised++
      if ($h -ne $last) { "foreground $Name window $h -> $r (now $([FG]::GetForegroundWindow() -eq $h))"; $last = $h }
    }
  }
  Start-Sleep -Milliseconds 300
}
if (-not $raised) { "no window raised for $Name (already foreground or never appeared)" }
