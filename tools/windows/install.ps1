<#
.SYNOPSIS
  Install the test-signed vgpu driver on this Windows guest.  Run from an elevated PowerShell.

.DESCRIPTION
  1. Checks Secure Boot / test-signing state (test signing needs Secure Boot NOT enforced).
  2. Creates a self-signed code-signing certificate and trusts it (Root + TrustedPublisher).
  3. Signs vgpu.sys, builds vgpu.cat with Inf2Cat, signs the catalog.
  4. Installs the driver package with pnputil.
  5. Stores the host GPU's memory size (tools\vgpuvram.exe, Vulkan) as the adapter's VideoMemoryMB and
     restarts the adapter, so Windows reports the real dedicated video memory.  -VramOnly runs just this
     step (the installer schedules it for the next logon when the install needs a reboot).

  Re-run after "bcdedit /set testsigning on" + reboot if step 1 asks for it.

  Exit codes (used by the vgpu3d-setup.exe installer): 0 installed, 1 error,
  3 test signing was just enabled (reboot, then run again), 3010 installed but a reboot is required.
#>
[CmdletBinding()]
param(
  [string]$PackageDir,
  [switch]$SkipInstall,
  [switch]$VramOnly
)
$ErrorActionPreference = 'Stop'
if (-not $PackageDir) { $PackageDir = Split-Path -Parent $MyInvocation.MyCommand.Path }
function Step($m) { Write-Host "==> $m" -ForegroundColor Cyan }
function Fail($m) { Write-Host "ERROR: $m" -ForegroundColor Red; exit 1 }

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  Fail 'Run this script from an elevated (Administrator) PowerShell.'
}

