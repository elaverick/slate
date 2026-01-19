; ===================================================================
; Slate Installer - Final Robust Version for Windows 11
; ===================================================================

!include "MUI2.nsh"
!include "nsDialogs.nsh"
!include "LogicLib.nsh"

; --------------------------------------------------
; Metadata & Settings
; --------------------------------------------------
Name "Slate"
OutFile "..\build\SetupSlate.exe"
InstallDir "$LOCALAPPDATA\Slate"
RequestExecutionLevel user ; Essential for HKCU writes

!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT "Open Default Apps settings"
!define MUI_FINISHPAGE_RUN_FUNCTION OpenDefaultApps

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
Page custom SelectExtensionsPage
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_LANGUAGE "English"

; --------------------------------------------------
; Variables
; --------------------------------------------------
Var chk_txt
Var chk_md
Var chk_log
Var chk_ini
Var chk_json
Var chk_xml
Var chk_me
Var chk_csv
Var chk_pem
Var chk_key
Var chk_reg

; ===================================================================
; MACROS
; ===================================================================

!macro RegisterExtension EXTENSION
  ; 1. The Context Menu (Legacy/Show More Options)
  ; Using SystemFileAssociations is the "cleanest" overlay method
  WriteRegStr HKCU "Software\Classes\SystemFileAssociations\${EXTENSION}\shell\OpenWithSlate" "" "Open with Slate"
  WriteRegStr HKCU "Software\Classes\SystemFileAssociations\${EXTENSION}\shell\OpenWithSlate" "Icon" "$INSTDIR\slate.ico"
  WriteRegStr HKCU "Software\Classes\SystemFileAssociations\${EXTENSION}\shell\OpenWithSlate\command" "" '"$INSTDIR\slate.exe" "%1"'

  ; 2. The "Open With" List
  ; This links the extension to our ProgID
  WriteRegStr HKCU "Software\Classes\${EXTENSION}\OpenWithProgids" "SlateFile" ""
  
  ; 3. The Applications Supported Types (Win 10/11 modern check)
  WriteRegStr HKCU "Software\Classes\Applications\slate.exe\SupportedTypes" "${EXTENSION}" ""
!macroend

!macro UnRegisterExtension EXTENSION
  DeleteRegKey HKCU "Software\Classes\SystemFileAssociations\${EXTENSION}\shell\OpenWithSlate"
  DeleteRegValue HKCU "Software\Classes\${EXTENSION}\OpenWithProgids" "SlateFile"
  ; Cleanup legacy broken keys from previous attempts
  DeleteRegKey HKCU "Software\Classes\${EXTENSION}\OpenWithProgids\SlateFile"
!macroend

; --------------------------------------------------
; Custom Page Logic
; --------------------------------------------------
Function SelectExtensionsPage
  nsDialogs::Create 1018
  Pop $0
  ${NSD_CreateLabel} 0 0 100% 25u "Choose which file types should show 'Open with Slate' in the context menu."
  
  ${NSD_CreateCheckbox} 0 30u 45% 12u "Text files (.txt)"
  Pop $chk_txt
  ${NSD_Check} $chk_txt

  ${NSD_CreateCheckbox} 0 45u 45% 12u "Markdown documents (.md)"
  Pop $chk_md
  ${NSD_Check} $chk_md

  ${NSD_CreateCheckbox} 0 60u 45% 12u "Log files (.log)"
  Pop $chk_log
  ${NSD_Check} $chk_log

  ${NSD_CreateCheckbox} 0 75u 45% 12u "Configuration files (.ini)"
  Pop $chk_ini
  ${NSD_Check} $chk_ini

  ${NSD_CreateCheckbox} 0 90u 45% 12u "JSON files (.json)"
  Pop $chk_json
  ${NSD_Check} $chk_json

  ${NSD_CreateCheckbox} 50% 30u 45% 12u "XML files (.xml)"
  Pop $chk_xml
  ${NSD_Check} $chk_xml

  ${NSD_CreateCheckbox} 50% 45u 45% 12u "Text documents (.me)"
  Pop $chk_me
  ${NSD_Check} $chk_me

  ${NSD_CreateCheckbox} 50% 60u 45% 12u "CSV spreadsheets (.csv)"
  Pop $chk_csv
  ${NSD_Check} $chk_csv

  ${NSD_CreateCheckbox} 50% 75u 45% 12u "Certificate files (.pem)"
  Pop $chk_pem
  ${NSD_Check} $chk_pem

  ${NSD_CreateCheckbox} 50% 90u 45% 12u "Private keys (.key)"
  Pop $chk_key
  ${NSD_Check} $chk_key

  ${NSD_CreateCheckbox} 50% 105u 45% 12u "Registry files (.reg)"
  Pop $chk_reg
  ${NSD_Check} $chk_reg

  nsDialogs::Show
