# Install the vgpu3d package, run C:\vgpu\job.ps1 in the interactive session (scheduled task vgpu-run),
# then always roll back to Microsoft Basic Display.  Output: C:\vgpu\job.txt
param([int]$TimeoutSec = 180)
reg add HKLM\SYSTEM\CurrentControlSet\Services\viogpu3d /v VgpuBootGuard /t REG_DWORD /d 0 /f 2>$null | Out-Null
if (-not (Get-Process Dbgview64 -EA 0)) { Start-ScheduledTask vgpu-dbgview; Start-Sleep 3 }
Remove-Item -Recurse -Force C:\vgpu\m1 -EA 0
Expand-Archive C:\vgpu\vgpu3d-m1.zip C:\vgpu\m1
Push-Location C:\vgpu\m1\vgpu3d
powershell -ExecutionPolicy Bypass -File .\install.ps1 2>&1 | Select-String "installed on|ERROR|reboot"
Pop-Location
Start-Sleep 8
Get-PnpDevice -Class Display | Select FriendlyName, Status, Problem | Format-Table -AutoSize | Out-String
Set-Content C:\vgpu\n0.txt (Get-Content C:\vgpu\dbg.log).Count
Remove-Item C:\vgpu\job.txt -EA 0
Stop-ScheduledTask vgpu-run -EA 0
Start-ScheduledTask vgpu-run
$t = 0
while (-not ((Test-Path C:\vgpu\job.txt) -and (Select-String -Path C:\vgpu\job.txt -Pattern "--- done" -Quiet)) -and $t -lt $TimeoutSec) { Start-Sleep 1; $t++ }
if ($t -ge $TimeoutSec) { "JOB TIMEOUT"; Stop-ScheduledTask vgpu-run -EA 0 }
Get-Content C:\vgpu\job.txt -EA 0
'=== rollback'
& C:\vgpu\vgpu3d-rollback.ps1
