@echo off
rem Starts the game with the glitch diagnostics armed (idle until the other .bat files are used).
rem The game log goes to diagnose\deadnation.log.
set "HERE=%~dp0"
del "%HERE%capture.trigger" "%HERE%reload.trigger" 2>nul
> "%HERE%toggles.txt" echo 0
set "DEADNATION_TOGGLE_FILE=%HERE%toggles.txt"
set "DEADNATION_CAPTURE_TRIGGER=%HERE%capture.trigger"
set "DEADNATION_CAPTURE_DIR=%HERE%captures"
set "DEADNATION_RELOAD_TRIGGER=%HERE%reload.trigger"
rem Every routine event in the log too, not only faults and errors.
set "DEADNATION_VERBOSE=1"
start "" /b "%HERE%..\deadnation.exe" %* > "%HERE%deadnation.log" 2>&1
