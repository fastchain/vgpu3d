# Gather evidence after a failed test: driver state, event log and a DbgView hint. Run elevated.
$out = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'vgpu-diagnostics.txt'
"== date: $(Get-Date)" | Out-File $out
"== display adapters" | Out-File $out -Append
Get-PnpDevice -Class Display | Format-List FriendlyName,Status,Problem,ProblemDescription,DriverProviderName,DriverVersion,InstanceId | Out-File $out -Append
"== bcdedit" | Out-File $out -Append; bcdedit /enum '{current}' | Out-File $out -Append
"== last 30 System events from display/driver sources" | Out-File $out -Append
Get-WinEvent -FilterHashtable @{LogName='System'; Level=1,2,3} -MaxEvents 200 -ErrorAction SilentlyContinue |
  Where-Object { $_.ProviderName -match 'Display|Kernel-PnP|volmgr|BugCheck|Service Control' } | Select-Object -First 30 |
  Format-List TimeCreated,ProviderName,Id,Message | Out-File $out -Append
"== minidumps" | Out-File $out -Append; Get-ChildItem C:\Windows\Minidump -ErrorAction SilentlyContinue | Out-File $out -Append
"Kernel messages (prefix 'vgpu3d:') are emitted with DbgPrintEx; capture them with Sysinternals DebugView (Capture > Capture Kernel) before reproducing." | Out-File $out -Append
Write-Host "Wrote $out"