# --- 5. host video memory ---------------------------------------------------------------
# The kernel driver cannot see the host GPU's memory; Venus can.  Store the device-local heap size in the
# adapter's driver key and restart the adapter once so the driver reports it as dedicated video memory.
function Set-VideoMemory {
  Step 'Reporting host video memory'
  $dev = Get-PnpDevice -PresentOnly -EA SilentlyContinue | ? { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' } | select -First 1
  if (-not $dev -or $dev.Status -ne 'OK') { Write-Host '  adapter not running; skipped' -ForegroundColor Yellow; return }
  $probe = Join-Path $PackageDir 'tools\vgpuvram.exe'
  if (-not (Test-Path $probe)) { Write-Host '  tools\vgpuvram.exe missing; skipped' -ForegroundColor Yellow; return }
  $mb = 0
  for ($i = 0; $i -lt 5 -and $mb -eq 0; $i++) {
    $o = & $probe 2>$null
    if ($LASTEXITCODE -eq 0 -and "$o" -match '^\s*(\d+)') { $mb = [int]$matches[1] } else { Start-Sleep 2 }
  }
  if ($mb -le 0) { Write-Host '  no Venus device answered; skipped' -ForegroundColor Yellow; return }
  $drv = (Get-PnpDeviceProperty -InstanceId $dev.InstanceId -KeyName 'DEVPKEY_Device_Driver').Data
  $key = "HKLM:\SYSTEM\CurrentControlSet\Control\Class\$drv"
  $cur = (Get-ItemProperty $key -EA SilentlyContinue).VideoMemoryMB
  Write-Host "  host GPU memory: $mb MiB"
  if ($cur -eq $mb) { return }
  Set-ItemProperty $key VideoMemoryMB $mb -Type DWord
  Write-Host '  restarting the display adapter to apply it'
  pnputil /restart-device "$($dev.InstanceId)" | Out-Null
}
if ($VramOnly) { Set-VideoMemory; exit 0 }

$sys = Join-Path $PackageDir 'vgpu.sys'
$inf = Join-Path $PackageDir 'vgpu.inf'
if (-not (Test-Path $sys) -or -not (Test-Path $inf)) { Fail "vgpu.sys / vgpu.inf not found in $PackageDir" }

# --- 1. test signing / secure boot ---------------------------------------------------
Step 'Checking boot configuration'
$sb = $null
try { $sb = Confirm-SecureBootUEFI } catch { $sb = $null }
$bcd = (bcdedit /enum '{current}') -join "`n"
$testsigning = $bcd -match 'testsigning\s+Yes'
Write-Host ("  Secure Boot enforced : {0}" -f $(if ($sb -eq $null) {'n/a (legacy BIOS)'} else {$sb}))
Write-Host ("  Test signing enabled : {0}" -f $testsigning)
if (-not $testsigning) {
  if ($sb -eq $true) {
    Fail ("Secure Boot is enforced, so test signing cannot be enabled.`n" +
          "  libvirt host: set <feature enabled='no' name='enrolled-keys'/> in the domain <os><firmware>, or disable Secure Boot in the OVMF menu, then boot again.")
  }
  bcdedit /set testsigning on | Out-Null
  Write-Host 'Test signing enabled. REBOOT, then run this script again.' -ForegroundColor Yellow
  exit 3
}

# --- 2. certificate ------------------------------------------------------------------
Step 'Preparing signing certificate'
$subject = 'CN=vgpu test signing'
# NotBefore check: a certificate created while the guest clock ran ahead is not yet valid and
# Set-AuthenticodeSignature then silently produces no signature.
$cert = Get-ChildItem Cert:\LocalMachine\My | Where-Object { $_.Subject -eq $subject -and $_.NotAfter -gt (Get-Date).AddDays(30) -and $_.NotBefore -lt (Get-Date) } | Select-Object -First 1
if (-not $cert) {
  $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject -CertStoreLocation Cert:\LocalMachine\My `
            -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(5)
}
$cer = Join-Path $env:TEMP 'vgpu-test.cer'
Export-Certificate -Cert $cert -FilePath $cer | Out-Null
Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null

# --- 3. sign + catalog ---------------------------------------------------------------
Step 'Signing driver binary'
$r = Set-AuthenticodeSignature -FilePath $sys -Certificate $cert -HashAlgorithm SHA256
if ($r.Status -ne 'Valid' -and $r.Status -ne 'UnknownError') { Write-Host "  signature status: $($r.Status)" -ForegroundColor Yellow }

Step 'Creating catalog (Inf2Cat)'
$inf2cat = Join-Path $PackageDir 'tools\Inf2Cat.exe'
if (-not (Test-Path $inf2cat)) {
  $inf2cat = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin' -Recurse -Filter Inf2Cat.exe -ErrorAction SilentlyContinue |
             Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $inf2cat) { Fail 'Inf2Cat.exe not found (expected in <package>\tools or a Windows Kits install).' }
Remove-Item (Join-Path $PackageDir 'vgpu.cat') -ErrorAction SilentlyContinue
& $inf2cat /driver:$PackageDir /os:10_X64 /uselocaltime
if ($LASTEXITCODE -ne 0) { Fail "Inf2Cat failed ($LASTEXITCODE). Check the messages above (INF syntax or missing files)." }
$r = Set-AuthenticodeSignature -FilePath (Join-Path $PackageDir 'vgpu.cat') -Certificate $cert -HashAlgorithm SHA256
if ($r.Status -ne 'Valid') { Fail "signing vgpu.cat failed: $($r.Status) $($r.StatusMessage)" }

if ($SkipInstall) { Write-Host 'Package signed (install skipped).' -ForegroundColor Green; exit 0 }

# --- 4. install ----------------------------------------------------------------------
Step 'Installing driver package'
# Re-arm the kernel driver's boot-loop guard: a leftover nonzero VgpuBootGuard makes DriverEntry refuse to load (Code 39).
$svc = 'HKLM:\SYSTEM\CurrentControlSet\Services\viogpu3d'
if (Test-Path $svc) { Set-ItemProperty $svc VgpuBootGuard 0 -Type DWord }
# Stage first, then remove every other package of this driver: Windows keeps the highest DriverVer, so
# installing an older (or differently built) package over a newer one would silently change nothing,
# and each install would leave another copy in the DriverStore.
$out = pnputil /add-driver $inf | Out-String
if ($LASTEXITCODE -ne 0) { Write-Host $out; Fail "pnputil /add-driver failed ($LASTEXITCODE)." }
$mine = if ($out -match 'Published Name:\s+(oem\d+\.inf)') { $matches[1] } else { $null }
$leaf = [regex]::Escape((Split-Path $inf -Leaf))
$old = (pnputil /enum-drivers | Out-String) -split "`r?`n`r?`n" | ? { $_ -match "Original Name:\s+$leaf" } |
       % { if ($_ -match 'Published Name:\s+(oem\d+\.inf)') { $matches[1] } } | ? { $_ -ne $mine }
foreach ($o in $old) { Write-Host "  removing older package $o"; pnputil /delete-driver $o /uninstall /force | Out-Null }
pnputil /add-driver $inf /install
$rc = $LASTEXITCODE
# 259 (ERROR_NO_MORE_ITEMS): package staged but no device took it (already current, or a better-ranked driver).
if ($rc -eq 259) { Write-Host '  package staged; no device was updated' -ForegroundColor Yellow; $rc = 0 }
if ($rc -ne 0 -and $rc -ne 3010) { Fail "pnputil failed ($rc)." }
# Switching from Microsoft Basic Display leaves the adapter at Code 14 (needs restart) while pnputil returns 0.
Start-Sleep 3
if (Get-PnpDevice -PresentOnly -EA SilentlyContinue | ? { $_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' -and $_.Problem -eq 'CM_PROB_NEED_RESTART' }) { $rc = 3010 }
Write-Host ''
Write-Host 'If the virtio-gpu adapter still shows the old driver, open Device Manager > Display adapters,' -ForegroundColor Yellow
Write-Host 'Update driver > Browse my computer > Let me pick > Have Disk, and choose this vgpu.inf.' -ForegroundColor Yellow
if ($rc -eq 3010) {
  Write-Host 'Reboot required to finish the installation.' -ForegroundColor Yellow
  $ps = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
  Set-ItemProperty 'HKLM:\Software\Microsoft\Windows\CurrentVersion\RunOnce' 'vgpu3d-vram' `
    "`"$ps`" -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$PSCommandPath`" -PackageDir `"$PackageDir`" -VramOnly"
} else {
  Set-VideoMemory
}
exit $rc
