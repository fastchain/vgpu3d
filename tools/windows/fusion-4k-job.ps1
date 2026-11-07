& C:\vgpu\m2test\modes2.exe 3840 2160
$lnk = Get-ChildItem $env:USERPROFILE\Desktop, C:\Users\Public\Desktop -Filter "*Fusion*.lnk" -EA 0 | Select -First 1
Start-Process $lnk.FullName -WindowStyle Maximized
Start-Sleep 110
Add-Type @"
using System; using System.Runtime.InteropServices;
public class W2 { [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c); [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h); }
"@
$p = Get-Process Fusion360 -EA 0 | ? MainWindowHandle -ne 0 | Select -First 1
if ($p) { [W2]::ShowWindow($p.MainWindowHandle, 3) | Out-Null; [W2]::SetForegroundWindow($p.MainWindowHandle) | Out-Null }
Start-Sleep 3
