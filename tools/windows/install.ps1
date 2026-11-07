<#
.SYNOPSIS
  Install the test-signed vgpu driver on this Windows guest.  Run from an elevated PowerShell.

.DESCRIPTION
  1. Checks Secure Boot / test-signing state (test signing needs Secure Boot NOT enforced).
  2. Creates a self-signed code-signing certificate and trusts it (Root + TrustedPublisher).
  3. Signs vgpu.sys, builds vgpu.cat with Inf2Cat, signs the catalog.
  4. Installs the driver package with pnputil.

  Re-run after "bcdedit /set testsigning on" + reboot if step 1 asks for it.
#>
[CmdletBinding()]
param(
  [string]$PackageDir,
  [switch]$SkipInstall
)
$ErrorActionPreference = 'Stop'
if (-not $PackageDir) { $PackageDir = Split-Path -Parent $MyInvocation.MyCommand.Path }
function Step($m) { Write-Host "==> $m" -ForegroundColor Cyan }
function Fail($m) { Write-Host "ERROR: $m" -ForegroundColor Red; exit 1 }

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  Fail 'Run this script from an elevated (Administrator) PowerShell.'
}

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
  exit 0
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
pnputil /add-driver $inf /install
Write-Host ''
Write-Host 'If the virtio-gpu adapter still shows the old driver, open Device Manager > Display adapters,' -ForegroundColor Yellow
Write-Host 'Update driver > Browse my computer > Let me pick > Have Disk, and choose this vgpu.inf.' -ForegroundColor Yellow
Write-Host 'Then run:  .\vgputest.exe -v' -ForegroundColor Green
