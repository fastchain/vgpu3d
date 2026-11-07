$boot0 = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime
$id = (Get-PnpDevice -InstanceId "PCI\VEN_1AF4&DEV_1050*").InstanceId
for ($i = 1; $i -le 15; $i++) {
  $t = (C:\vgpu\vgpu-guest\vgputest.exe | Select -Last 1)
  $c = (& "C:\Program Files\vgpu\tools\vkcompute.exe" | Select-String 'RESULT').Line
  pnputil /restart-device "$id" | Out-Null
  Start-Sleep 4
  $st = (Get-PnpDevice -InstanceId $id).Status
  "cycle $i : vgputest=[$t] vkcompute=[$c] device=$st"
}
"boot unchanged: $((Get-CimInstance Win32_OperatingSystem).LastBootUpTime -eq $boot0)"
