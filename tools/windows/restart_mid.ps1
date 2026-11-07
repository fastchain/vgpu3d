$boot0 = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime
Remove-Item C:\vgpu\vkcube_long.txt -ErrorAction SilentlyContinue
$a = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File C:\vgpu\vkcube_long.ps1'
$pr = New-ScheduledTaskPrincipal -UserId 'User' -LogonType Interactive -RunLevel Limited
Register-ScheduledTask -TaskName vgpu-vkcube-long -Action $a -Principal $pr -Force | Out-Null
Start-ScheduledTask vgpu-vkcube-long
Start-Sleep 6
$v = Get-Process vkcube -ErrorAction SilentlyContinue
"vkcube running before restart: $([bool]$v)"
$id = (Get-PnpDevice -InstanceId "PCI\VEN_1AF4&DEV_1050*").InstanceId
$sw = [Diagnostics.Stopwatch]::StartNew()
pnputil /restart-device "$id" | Select -Last 1
"restart-device took $([int]$sw.Elapsed.TotalSeconds)s, device=$((Get-PnpDevice -InstanceId $id).Status)"
Start-Sleep 5
$v = Get-Process vkcube -ErrorAction SilentlyContinue
if ($v) {
  "vkcube still running after restart (threads=$($v.Threads.Count)); killing"
  Stop-Process -Id $v.Id -Force -ErrorAction SilentlyContinue
  Start-Sleep 3
  $v2 = Get-Process -Id $v.Id -ErrorAction SilentlyContinue
  "vkcube after kill: $(if ($v2) { 'STILL ALIVE (stuck in kernel?) threads=' + $v2.Threads.Count } else { 'gone' })"
} else { "vkcube already exited: $(Get-Content C:\vgpu\vkcube_long.txt -ErrorAction SilentlyContinue)" }
"vgputest after: $(C:\vgpu\vgpu-guest\vgputest.exe | Select -Last 1)"
"boot unchanged: $((Get-CimInstance Win32_OperatingSystem).LastBootUpTime -eq $boot0)"
