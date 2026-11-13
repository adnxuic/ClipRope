Unicode true
!include "LogicLib.nsh"
!include "FileFunc.nsh"
RequestExecutionLevel user
!ifdef CLIPROPE_SOLID_LZMA
  SetCompressor /SOLID lzma
  SetCompressorDictSize 8
!else
  SetCompressor zlib
!endif
Icon "${APP_ICON}"
UninstallIcon "${APP_ICON}"
Name "ClipRope ${VERSION}"
OutFile "${OUTPUT}"
!define /ifndef PACKAGE_LOCAL "$LOCALAPPDATA"
InstallDir "${PACKAGE_LOCAL}\Programs\ClipRope"
ShowInstDetails show
ShowUninstDetails show
Page components
PageEx directory
  PageCallbacks DirectoryPre "" DirectoryLeave
  DirVerify leave
PageExEnd
Page instfiles
UninstPage instfiles
!define /ifndef PRODUCT_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\ClipRope"
!define /ifndef RUN_KEY "Software\Microsoft\Windows\CurrentVersion\Run"
!define /ifndef STARTUP_KEY "Software\ClipRope\Startup"
!define /ifndef APP_SHORTCUT "$SMPROGRAMS\ClipRope.lnk"
!define /ifndef SETUP_MUTEX "Global\ClipRope.Package.Change.v1"
!define /ifndef LISTENER_CLASS "ClipRope.Listener"
Var registeredRoot
Var foreignStartup
Var rollback
Var signIn
Var installedVersion
Var setupMutex

!macro SingleSetup
  System::Call 'kernel32::CreateMutexW(p 0, i 0, w "${SETUP_MUTEX}") p .r0 ?e'
  Pop $1
  ${If} $0 == 0
  ${OrIf} $1 == 183
    MessageBox MB_OK|MB_ICONSTOP "Another ClipRope setup is active. Try again when it finishes." /SD IDOK
    SetErrorLevel 6
    Quit
  ${EndIf}
  StrCpy $setupMutex $0
!macroend

!macro RunSupport arguments
  InitPluginsDir
  SetOutPath "$PLUGINSDIR"
  File /oname=ProfileSupport.exe "${SUPPORT}"
  nsExec::ExecToStack '"$PLUGINSDIR\ProfileSupport.exe" ${arguments}'
  Pop $0
  Pop $1
!macroend

!ifmacrondef Support
!macro Support mode
  !insertmacro RunSupport "${mode}"
  ${If} $0 != 0
    MessageBox MB_OK|MB_ICONSTOP "ClipRope could not safely access its folders. Close ClipRope and check folder permissions and links." /SD IDOK
    SetErrorLevel 2
    Quit
  ${EndIf}
!macroend
!endif

!macro CheckDestination mode
  !insertmacro RunSupport '${mode} "$INSTDIR\." "${PACKAGE_LOCAL}"'
  ${If} $0 != 0
    ${If} $1 == ""
      StrCpy $1 "ClipRope could not verify the installation folder."
    ${EndIf}
    MessageBox MB_OK|MB_ICONSTOP "$1" /SD IDOK
  ${EndIf}
!macroend

Function DirectoryPre
  ${If} $registeredRoot != ""
    Abort
  ${EndIf}
FunctionEnd

Function DirectoryLeave
  !insertmacro CheckDestination --check-new-install
  ${If} $0 != 0
    Abort
  ${EndIf}
FunctionEnd

Function FirstInstallDirectory
  ; NSIS drops /D from CMDLINE and can replace an invalid /D with InstallDir
  ; before .onInit. Recover the final literal argument so preflight sees it.
  System::Call 'kernel32::GetCommandLineW() p .r0'
  System::Call 'shlwapi::StrStrW(p r0, w " /D=") p .r1'
  ${If} $1 != 0
    System::Call '*$1(&w${NSIS_MAX_STRLEN} .r2)'
    StrCpy $INSTDIR $2 "" 4
  ${EndIf}
FunctionEnd

!include "payload.nsh"

!macro CheckIdle
  FindWindow $0 "${LISTENER_CLASS}"
  ${If} $0 != 0
    MessageBox MB_OK|MB_ICONSTOP "Exit ClipRope from its tray menu, then run setup again." /SD IDOK
    SetErrorLevel 3
    Quit
  ${EndIf}
!macroend

