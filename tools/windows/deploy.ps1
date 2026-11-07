Remove-Item -Recurse -Force C:\vgpu\vgpu-guest -ErrorAction SilentlyContinue
Expand-Archive C:\Users\User\vgpu-guest.zip C:\vgpu -Force
Remove-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Services\VGpu -Name InitStatus -ErrorAction SilentlyContinue
if (-not (Get-Process Dbgview64 -ErrorAction SilentlyContinue)) { Start-ScheduledTask vgpu-dbgview; Start-Sleep 4 }
$n0 = (Get-Content C:\vgpu\dbg.log -ErrorAction SilentlyContinue).Count
powershell -NoProfile -ExecutionPolicy Bypass -File C:\vgpu\vgpu-guest\install.ps1 *>&1 | Select-String "ERROR|Published|reboot|added|Failed" | Out-String -Width 200
$id = (Get-PnpDevice -InstanceId "PCI\VEN_1AF4&DEV_1050*").InstanceId
Start-Sleep 3
Get-PnpDevice -InstanceId $id | Select Status, Problem | Format-List
'DriverVersion: ' + (Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_DriverVersion).Data
$s = (Get-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Services\VGpu -ErrorAction SilentlyContinue).InitStatus
if ($s -ne $null) { 'InitStatus: 0x{0:X8}' -f [uint32]$s } else { 'InitStatus: (not written)' }
Get-WinEvent -FilterHashtable @{LogName='System'; ProviderName='Microsoft-Windows-Kernel-PnP'; Id=219; StartTime=(Get-Date).AddMinutes(-2)} -ErrorAction SilentlyContinue | ForEach-Object { ([xml]$_.ToXml()).Event.EventData.Data | Where-Object Name -eq Status | ForEach-Object { 'PnP219 status 0x{0:X}' -f [uint32]$_.'#text' } }
Get-ChildItem C:\Windows\Minidump -ErrorAction SilentlyContinue | Select Name, LastWriteTime
Get-Content C:\vgpu\dbg.log | Select -Skip $n0 | Where-Object { $_ -match 'vgpu|KMDOD|viogpu|VioGpu|dxg' } | Select -First 60
