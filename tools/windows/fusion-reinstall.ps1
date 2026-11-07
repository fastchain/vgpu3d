# Close Fusion, install C:\vgpu\vgpu3d-m1.zip (keeps it installed), relaunch Fusion in session 1 and wait for it.
param([int]$WaitSec = 100)
Get-Process Fusion360, FusionLauncher -EA 0 | Stop-Process -Force
Start-Sleep 3
Set-Content C:\vgpu\job.ps1 @"
`$lnk = Get-ChildItem C:\Users\User\Desktop, C:\Users\Public\Desktop -Filter "*Fusion*.lnk" -EA 0 | Select -First 1
Start-Process `$lnk.FullName
Start-Sleep $WaitSec
& C:\vgpu\shot.ps1
"@
powershell -ExecutionPolicy Bypass -File C:\vgpu\install-run.ps1 -TimeoutSec ($WaitSec + 100)
