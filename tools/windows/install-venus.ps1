<#
.SYNOPSIS
  Install the vgpu Vulkan driver (Mesa Venus) and DXVK for Direct3D 8/9/10/11 apps.
  Run from an elevated PowerShell after the vgpu kernel driver (install.ps1) is working.

.DESCRIPTION
  - copies the Venus ICD to C:\Program Files\vgpu\venus and registers it with the Vulkan loader
    (HKLM\SOFTWARE\Khronos\Vulkan\Drivers)
  - installs the Khronos Vulkan loader (vulkan-1.dll) into System32/SysWOW64 if none is present
  - copies DXVK to C:\Program Files\vgpu\dxvk; use it per application (see README-venus.txt)
#>
[CmdletBinding()] param([string]$PackageDir)
$ErrorActionPreference = 'Stop'
if (-not $PackageDir) { $PackageDir = Split-Path -Parent $MyInvocation.MyCommand.Path }
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  Write-Host 'ERROR: run from an elevated PowerShell' -ForegroundColor Red; exit 1
}
$dest = Join-Path $env:ProgramFiles 'vgpu'
$venus = Join-Path $dest 'venus'
New-Item -ItemType Directory -Force $venus, (Join-Path $dest 'dxvk'), (Join-Path $dest 'tools') | Out-Null

Write-Host '==> Vulkan driver (Venus)' -ForegroundColor Cyan
Copy-Item (Join-Path $PackageDir 'venus\*') $venus -Force
$key = 'HKLM:\SOFTWARE\Khronos\Vulkan\Drivers'
New-Item -Force -Path $key | Out-Null
# drop stale test registrations of this ICD
(Get-Item $key).Property | Where-Object { $_ -like '*venus_icd.json' } | ForEach-Object { Remove-ItemProperty -Path $key -Name $_ }
New-ItemProperty -Path $key -Name (Join-Path $venus 'venus_icd.json') -Value 0 -PropertyType DWord -Force | Out-Null

Write-Host '==> Vulkan loader' -ForegroundColor Cyan
foreach ($pair in @(@('x64', "$env:SystemRoot\System32"), @('x86', "$env:SystemRoot\SysWOW64"))) {
  $src = Join-Path $PackageDir "vulkanrt\$($pair[0])\vulkan-1.dll"
  $dst = Join-Path $pair[1] 'vulkan-1.dll'
  if (-not (Test-Path $dst)) { Copy-Item $src $dst; Write-Host "  installed $dst" }
  else { Write-Host "  kept existing $dst ($((Get-Item $dst).VersionInfo.FileVersion))" }
}
Copy-Item (Join-Path $PackageDir 'vulkanrt\x64\vulkaninfo.exe') (Join-Path $dest 'tools') -Force

Write-Host '==> DXVK + test tools' -ForegroundColor Cyan
Copy-Item (Join-Path $PackageDir 'dxvk\*') (Join-Path $dest 'dxvk') -Recurse -Force
Copy-Item (Join-Path $PackageDir 'tools\*') (Join-Path $dest 'tools') -Force

Write-Host '==> Check' -ForegroundColor Cyan
$ErrorActionPreference = 'Continue'   # the loader prints harmless warnings on stderr
& (Join-Path $dest 'tools\vulkaninfo.exe') --summary 2>&1 | ForEach-Object { "$_" } | Select-String 'deviceName|driverName|apiVersion'
& (Join-Path $dest 'tools\vkcompute.exe') 2>&1 | ForEach-Object { "$_" } | Select-String 'RESULT'
Write-Host "Done. Vulkan apps now use the host GPU. For Direct3D apps see $dest\dxvk\README.txt" -ForegroundColor Green
