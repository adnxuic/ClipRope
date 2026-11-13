; Compile the actual setup with disposable shell locations. Shared-data cleanup is
; covered by the registration fixture and must never reach the real AppContainer.
!ifdef SHORTCUT_PROBE
  Unicode true
  RequestExecutionLevel user
  SilentInstall silent
  OutFile "${APP_BIN}\ClipRope.exe"
  Section
    WriteINIStr "${FIXTURE}\launch.ini" "Probe" "Executable" "$EXEPATH"
  SectionEnd
!else
  !define PACKAGE_LOCAL "${FIXTURE}\local"
  !define APP_SHORTCUT "${FIXTURE}\ClipRope.lnk"
  !define PRODUCT_KEY "${REGISTRY}\Product"
  !define STARTUP_KEY "${REGISTRY}\Startup"
  !define RUN_KEY "${REGISTRY}\Run"
  !define SETUP_MUTEX "Local\ClipRope.Setup.${TOKEN}"
  !define LISTENER_CLASS "ClipRope.Listener.${TOKEN}"
  !macro Support mode
    DetailPrint "Fixture shared-data action: ${mode}"
  !macroend
  !addincludedir "${SOURCE}\packaging\windows"
  !include "${SOURCE}\packaging\windows\ClipRope.nsi"
!endif
