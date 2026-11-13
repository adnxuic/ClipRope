; Every deletion names a packaged file. Unknown files keep their directory in place.
!macro PackageFileInUse
  MessageBox MB_OK|MB_ICONSTOP "A package file is in use. Close ClipRope and run setup again." /SD IDOK
  SetErrorLevel 4
  Quit
!macroend
!macro DeleteChecked slot name
  Delete "$INSTDIR\${slot}\${name}"
  ${If} ${FileExists} "$INSTDIR\${slot}\${name}"
    !insertmacro PackageFileInUse
  ${EndIf}
!macroend
!macro RemovePayload slot
  !insertmacro DeleteChecked ${slot} ClipRope.exe
  !insertmacro DeleteChecked ${slot} ClipRopeWorker.exe
  !insertmacro DeleteChecked ${slot} ClipRopePreviewWorker.exe
  !insertmacro DeleteChecked ${slot} ClipRopeTargetWorker.exe
  !insertmacro DeleteChecked ${slot} cliprope_headless.exe
  !insertmacro DeleteChecked ${slot} ThirdPartyNotices.txt
  !insertmacro DeleteChecked ${slot} Uninstall.exe
  !insertmacro DeleteChecked ${slot} release.ini
  RMDir "$INSTDIR\${slot}"
  ${If} ${FileExists} "$INSTDIR\${slot}\*.*"
    SetErrorLevel 4
    Quit
  ${EndIf}
!macroend

!macro CheckSlot slot
  ${If} ${FileExists} "$INSTDIR\${slot}\*.*"
    ReadINIStr $0 "$INSTDIR\${slot}\release.ini" "ClipRope" "Version"
    ${If} $0 == ""
      MessageBox MB_OK|MB_ICONSTOP "A package directory was not recognized. Its contents have been preserved." /SD IDOK
      SetErrorLevel 4
      Quit
    ${EndIf}
  ${EndIf}
!macroend
