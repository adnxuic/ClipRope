Function un.RemoveShortcut
  StrCpy $2 0
  retryShortcut:
  !insertmacro CheckProductRegistration 1
  System::Call 'kernel32::DeleteFileW(w "${APP_SHORTCUT}") i.r0 ?e'
  Pop $1
  ${If} $0 == 0
  ${AndIf} $1 != 2
  ${AndIf} $1 != 3
    ${If} $1 == 5
    ${OrIf} $1 == 32
    ${OrIf} $1 == 33
      IntOp $2 $2 + 1
      ${If} $2 < 20
        Sleep 100
        Goto retryShortcut
      ${EndIf}
    ${EndIf}
    DetailPrint "The ClipRope Start menu shortcut could not be removed."
    !insertmacro PackageFileInUse
  ${EndIf}
FunctionEnd

Section "Uninstall"
  SetRegView 64
  SetShellVarContext current
  ; NSIS preserves the original uninstaller directory in INSTDIR when running its temp copy.
  ${GetFileName} "$INSTDIR" $0
  ${If} $0 != "current"
    MessageBox MB_OK|MB_ICONSTOP "Run the uninstaller in the registered ClipRope current folder." /SD IDOK
    SetErrorLevel 4
    Quit
  ${EndIf}
  ${GetParent} "$INSTDIR" $0
  StrCpy $INSTDIR $0
  !insertmacro CheckIdle
  !insertmacro CheckUninstallRegistration
  !insertmacro CheckDestination --check-install-tree
  ${If} $0 != 0
    SetErrorLevel 2
    Quit
  ${EndIf}
  !insertmacro CheckSlot current
  !insertmacro CheckSlot previous
  !insertmacro CheckSlot incoming
  ReadINIStr $0 "$INSTDIR\current\release.ini" "ClipRope" "Version"
  StrCmp $0 "" stop
  ${GetParameters} $R0
  ${GetOptions} $R0 "/PURGEHISTORY" $0
  IfErrors 0 purge
  IfSilent retain
  MessageBox MB_YESNOCANCEL|MB_ICONQUESTION|MB_DEFBUTTON1 "Keep your ClipRope history and local backups? Yes keeps them for a later installation. No deletes this profile, including Floating cards and backups." IDYES retain IDNO purge
  Goto stop
  purge:
  !insertmacro CheckUninstallRegistration
  ${If} $foreignStartup == 1
    MessageBox MB_OK|MB_ICONSTOP "The current sign-in entry identifies a different installation." /SD IDOK
    SetErrorLevel 4
    Quit
  ${EndIf}
  !insertmacro Support --confirm-delete-installed-history
  retain:
  !insertmacro CheckUninstallRegistration
  ; The shell can briefly hold the shortcut after setup. Keep a retryable
  ; installation in place if the shortcut remains locked.
  Call un.RemoveShortcut
  !insertmacro CheckUninstallRegistration
  ${If} $foreignStartup == 0
    !insertmacro Support --remove-preview-container
  ${EndIf}
  SetOutPath "$PLUGINSDIR"
  !insertmacro RemovePayload previous
  !insertmacro RemovePayload incoming
  !insertmacro RemovePayload current
  RMDir "$INSTDIR"
  !insertmacro RemoveOwnedRegistration
  Goto complete
  stop:
  SetErrorLevel 5
  complete:
SectionEnd
