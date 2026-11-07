# Remove every vgpu3d.inf driver package -> the adapter falls back to Microsoft Basic Display.
$infs = (pnputil /enum-drivers | Out-String) -split "`r?`n`r?`n" | ? { $_ -match "vgpu3d.inf" } |
        % { if ($_ -match "Published Name:\s+(oem\d+\.inf)") { $matches[1] } }
foreach ($i in $infs) { "$i : " + (pnputil /delete-driver $i /uninstall /force | Select -Last 1) }
pnputil /scan-devices | Out-Null
Start-Sleep 8
Get-PnpDevice -Class Display | Select FriendlyName, Status, Problem | Format-Table -AutoSize | Out-String
