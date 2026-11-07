# 32-bit (WOW64) driver checks; run in session 1 via the vgpu-run task.
$d = "C:\vgpu\t32"
"== d3d11test32"; & $d\d3d11test32.exe 2>&1 | Select -Last 3
"== vkcompute32"; & $d\vkcompute32.exe 2>&1 | Select -Last 3
"== sharetest32 (32 -> 32)"; & $d\sharetest32.exe 2>&1 | Select -Last 1
"== sharetest 32 -> 64"; $env:SHARETEST_CHILD = "$d\sharetest64.exe"; & $d\sharetest32.exe 2>&1 | Select -Last 2
"== sharetest 64 -> 32"; $env:SHARETEST_CHILD = "$d\sharetest32.exe"; & $d\sharetest64.exe 2>&1 | Select -Last 2
Remove-Item Env:SHARETEST_CHILD
"== blttest32"; Start-Process $d\blttest32.exe -ArgumentList 10; Start-Sleep 5; & C:\vgpu\shot.ps1
$p = Get-Process blttest32 -EA 0; if ($p) { $m = $p.Modules | ? ModuleName -match "vgpu|dxvk|vulkan_virtio" | % ModuleName; "blttest32 modules: $($m -join ', ')" }
Start-Sleep 6
"== 64-bit regression"
"d3d11test64: " + (& C:\vgpu\t32\d3d11test64.exe 2>&1 | Select -Last 1)
"vkcompute64: " + (& C:\vgpu\t32\vkcompute64.exe 2>&1 | Select -Last 1)
"sharetest64: " + (& C:\vgpu\t32\sharetest64.exe 2>&1 | Select -Last 1)
