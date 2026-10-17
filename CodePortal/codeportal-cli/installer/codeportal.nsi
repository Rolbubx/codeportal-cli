; CodePortal installer (NSIS 3).
;
; Build (from CodePortal/codeportal-cli):
;   makensis -DVERSION=1.3.0 installer/codeportal.nsi
; Optional: -DEXE=path\to\codeportal.exe  -DPAGE=path\to\codeportal-chat.html  -DOUTFILE=path\to\setup.exe
;
; Installs for the current user only (no administrator rights needed), by default in
; %LOCALAPPDATA%\Programs\CodePortal, so `codeportal --update` can later replace the exe without a UAC prompt.
; Settings, API keys and memory (%APPDATA%\CodePortal) are never touched, not even by the uninstaller.

Target amd64-unicode  ; CodePortal est un programme 64 bits : l'installeur aussi
SetCompressor /SOLID lzma
RequestExecutionLevel user

!ifndef VERSION
  !error "Pass the version: makensis -DVERSION=1.3.0 installer/codeportal.nsi"
!endif
!ifndef EXE
  !define EXE "..\build\codeportal.exe"
!endif
!ifndef PAGE
  !define PAGE "..\web\codeportal-chat.html"
!endif
!ifndef OUTFILE
  !define OUTFILE "..\build\CodePortal-Setup-${VERSION}.exe"
!endif

!define APP "CodePortal"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\CodePortal"
!define REPO_URL "https://github.com/Rolbubx/codeportal-cli"

Name "${APP} ${VERSION}"
OutFile "${OUTFILE}"
InstallDir "$LOCALAPPDATA\Programs\CodePortal"
InstallDirRegKey HKCU "${UNINST_KEY}" "InstallLocation"
BrandingText "${APP} ${VERSION}"

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${APP}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "FileDescription" "${APP} installer"
VIAddVersionKey "CompanyName" "${APP}"
VIAddVersionKey "LegalCopyright" "${APP}"

!include "MUI2.nsh"
!include "LogicLib.nsh"

!define MUI_ICON "..\res\codeportal.ico"
!define MUI_UNICON "..\res\codeportal.ico"
!define MUI_WELCOMEFINISHPAGE_BITMAP "welcome.bmp"
!define MUI_UNWELCOMEFINISHPAGE_BITMAP "welcome.bmp"
!define MUI_HEADERIMAGE
!define MUI_HEADERIMAGE_RIGHT
!define MUI_HEADERIMAGE_BITMAP "header.bmp"
!define MUI_ABORTWARNING
!define MUI_COMPONENTSPAGE_SMALLDESC

!define MUI_WELCOMEPAGE_TITLE "Welcome to ${APP} ${VERSION}"
!define MUI_WELCOMEPAGE_TEXT "${APP} is a coding agent for your terminal: it reads and edits your files, runs commands and searches the web for you.$\r$\n$\r$\nThis wizard installs it for your user account. No administrator rights are needed.$\r$\n$\r$\nClick Next to continue."
!insertmacro MUI_PAGE_WELCOME

!define MUI_DIRECTORYPAGE_TEXT_TOP "Choose the folder where ${APP} will be installed.$\r$\n$\r$\nKeep a folder you own (the default is fine) so that codeportal --update can update it without administrator rights."
!insertmacro MUI_PAGE_DIRECTORY

!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES

!define MUI_FINISHPAGE_TITLE "${APP} is installed"
!define MUI_FINISHPAGE_TEXT "Open a new terminal in your project folder and type:$\r$\n$\r$\n    codeportal$\r$\n$\r$\nOn first launch, paste an API key (OpenAI, Anthropic, Gemini, Mistral...).$\r$\n$\r$\nTo update later:  codeportal --update"
!define MUI_FINISHPAGE_LINK "Documentation on GitHub"
!define MUI_FINISHPAGE_LINK_LOCATION "${REPO_URL}#readme"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; ---------------------------------------------------------------------------
Section "CodePortal (required)" SecCore
  SectionIn RO
  SetOutPath "$INSTDIR"
  File "/oname=codeportal.exe" "${EXE}"
  Delete "$INSTDIR\codeportal.old.exe"
  SetOutPath "$INSTDIR\web"
  File "/oname=codeportal-chat.html" "${PAGE}"

  SetOutPath "$INSTDIR"
  WriteUninstaller "$INSTDIR\uninstall.exe"

  WriteRegStr HKCU "${UNINST_KEY}" "DisplayName" "${APP}"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINST_KEY}" "Publisher" "${APP}"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\codeportal.exe"
  WriteRegStr HKCU "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINST_KEY}" "URLInfoAbout" "${REPO_URL}"
  WriteRegStr HKCU "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKCU "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1
  WriteRegDWORD HKCU "${UNINST_KEY}" "EstimatedSize" 6000
SectionEnd

Section "Add to PATH (type codeportal in any terminal)" SecPath
  nsExec::ExecToLog '"$INSTDIR\codeportal.exe" --add-to-path'
  Pop $0
  ${If} $0 != 0
    DetailPrint "Could not add $INSTDIR to PATH (code $0)."
  ${EndIf}
SectionEnd

Section "Start Menu shortcut" SecStartMenu
  SetOutPath "$PROFILE"  ; dossier de travail du raccourci
  CreateShortcut "$SMPROGRAMS\${APP}.lnk" "$INSTDIR\codeportal.exe" "" "$INSTDIR\codeportal.exe" 0
SectionEnd

Section /o "Desktop shortcut" SecDesktop
  SetOutPath "$PROFILE"
  CreateShortcut "$DESKTOP\${APP}.lnk" "$INSTDIR\codeportal.exe" "" "$INSTDIR\codeportal.exe" 0
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecCore} "codeportal.exe and the web chat page."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecPath} "Lets you start CodePortal by typing codeportal in any new terminal."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecStartMenu} "A CodePortal entry in the Start Menu."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "A CodePortal icon on the desktop."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; ---------------------------------------------------------------------------
Section "Uninstall"
  nsExec::ExecToLog '"$INSTDIR\codeportal.exe" --remove-from-path'
  Pop $0
  Delete "$SMPROGRAMS\${APP}.lnk"
  Delete "$DESKTOP\${APP}.lnk"
  Delete "$INSTDIR\codeportal.exe"
  Delete "$INSTDIR\codeportal.old.exe"
  Delete "$INSTDIR\codeportal.new.exe"
  Delete "$INSTDIR\web\codeportal-chat.html"
  RMDir "$INSTDIR\web"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
  DeleteRegKey HKCU "${UNINST_KEY}"
SectionEnd
