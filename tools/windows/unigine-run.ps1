# Run a Unigine benchmark unattended (Phoronix-style automation define) in session 1.
# usage: unigine-run.ps1 -Bench Heaven|Valley -Api direct3d11|opengl [-W 1920 -H 1080 -Full 0]
param([string]$Bench = "Heaven", [string]$Api = "direct3d11", [int]$W = 1920, [int]$H = 1080, [int]$Full = 0,
      [string]$Quality = "QUALITY_HIGH", [int]$TimeoutSec = 600, [int]$VSync = -1)
$root = "C:\bench\$Bench"
$cfg = if ($Bench -eq "Heaven") { "../data/heaven_4.0.cfg" } else { "../data/valley_1.0.cfg" }
$script = if ($Bench -eq "Heaven") { "heaven/unigine.cpp" } else { "valley/unigine.cpp" }
$args = @("-project_name", $Bench, "-data_path", "../", "-engine_config", $cfg, "-system_script", $script,
          "-sound_app", "null", "-video_app", $Api, "-video_multisample", "0", "-video_fullscreen", "$Full",
          "-video_mode", "-1", "-video_width", "$W", "-video_height", "$H",
          "-extern_define", "PHORONIX,RELEASE,$Quality")
if ($VSync -ge 0) { $args += @("-video_vsync", "$VSync") }
$t0 = Get-Date
$p = Start-Process "$root\bin\$Bench.exe" -ArgumentList $args -WorkingDirectory "$root\bin" -PassThru
# mid-run screenshot proves the frames are really rendered (a white/black window can still score)
Start-Sleep 60
& C:\vgpu\shot.ps1 | Out-Null
Copy-Item C:\vgpu\m2test\fusion.png "C:\vgpu\m2test\$Bench-$Api.png" -Force
if (-not $p.WaitForExit($TimeoutSec * 1000)) { "TIMEOUT"; $p.Kill() }
$log = (Get-Content "$env:USERPROFILE\$Bench\log.html" -Raw) -replace "<[^>]+>"," " -replace "&nbsp;"," "
$fps = [regex]::Match($log, "FPS:\s+([0-9.]+)").Groups[1].Value
$err = ([regex]::Matches($log, "GLShader::load\w+\(\): error")).Count
"RESULT $Bench $Api ${W}x$H vsync=$VSync fps=$fps shader_errors=$err exit=$($p.ExitCode) $([math]::Round(((Get-Date) - $t0).TotalSeconds)) s"