!include "registration.nsh"

Section "ClipRope and local recovery tool" app
  SectionIn RO
  SetShellVarContext current
  SetRegView 64
  !insertmacro CheckIdle
  !insertmacro CheckRegistration 1
  ${If} $registeredRoot == ""
    !insertmacro CheckDestination --check-new-install
  ${Else}
    !insertmacro CheckDestination --check-install-tree
  ${EndIf}
  ${If} $0 != 0
    SetErrorLevel 2
    Quit
  ${EndIf}
  ; Store the same spelling that the uninstaller will derive from its current slot.
  StrCpy $0 "$INSTDIR" 1 -1
  ${If} $0 == "\"
    StrCpy $INSTDIR "$INSTDIR" -1
  ${EndIf}
  ClearErrors
  CreateDirectory "$INSTDIR"
  IfErrors unavailable
  !insertmacro CheckSlot current
  !insertmacro CheckSlot previous
  !insertmacro CheckSlot incoming
  ; Recover the last complete package if setup stopped between the two renames.
  IfFileExists "$INSTDIR\current\release.ini" recovered
  IfFileExists "$INSTDIR\previous\release.ini" 0 recovered
  Rename "$INSTDIR\previous" "$INSTDIR\current"
  recovered:
  ${If} $rollback == 1
    IfFileExists "$INSTDIR\previous\release.ini" 0 unavailable
    IfFileExists "$INSTDIR\incoming\*.*" unavailable
    ClearErrors
    Rename "$INSTDIR\current" "$INSTDIR\incoming"
    IfErrors unavailable
    Rename "$INSTDIR\previous" "$INSTDIR\current"
    IfErrors restoreCurrent
    Rename "$INSTDIR\incoming" "$INSTDIR\previous"
    IfErrors 0 register
    Rename "$INSTDIR\current" "$INSTDIR\previous"
    Goto restoreCurrent
    restoreCurrent:
    Rename "$INSTDIR\incoming" "$INSTDIR\current"
    Goto unavailable
  ${EndIf}
  IfFileExists "$INSTDIR\incoming\*.*" 0 extract
  !insertmacro RemovePayload incoming
  IfFileExists "$INSTDIR\incoming\*.*" unavailable
  extract:
  ClearErrors
  SetOutPath "$INSTDIR\incoming"
  IfErrors unavailable
  WriteINIStr "$INSTDIR\incoming\release.ini" "ClipRope" "Version" "${VERSION}"
  WriteINIStr "$INSTDIR\incoming\release.ini" "ClipRope" "Revision" "${REVISION}"
  File "${APP_BIN}\ClipRope.exe"
  File "${APP_BIN}\cliprope_headless.exe"
  !ifdef HAS_WORKER
    File "${APP_BIN}\ClipRopeWorker.exe"
    File "${APP_BIN}\ClipRopePreviewWorker.exe"
    File "${APP_BIN}\ClipRopeTargetWorker.exe"
  !endif
  File /oname=ThirdPartyNotices.txt "${NOTICES}"
  WriteUninstaller "$INSTDIR\incoming\Uninstall.exe"
  IfErrors unavailable
  SetOutPath "$PLUGINSDIR"
  IfFileExists "$INSTDIR\previous\*.*" 0 switchCurrent
  !insertmacro RemovePayload previous
  IfFileExists "$INSTDIR\previous\*.*" unavailable
  switchCurrent:
  IfFileExists "$INSTDIR\current\release.ini" 0 install
  ClearErrors
  Rename "$INSTDIR\current" "$INSTDIR\previous"
  IfErrors unavailable
  install:
  ; A just-uninstalled slot or the freshly extracted image can still have a closing Windows handle.
  ; MoveFile never replaces an existing destination. Retry only transient sharing/access errors.
  StrCpy $2 0
  promoteIncoming:
  System::Call 'kernel32::MoveFileW(w "$INSTDIR\incoming", w "$INSTDIR\current") i.r0 ?e'
  Pop $1
  ${If} $0 != 0
    Goto register
  ${EndIf}
  ${If} $1 == 5
  ${OrIf} $1 == 32
    IntOp $2 $2 + 1
    ${If} $2 < 12
      Sleep 100
      Goto promoteIncoming
    ${EndIf}
  ${EndIf}
  Rename "$INSTDIR\previous" "$INSTDIR\current"
  Goto unavailable
  register:
  ${If} $rollback == 1
    ; Keep the cleanup contract for the startup marker when restoring an older executable.
    ClearErrors
    WriteUninstaller "$INSTDIR\current\Uninstall.exe"
    IfErrors unavailable
  ${EndIf}
  ReadINIStr $installedVersion "$INSTDIR\current\release.ini" "ClipRope" "Version"
  ClearErrors
  SetOutPath "$INSTDIR\current"
  CreateShortcut "${APP_SHORTCUT}" "$INSTDIR\current\ClipRope.exe"
  WriteRegStr HKCU "${PRODUCT_KEY}" "DisplayName" "ClipRope"
  WriteRegStr HKCU "${PRODUCT_KEY}" "DisplayVersion" "$installedVersion"
  WriteRegStr HKCU "${PRODUCT_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${PRODUCT_KEY}" "DisplayIcon" "$INSTDIR\current\ClipRope.exe"
  WriteRegStr HKCU "${PRODUCT_KEY}" "UninstallString" '"$INSTDIR\current\Uninstall.exe"'
  WriteRegStr HKCU "${PRODUCT_KEY}" "QuietUninstallString" '"$INSTDIR\current\Uninstall.exe" /S'
  WriteRegDWORD HKCU "${PRODUCT_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${PRODUCT_KEY}" "NoRepair" 1
  IfErrors unavailable
  Goto done
  unavailable:
  MessageBox MB_OK|MB_ICONSTOP "The package could not be switched. Check available disk space, folder access and the previous package." /SD IDOK
  SetErrorLevel 4
  Quit
  done:
