Set-Location 'C:\Program Files\vgpu\tools'
$p = Start-Process .\vkcube.exe -ArgumentList '--c','3000' -PassThru
$p.WaitForExit(120000) | Out-Null
"exited=$($p.HasExited) code=$(if ($p.HasExited) { $p.ExitCode })" | Set-Content C:\vgpu\vkcube_long.txt
