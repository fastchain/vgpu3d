# Remove the vgpu Vulkan driver registration and files (keeps the system Vulkan loader).
$ErrorActionPreference = 'Continue'
$key = 'HKLM:\SOFTWARE\Khronos\Vulkan\Drivers'
(Get-Item $key -ErrorAction SilentlyContinue).Property | Where-Object { $_ -like '*venus_icd.json' } |
  ForEach-Object { Remove-ItemProperty -Path $key -Name $_ }
Remove-Item (Join-Path $env:ProgramFiles 'vgpu') -Recurse -Force -ErrorAction SilentlyContinue
Write-Host 'vgpu Vulkan driver removed.'
