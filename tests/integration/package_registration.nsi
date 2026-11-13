Unicode true
!include "LogicLib.nsh"
!include "FileFunc.nsh"
RequestExecutionLevel user
SilentInstall silent
SilentUnInstall silent
OutFile "${FIXTURE}\Setup.exe"
!define PRODUCT_KEY "${REGISTRY}\Product"
!define RUN_KEY "${REGISTRY}\Run"
!define STARTUP_KEY "${REGISTRY}\Startup"
!define APP_SHORTCUT "${FIXTURE}\ClipRope.lnk"
Var foreignStartup
!include "${SOURCE}\packaging\windows\registration.nsh"
!include "${SOURCE}\packaging\windows\payload.nsh"
; Exercise the production section with disposable files and registry keys.
; Sentinels replace native helpers: no actual history or AppContainer is used.
!macro CheckIdle
!macroend
!macro Support mode
  FileOpen $R8 "${FIXTURE}\actions.txt" a
  FileWrite $R8 "${mode}$\r$\n"
  FileClose $R8
  !if "${mode}" == "--check-install-tree"
    ReadINIStr $R9 "${FIXTURE}\case.ini" "Fixture" "ChangeOwner"
    ${If} $R9 == 1
      WriteRegStr HKCU "${STARTUP_KEY}" "Owner" "C:\foreign\ClipRope.exe"
    ${EndIf}
  !endif
  !if "${mode}" == "--confirm-delete-installed-history"
    Delete "${FIXTURE}\history.txt"
  !endif
  !if "${mode}" == "--remove-preview-container"
    ReadINIStr $R9 "${FIXTURE}\case.ini" "Fixture" "ChangeBeforeRemoval"
    ${If} $R9 == 1
      WriteRegStr HKCU "${RUN_KEY}" "ClipRope" '"C:\foreign\ClipRope.exe"'
      WriteRegStr HKCU "${STARTUP_KEY}" "Owner" "C:\foreign\ClipRope.exe"
    ${EndIf}
  !endif
!macroend
!macro CheckDestination mode
  !insertmacro Support "${mode}"
  StrCpy $0 0
!macroend
Section
  WriteUninstaller "${FIXTURE}\Uninstall.exe"
SectionEnd
!include "${SOURCE}\packaging\windows\uninstall.nsh"
