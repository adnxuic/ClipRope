!macro CheckProductRegistration required
  ReadRegStr $0 HKCU "${PRODUCT_KEY}" "InstallLocation"
  ${If} $0 != "$INSTDIR"
  !if ${required} == 0
  ${AndIf} $0 != ""
  !endif
    MessageBox MB_OK|MB_ICONSTOP "The existing ClipRope registration identifies a different installation." /SD IDOK
    SetErrorLevel 4
    Quit
  ${EndIf}
!macroend

!macro CheckRegistration allowDirect
  !insertmacro CheckProductRegistration 0
  ReadRegStr $0 HKCU "${RUN_KEY}" "ClipRope"
  !if ${allowDirect} == 1
    ReadRegStr $1 HKCU "${STARTUP_KEY}" "Owner"
    ${If} $1 != ""
    ${AndIf} $0 == '"$1"'
      StrCpy $0 ""
    ${EndIf}
  !endif
  ${If} $0 != ""
  ${AndIf} $0 != '"$INSTDIR\current\ClipRope.exe"'
    MessageBox MB_OK|MB_ICONSTOP "The current sign-in entry identifies a different installation." /SD IDOK
    SetErrorLevel 4
    Quit
  ${EndIf}
!macroend

; A foreign sign-in entry must not prevent removing this installed payload. It does
; prohibit purging the shared history or removing the shared preview container.
!macro CheckUninstallRegistration
  !insertmacro CheckProductRegistration 1
  StrCpy $foreignStartup 0
  ReadRegStr $0 HKCU "${RUN_KEY}" "ClipRope"
  ${If} $0 != ""
  ${AndIf} $0 != '"$INSTDIR\current\ClipRope.exe"'
    StrCpy $foreignStartup 1
  ${EndIf}
  ReadRegStr $0 HKCU "${STARTUP_KEY}" "Owner"
  ${If} $0 != ""
  ${AndIf} $0 != "$INSTDIR\current\ClipRope.exe"
    StrCpy $foreignStartup 1
  ${EndIf}
!macroend

; Re-read each value at removal time. Neither an absent value nor another copy's
; registration is ours to delete, even when the other value names this executable.
!macro RemoveOwnedRegistration
  ReadRegStr $0 HKCU "${RUN_KEY}" "ClipRope"
  ${If} $0 == '"$INSTDIR\current\ClipRope.exe"'
    DeleteRegValue HKCU "${RUN_KEY}" "ClipRope"
  ${EndIf}
  ReadRegStr $0 HKCU "${STARTUP_KEY}" "Owner"
  ${If} $0 == "$INSTDIR\current\ClipRope.exe"
    DeleteRegValue HKCU "${STARTUP_KEY}" "Owner"
    DeleteRegKey /ifempty HKCU "${STARTUP_KEY}"
  ${EndIf}
  ReadRegStr $0 HKCU "${PRODUCT_KEY}" "InstallLocation"
  ${If} $0 == "$INSTDIR"
    DeleteRegKey HKCU "${PRODUCT_KEY}"
  ${EndIf}
!macroend
