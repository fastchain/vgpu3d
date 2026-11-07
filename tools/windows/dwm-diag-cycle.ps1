# Install vgpu3d, let DWM run on it, collect DWM diagnostics, roll back.
# Output: C:\vgpu\dwmdiag\*  (dbg lines of DWM, KMD present activity, cdb stacks)
param([int]$Settle = 25)
$o = 'C:\vgpu\dwmdiag'; Remove-Item -Recurse -Force $o -EA 0; New-Item -ItemType Directory $o | Out-Null
reg add HKLM\SYSTEM\CurrentControlSet\Services\viogpu3d /v VgpuBootGuard /t REG_DWORD /d 0 /f 2>$null | Out-Null
# fresh DbgView log per round (a long log costs DbgView and PowerShell gigabytes of RAM)
Stop-Process -Name Dbgview64 -Force -EA 0; Start-Sleep 1
Remove-Item C:\vgpu\dbg.log -EA 0
Start-ScheduledTask vgpu-dbgview; Start-Sleep 3
$n0 = 0
Remove-Item -Recurse -Force C:\vgpu\m1 -EA 0
Expand-Archive C:\vgpu\vgpu3d-m1.zip C:\vgpu\m1
Push-Location C:\vgpu\m1\vgpu3d
powershell -ExecutionPolicy Bypass -File .\install.ps1 2>&1 | Select-String "installed on|ERROR|reboot"
Pop-Location
Start-Sleep $Settle
Get-PnpDevice -Class Display | Select FriendlyName, Status, Problem | Format-Table -AutoSize | Out-String
# screenshot from the interactive session (C:\vgpu\job.ps1 via task vgpu-run)
Remove-Item C:\vgpu\job.txt -EA 0
Stop-ScheduledTask vgpu-run -EA 0
Start-ScheduledTask vgpu-run
$t = 0
while (-not ((Test-Path C:\vgpu\job.txt) -and (Select-String -Path C:\vgpu\job.txt -Pattern "--- done" -Quiet)) -and $t -lt 30) { Start-Sleep 1; $t++ }
Get-Content C:\vgpu\job.txt -EA 0
$dwm = Get-Process dwm | Sort StartTime -Desc | Select -First 1
"dwm pid $($dwm.Id) started $($dwm.StartTime) responding=$($dwm.Responding)"
Get-WinEvent -FilterHashtable @{LogName='Application'; ProviderName='Dwminit'; StartTime=(Get-Date).AddMinutes(-2)} -EA 0 | Select -First 3 TimeCreated, Message | Format-List | Out-String -Width 200
# cdb non-invasive: all thread stacks
$sym = 'srv*C:\symcache*https://msdl.microsoft.com/download/symbols;C:\vgpu\m1\vgpu3d'
& C:\vgpu\kd\cdb.exe -pv -p $dwm.Id -y $sym -lines -c ".reload; ~* kc 25; qd" 2>&1 | Out-File $o\dwm_stacks.txt -Width 300
# text of a pending assertion dialog (MinGW msvcrt assert -> MessageBox), searched on every thread's stack
& C:\vgpu\kd\cdb.exe -pv -p $dwm.Id -c "~*e .foreach /pS 0 (a {s -[1]a @rsp L20000 \"Assertion\"}) {da ${a} L300}; qd" 2>&1 | Select-String -Pattern '^[0-9a-f]{8}`' | Out-File $o\dwm_assert.txt -Width 400
$l = Get-Content C:\vgpu\dbg.log | Select -Skip $n0
$l | ? { ($_ -split "`t")[2] -eq "$($dwm.Id)" } | Out-File $o\dwm_dbg.txt -Width 300
$l | ? { $_ -match "Present|Flip|SetVidPn|Scanout|SCANOUT|CommitVidPn|SetSourceAddress|PRESENT|Gate|GATE|Escape|error|ERROR|fail|FAIL|STALE" -and $_ -notmatch "Dpc|caps\[" } | Select -Last 300 | Out-File $o\kmd_present.txt -Width 300
"collected"
'=== rollback'
& C:\vgpu\vgpu3d-rollback.ps1