SectionEnd

Section /o "Start ClipRope when I sign in" startup
SectionEnd

Section -SignIn
  ClearErrors
  SectionGetFlags ${startup} $0
  IntOp $0 $0 & 1
  ${If} $0 == 1
    WriteRegStr HKCU "${RUN_KEY}" "ClipRope" '"$INSTDIR\current\ClipRope.exe"'
  ${Else}
    ReadRegStr $0 HKCU "${RUN_KEY}" "ClipRope"
    ClearErrors
    ${If} $0 != ""
      DeleteRegValue HKCU "${RUN_KEY}" "ClipRope"
    ${EndIf}
  ${EndIf}
  IfErrors signInError
  WriteRegStr HKCU "${STARTUP_KEY}" "Owner" "$INSTDIR\current\ClipRope.exe"
  IfErrors signInError signInDone
  signInError:
  MessageBox MB_OK|MB_ICONSTOP "Windows could not save the sign-in settings." /SD IDOK
  SetErrorLevel 4
  Quit
  signInDone:
SectionEnd

Function .onInit
  !insertmacro SingleSetup
  SetRegView 64
  StrCpy $rollback 0
  ${GetParameters} $R0
  ${GetOptions} $R0 "/ROLLBACK" $0
  ${Unless} ${Errors}
    StrCpy $rollback 1
  ${EndUnless}
  ReadRegStr $signIn HKCU "${RUN_KEY}" "ClipRope"
  ReadRegStr $registeredRoot HKCU "${PRODUCT_KEY}" "InstallLocation"
  ${If} $registeredRoot != ""
    ; A registered package always upgrades or rolls back in place, including with /D.
    StrCpy $INSTDIR $registeredRoot
  ${ElseIf} $rollback == 1
    MessageBox MB_OK|MB_ICONSTOP "There is no registered ClipRope installation to roll back." /SD IDOK
    SetErrorLevel 4
    Quit
  ${Else}
    Call FirstInstallDirectory
  ${EndIf}
  ReadRegStr $1 HKCU "${STARTUP_KEY}" "Owner"
  ${If} $registeredRoot == ""
  ${AndIf} $1 == ""
    StrCpy $signIn 1
  ${EndIf}
  ${GetOptions} $R0 "/STARTUP" $0
  ${Unless} ${Errors}
    StrCpy $signIn 1
  ${EndUnless}
  ${GetOptions} $R0 "/NOSTARTUP" $0
  ${Unless} ${Errors}
    StrCpy $signIn ""
  ${EndUnless}
  ${If} $signIn != ""
    SectionSetFlags ${startup} 1
  ${EndIf}
FunctionEnd

Function un.onInit
  !insertmacro SingleSetup
FunctionEnd

!include "uninstall.nsh"
