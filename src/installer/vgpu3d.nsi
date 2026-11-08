; vgpu3d-setup.exe: installer for the vgpu3d WDDM driver package (build/build_installer.sh).
; Wraps out/package-kmd3d/vgpu3d and drives install.ps1 (test signing, certificate, catalog, pnputil)
; so the guest needs no manual steps.  makensis -DVERSION=x.y.z -DPKGDIR=<package> -DOUTFILE=<exe>
Unicode true
ManifestDPIAware true
RequestExecutionLevel admin
SetCompressor /SOLID lzma

!include MUI2.nsh
!include LogicLib.nsh
!include x64.nsh
!include WinVer.nsh

!define NAME "vgpu3d"
!define FULLNAME "vgpu3d virtio-gpu 3D driver"
!define UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\vgpu3d"
; the 64-bit tools: this installer is a 32-bit process, so System32 would redirect to SysWOW64
!define PS '"$WINDIR\sysnative\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass'

Name "${FULLNAME}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\vgpu3d"
BrandingText "${NAME} ${VERSION}"
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${FULLNAME}"
VIAddVersionKey "FileDescription" "${FULLNAME} setup"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "MIT / zlib / LGPL components, see README"

Var FinishText

!define MUI_ABORTWARNING
!define MUI_ICON "${NSISDIR}\Contrib\Graphics\Icons\modern-install.ico"
!define MUI_UNICON "${NSISDIR}\Contrib\Graphics\Icons\modern-uninstall.ico"
!define MUI_WELCOMEPAGE_TITLE "${FULLNAME} ${VERSION}"
!define MUI_WELCOMEPAGE_TEXT "Hardware-accelerated Vulkan, Direct3D 9/10/11/12 and OpenGL for Windows 11 guests on QEMU/KVM virtio-gpu (Venus + virgl on the host).$\r$\n$\r$\nSetup will:$\r$\n  - enable test signing if needed (one reboot; Secure Boot must be off)$\r$\n  - create a local code-signing certificate and sign the driver$\r$\n  - install the driver for the virtio-gpu display adapter$\r$\n$\r$\nThe display adapter restarts during installation: apps that use the GPU may close. Save your work first."
!define MUI_FINISHPAGE_TITLE "${FULLNAME}"
!define MUI_FINISHPAGE_TEXT "$FinishText"
!define MUI_FINISHPAGE_TEXT_REBOOT "$FinishText$\r$\n$\r$\nReboot now?"
!define MUI_FINISHPAGE_LINK "Project documentation"
!define MUI_FINISHPAGE_LINK_LOCATION "$INSTDIR\README.txt"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"


Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "${FULLNAME} needs 64-bit Windows 11." /SD IDOK
    Abort
  ${EndIf}
  ${IfNot} ${AtLeastBuild} 22000
    MessageBox MB_ICONEXCLAMATION|MB_OKCANCEL "This driver is built and tested for Windows 11 (build 22000+). Continue anyway?" /SD IDOK IDOK +2
    Abort
  ${EndIf}
  SetRegView 64
  ; virtio-gpu PCI device (1AF4:1050) present?
  nsExec::Exec `${PS} -Command "if (Get-PnpDevice -PresentOnly -EA 0 | ? { $$_.InstanceId -like 'PCI\VEN_1AF4&DEV_1050*' }) { exit 0 } else { exit 1 }"`
  Pop $0
  ${If} $0 != 0
    MessageBox MB_ICONEXCLAMATION|MB_OKCANCEL "No virtio-gpu display adapter (PCI 1AF4:1050) was found.$\r$\n$\r$\nAdd one to the VM (libvirt: <video><model type='virtio'><acceleration accel3d='yes'/></model></video>), then run setup again.$\r$\n$\r$\nInstall the files anyway?" /SD IDCANCEL IDOK +2
    Abort
  ${EndIf}
FunctionEnd

Section "Driver" SecDriver
  SectionIn RO
  SetRegView 64
  SetOutPath "$INSTDIR"
  File /r "${PKGDIR}\*.*"
  File "/oname=README.txt" "${README}"
  WriteUninstaller "$INSTDIR\uninstall.exe"

  WriteRegStr HKLM "${UNINSTKEY}" "DisplayName" "${FULLNAME}"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINSTKEY}" "Publisher" "vgpu3d project"
  WriteRegStr HKLM "${UNINSTKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayIcon" "$INSTDIR\uninstall.exe"
  WriteRegStr HKLM "${UNINSTKEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKLM "${UNINSTKEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegDWORD HKLM "${UNINSTKEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINSTKEY}" "NoRepair" 1
  WriteRegDWORD HKLM "${UNINSTKEY}" "EstimatedSize" ${SIZEKB}

  DetailPrint "Signing and installing the driver package..."
  nsExec::ExecToLog `${PS} -File "$INSTDIR\install.ps1" -PackageDir "$INSTDIR"`
  Pop $0
  DetailPrint "install.ps1 exit code: $0"
  ${If} $0 == 0
    StrCpy $FinishText "The driver is installed.$\r$\n$\r$\nCheck Device Manager > Display adapters: it should list 'vgpu VirtIO GPU 3D (WDDM)'."
  ${ElseIf} $0 == 3010
    StrCpy $FinishText "The driver is installed. Windows needs a reboot to start it."
    SetRebootFlag true
  ${ElseIf} $0 == 3
    ; test signing was just switched on: finish the install at the next logon
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\RunOnce" "vgpu3d-setup" `${PS} -NoExit -File "$INSTDIR\install.ps1" -PackageDir "$INSTDIR"`
    StrCpy $FinishText "Test signing has been enabled. Reboot now: the driver installation continues automatically in a console window when you log on again."
    SetRebootFlag true
  ${Else}
    StrCpy $FinishText "Driver installation FAILED (code $0). Show details on the previous page for the messages, fix the cause and run setup again.$\r$\n$\r$\nCommon cause: Secure Boot is enabled, so test signing cannot be turned on."
    SetDetailsView show
    MessageBox MB_ICONSTOP "Driver installation failed (code $0). See the details log." /SD IDOK
    SetErrorLevel 1
  ${EndIf}
  nsExec::ExecToLog `${PS} -Command "Get-PnpDevice -Class Display -PresentOnly | Format-Table FriendlyName, Status -AutoSize | Out-String -Width 120"`
  Pop $0
SectionEnd

Section "Uninstall"
  SetRegView 64
  DetailPrint "Removing the vgpu3d driver packages (the adapter falls back to Microsoft Basic Display)..."
  nsExec::ExecToLog `${PS} -File "$INSTDIR\uninstall3d.ps1" -RemoveCert`
  Pop $0
  DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\RunOnce" "vgpu3d-setup"
  DeleteRegKey HKLM "${UNINSTKEY}"
  ; in-use DLLs (loaded by running apps) go at the next reboot
  RMDir /r /REBOOTOK "$INSTDIR"
  DetailPrint "Test signing is still on; run 'bcdedit /set testsigning off' to disable it."
SectionEnd
