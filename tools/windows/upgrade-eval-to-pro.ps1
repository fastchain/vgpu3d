# In-place upgrade of an Enterprise *Evaluation* VM to Windows 11 Pro (newer build) keeping files+apps.
# Evaluation sources only allow clean installs (setup MigChoice 0xC1900204), so the recorded edition is
# switched to Professional before running setup from the regular multi-edition ISO. Result is NOT activated.
param([string]$Iso = 'C:\vgpu\Win11_Client_26300.iso')
$ErrorActionPreference = 'Stop'
$k = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
$k32 = 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows NT\CurrentVersion'

# unmount any other mounted Windows ISO, mount ours
Get-DiskImage -ImagePath (Get-ChildItem C:\vgpu\*.iso).FullName -ErrorAction SilentlyContinue | Where-Object Attached | Dismount-DiskImage | Out-Null
$img = Mount-DiskImage -ImagePath $Iso -PassThru
$d = ($img | Get-Volume).DriveLetter + ':'
"mounted $Iso as $d"
$wim = Get-ChildItem "$d\sources\install.*" | Select -First 1
$pro = Get-WindowsImage -ImagePath $wim.FullName | Where-Object ImageName -eq 'Windows 11 Pro'
if (-not $pro) { throw "no 'Windows 11 Pro' image in $($wim.FullName)" }
"found: $($pro.ImageName) (index $($pro.ImageIndex))"

# back up and switch the recorded edition
reg export 'HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion' C:\vgpu\edition-backup.reg /y | Out-Null
foreach ($key in $k, $k32) {
  Set-ItemProperty $key -Name EditionID -Value 'Professional'
  Set-ItemProperty $key -Name CompositionEditionID -Value 'Enterprise'
  Set-ItemProperty $key -Name ProductName -Value 'Windows 10 Pro'
}
"edition now: " + (Get-ItemProperty $k).EditionID

# generic (non-activating) Windows Pro install key selects the edition
$args = "/auto upgrade /quiet /eula accept /dynamicupdate disable /compat ignorewarning /showoobe none /telemetry disable /pkey VK7JG-NPHTM-C97JM-9MPGT-3V66T"
Set-Content C:\vgpu\setup-args.txt "$d\setup.exe $args"
"setup command: $d\setup.exe $args"
