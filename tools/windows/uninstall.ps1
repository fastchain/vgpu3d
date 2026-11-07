# Remove the vgpu driver package (falls back to the previous display driver). Run elevated.
$ErrorActionPreference = 'Stop'
$pkg = (pnputil /enum-drivers) -join "`n"
$m = [regex]::Matches($pkg, 'Published Name:\s+(oem\d+\.inf)\s+Original Name:\s+vgpu\.inf')
foreach ($x in $m) { pnputil /delete-driver $x.Groups[1].Value /uninstall /force }
if ($m.Count -eq 0) { Write-Host 'vgpu package not found.' }