FunctionEnd

Function OpenDefaultApps
  ExecShell "open" "ms-settings:defaultapps"
FunctionEnd

; --------------------------------------------------
; Main Installation
; --------------------------------------------------
Section "Install"
  SetOutPath "$INSTDIR"
  File "..\build\slate.exe"
  File "..\resources\slate.ico"

  ; 1. Start Menu & Uninstaller
  CreateDirectory "$SMPROGRAMS\Slate"
  CreateShortCut "$SMPROGRAMS\Slate\Slate.lnk" "$INSTDIR\slate.exe" "" "$INSTDIR\slate.ico"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateShortCut "$SMPROGRAMS\Slate\Uninstall Slate.lnk" "$INSTDIR\Uninstall.exe"

  ; 2. App Paths (Helps Windows find the EXE)
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\App Paths\slate.exe" "" "$INSTDIR\slate.exe"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\App Paths\slate.exe" "Path" "$INSTDIR"

  ; 3. Applications Registration (The "Modern" registration)
  WriteRegStr HKCU "Software\Classes\Applications\slate.exe" "FriendlyAppName" "Slate"
  WriteRegStr HKCU "Software\Classes\Applications\slate.exe\shell\open\command" "" '"$INSTDIR\slate.exe" "%1"'
  WriteRegStr HKCU "Software\Classes\Applications\slate.exe\DefaultIcon" "" "$INSTDIR\slate.ico"

  ; 4. ProgID Registration (The "Identity")
  WriteRegStr HKCU "Software\Classes\SlateFile" "" "Slate Document"
  WriteRegStr HKCU "Software\Classes\SlateFile\DefaultIcon" "" "$INSTDIR\slate.ico"
  WriteRegStr HKCU "Software\Classes\SlateFile\shell\Open\command" "" '"$INSTDIR\slate.exe" "%1"'

  ; 5. Process all extensions
  !insertmacro RegisterExtension ".txt"
  !insertmacro RegisterExtension ".md"
  !insertmacro RegisterExtension ".log"
  !insertmacro RegisterExtension ".ini"
  !insertmacro RegisterExtension ".json"
  !insertmacro RegisterExtension ".xml"
  !insertmacro RegisterExtension ".me"
  !insertmacro RegisterExtension ".csv"
  !insertmacro RegisterExtension ".pem"
  !insertmacro RegisterExtension ".key"
  !insertmacro RegisterExtension ".reg"

  ; 6. CRITICAL: Refresh the Shell
  ; This forces Windows to stop using cached context menu data
  System::Call 'shell32::SHChangeNotify(i, i, i, i) v (0x08000000, 0, 0, 0)'
SectionEnd

; --------------------------------------------------
; Uninstallation
; --------------------------------------------------
Section "Uninstall"
  Delete "$INSTDIR\slate.exe"
  Delete "$INSTDIR\slate.ico"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir /r "$SMPROGRAMS\Slate"

  ; Remove Global Identifiers
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\App Paths\slate.exe"
  DeleteRegKey HKCU "Software\Classes\Applications\slate.exe"
  DeleteRegKey HKCU "Software\Classes\SlateFile"

  ; Remove per-extension registrations
  !insertmacro UnRegisterExtension ".txt"
  !insertmacro UnRegisterExtension ".md"
  !insertmacro UnRegisterExtension ".log"
  !insertmacro UnRegisterExtension ".ini"
  !insertmacro UnRegisterExtension ".json"
  !insertmacro UnRegisterExtension ".xml"
  !insertmacro UnRegisterExtension ".me"
  !insertmacro UnRegisterExtension ".csv"
  !insertmacro UnRegisterExtension ".pem"
  !insertmacro UnRegisterExtension ".key"
  !insertmacro UnRegisterExtension ".reg"

  System::Call 'shell32::SHChangeNotify(i, i, i, i) v (0x08000000, 0, 0, 0)'
  RMDir "$INSTDIR"
SectionEnd