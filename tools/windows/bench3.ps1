# Step-3 benchmarks (no account): FurMark 2 (1080p preset) and GravityMark, 1920x1080.
# Each run: mid-run screenshot C:\vgpu\m2test\<name>.png, then results collected.
param([string[]]$Only = @())
function Shot($name) { & C:\vgpu\shot.ps1 | Out-Null; Copy-Item C:\vgpu\m2test\fusion.png "C:\vgpu\m2test\$name.png" -Force }
function Bench($name, $exe, $al, $dir, $shotAfter = 30, $timeout = 600) {
  if ($Only.Count -and $Only -notcontains $name) { return }
  $t0 = Get-Date
  $p = Start-Process $exe -ArgumentList $al -WorkingDirectory $dir -PassThru
  Start-Sleep $shotAfter; Shot $name
  if (-not $p.WaitForExit($timeout * 1000)) { "TIMEOUT $name"; $p.Kill() }
  "RUN $name exit=$($p.ExitCode) $([math]::Round(((Get-Date) - $t0).TotalSeconds)) s"
}
$fm = "C:\bench\FurMark2"
Bench "furmark-vk" "$fm\furmark.exe" @("--demo","furmark-vk","--p1080","--no-score-box") $fm 30
Bench "furmark-gl" "$fm\furmark.exe" @("--demo","furmark-gl","--p1080","--no-score-box") $fm 30
$gm = "C:\Program Files\GravityMark\bin"
foreach ($api in "vk","d3d11","gl") {
  Bench "gravitymark-$api" "$gm\GravityMark.exe" @("-temporal","1","-fps","1","-info","1","-benchmark","1","-close","1",
    "-fullscreen","0","-width","1920","-height","1080","-vsync","0","-$api","-image","C:\vgpu\m2test\gravitymark-$api-score.png") $gm 45
}
"== FurMark scores"
Get-ChildItem $fm -Recurse -Filter "*scores*.csv" -EA 0 | % { Get-Content $_.FullName | Select -Last 4 }
"== GravityMark logs"
Get-ChildItem $env:APPDATA, $env:LOCALAPPDATA, $env:USERPROFILE, $gm -Recurse -Depth 2 -Include "*.log","*.txt" -EA 0 | ? { $_.LastWriteTime -gt (Get-Date).AddMinutes(-20) -and $_.FullName -match "(?i)gravity" } | % { "-- $($_.FullName)"; Get-Content $_.FullName | Select-String -Pattern "(?i)score|fps|vulkan|direct3d|opengl|device" | Select -Last 12 | % { $_.Line } }
