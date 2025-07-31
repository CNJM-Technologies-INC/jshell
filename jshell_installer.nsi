!define APPNAME "jshell"
!define COMPANYNAME "CNJMTechnologies INC [https://cnjm-technologies-inc.vercel.app]"
!define DESCRIPTION "Enhanced C++ JShell for Windows"
!define VERSIONMAJOR 0
!define VERSIONMINOR 0
!define VERSIONBUILD 0

RequestExecutionLevel admin
InstallDir "C:\${APPNAME}"  ; Install to root directory
Name "${APPNAME}"
OutFile "${APPNAME}-installer.exe"
Icon "jshell-icon.ico"           ; Installer icon
UninstallIcon "jshell-icon.ico"  ; Uninstaller icon
BrandingText "By Camresh - CNJMTechnologies INC"
!include LogicLib.nsh
!include WinMessages.nsh

Page license
Page directory
Page instfiles

LicenseData "license.txt"



Section "install"
    ; Check if already installed
    ReadRegStr $R0 HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}" "UninstallString"
    ${If} $R0 != ""
        MessageBox MB_YESNO "${APPNAME} is already installed. Uninstall first?" IDYES uninstall_first
        Abort
        uninstall_first:
            ExecWait '$R0'
    ${EndIf}
    
    SetOutPath $INSTDIR
    File "jshell.exe"
    File "jshell-icon.ico"       ; Copy icon to installation directory
    File "license.txt"           ; Copy license to installation directory
    File "INSTALLATION_NOTES.txt" ; Copy installation instructions
    WriteUninstaller "$INSTDIR\uninstall.exe"
    
    ; Create Start Menu folder and shortcuts with icon
    CreateDirectory "$SMPROGRAMS\${APPNAME}"
    CreateShortcut "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk" "$INSTDIR\jshell.exe" "" "$INSTDIR\jshell-icon.ico"
    CreateShortcut "$DESKTOP\${APPNAME}.lnk" "$INSTDIR\jshell.exe" "" "$INSTDIR\jshell-icon.ico"
    
    ; NOTE: PATH is not automatically modified by this installer
    ; Users can manually add C:\jshell to their PATH if desired
    
    ; Registry entries for Add/Remove Programs
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}" "DisplayName" "${APPNAME} - ${DESCRIPTION}"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}" "UninstallString" "$INSTDIR\uninstall.exe"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}" "DisplayIcon" "$INSTDIR\jshell-icon.ico"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}" "Publisher" "${COMPANYNAME}"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}" "DisplayVersion" "${VERSIONMAJOR}.${VERSIONMINOR}.${VERSIONBUILD}"
    WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}" "NoModify" 1
    WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}" "NoRepair" 1
SectionEnd

Section "uninstall"
    ; Remove shortcuts
    Delete "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk"
    RMDir "$SMPROGRAMS\${APPNAME}"
    Delete "$DESKTOP\${APPNAME}.lnk"
    
    ; NOTE: PATH is not modified by this installer
    ; Users must manually remove C:\jshell from PATH if they added it
    
    ; Remove files
    Delete "$INSTDIR\jshell.exe"
    Delete "$INSTDIR\jshell-icon.ico"
    Delete "$INSTDIR\license.txt"
    Delete "$INSTDIR\INSTALLATION_NOTES.txt"
    Delete "$INSTDIR\uninstall.exe"
    RMDir $INSTDIR
    
    ; Remove registry entries
    DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}"
SectionEnd