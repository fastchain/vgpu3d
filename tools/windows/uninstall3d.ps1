<#
.SYNOPSIS
  Remove every vgpu3d.inf driver package; the adapter falls back to Microsoft Basic Display.
  Run elevated.  -RemoveCert also deletes the "CN=vgpu test signing" certificate from the
  My / Root / TrustedPublisher stores.  Test signing stays on: "bcdedit /set testsigning off" to undo.
#>
param([switch]$RemoveCert)
$infs = (pnputil /enum-drivers | Out-String) -split "`r?`n`r?`n" | ? { $_ -match 'vgpu3d\.inf' } |
        % { if ($_ -match 'Published Name:\s+(oem\d+\.inf)') { $matches[1] } }
if (-not $infs) { Write-Host 'No vgpu3d driver package found.' }
foreach ($i in $infs) { "$i : " + (pnputil /delete-driver $i /uninstall /force | Select -Last 1) }
if ($RemoveCert) {
  foreach ($store in 'My', 'Root', 'TrustedPublisher') {
    Get-ChildItem "Cert:\LocalMachine\$store" | ? { $_.Subject -eq 'CN=vgpu test signing' } |
      % { Remove-Item $_.PSPath; "removed certificate from LocalMachine\$store" }
  }
}
pnputil /scan-devices | Out-Null
Start-Sleep 5
Get-PnpDevice -Class Display -PresentOnly | Select FriendlyName, Status | Format-Table -AutoSize | Out-String
exit 0
