@echo off
rem ============================================================================
rem  Workaday - build the Watchy 2.0 firmware and flash it to the watch.
rem
rem  Double-click it, or run it from a prompt:
rem
rem    flash.bat              build, then upload to whichever port PlatformIO finds
rem    flash.bat COM7         same, but say which port
rem    flash.bat monitor      upload, then stay in the serial monitor
rem    flash.bat test         run the host gate first, and stop if it fails
rem
rem  The words combine in any order:  flash.bat test COM7 monitor
rem
rem  This is a convenience wrapper, not a second build system. It runs the same
rem  pio commands README.md documents, from this directory, and hands their exit
rem  code back. If it ever disagrees with platformio.ini, platformio.ini is right.
rem ============================================================================
setlocal enabledelayedexpansion

rem pio needs platformio.ini beside it, so ignore the caller's directory.
cd /d "%~dp0"

rem Double-clicked? Then the window must not vanish with the result still in it.
set "HOLD="
echo "%cmdcmdline%" | find /i "%~nx0" >nul
if not errorlevel 1 set "HOLD=1"

set "PORT="
set "MONITOR="
set "TEST="

:parse
if "%~1"=="" goto parsed
set "ARG=%~1"
if /i "!ARG!"=="monitor" (
    set "MONITOR=1"
) else if /i "!ARG!"=="-m" (
    set "MONITOR=1"
) else if /i "!ARG!"=="test" (
    set "TEST=1"
) else if /i "!ARG!"=="-t" (
    set "TEST=1"
) else if /i "!ARG!"=="-h" (
    goto usage
) else if /i "!ARG!"=="--help" (
    goto usage
) else if "!ARG!"=="/?" (
    goto usage
) else if /i "!ARG:~0,3!"=="COM" (
    set "PORT=!ARG!"
) else (
    echo.
    echo   Do not know what "!ARG!" means.
    set "BADARG=1"
    goto usage
)
shift
goto parse
:parsed

rem PlatformIO installed by the VS Code extension is not on PATH, and on Windows
rem that is the ordinary way to have it - so look there before giving up.
set "PIO=pio"
where pio >nul 2>&1
if errorlevel 1 (
    set "PIO=%USERPROFILE%\.platformio\penv\Scripts\pio.exe"
    if not exist "!PIO!" (
        echo.
        echo   PlatformIO is not on PATH, and there is nothing at
        echo     %USERPROFILE%\.platformio\penv\Scripts\pio.exe
        echo.
        echo   Install it from https://platformio.org/install, or open this folder
        echo   in VS Code once and let the PlatformIO extension do it for you.
        goto fail
    )
)

rem A failed 'where' leaves errorlevel at 1, and nothing between here and the
rem next external command would clear it. Today the next command is pio itself,
rem which sets its own - but a line added above this one must not be able to make
rem a good build look like a failed one.
ver >nul

if defined TEST (
    echo.
    echo === host tests ===========================================================
    "!PIO!" test -e native
    if errorlevel 1 (
        echo.
        echo   The host gate failed. Not flashing a build that does not pass it.
        goto fail
    )
)

echo.
echo === build and upload =====================================================
if defined PORT (
    "!PIO!" run -e watchy_v20 -t upload --upload-port !PORT!
) else (
    "!PIO!" run -e watchy_v20 -t upload
)
if errorlevel 1 (
    echo.
    echo   That failed - read the output above for which half. If the compile was
    echo   fine and esptool simply could not reach the board:
    echo.
    echo     - the USB cable has to carry data, not only power. That is the usual one.
    echo     - close anything already holding the port: a serial monitor, the
    echo       PlatformIO monitor in VS Code, a terminal program.
    echo     - press the reset button on the back of the watch, then run this again.
    echo     - name the port yourself:  flash.bat COM7
    goto fail
)

echo.
echo   Flashed. The watch resets itself and redraws in a second or two.

if defined MONITOR (
    echo.
    echo === serial monitor - Ctrl+C to leave =====================================
    echo   Quiet by design: WORKADAY_DIAG is 0 in platformio.ini, so nothing but a
    echo   panic prints here, and a panic arrives already decoded.
    echo.
    if defined PORT (
        "!PIO!" device monitor -p !PORT!
    ) else (
        "!PIO!" device monitor
    )
)

if defined HOLD pause
endlocal
exit /b 0

:usage
echo.
echo   flash.bat [COMx] [test] [monitor]
echo.
echo     COMx      upload to that port instead of letting PlatformIO choose
echo     test      run pio test -e native first, and stop if it fails
echo     monitor   open the serial monitor once the watch has been flashed
echo.
echo   With no arguments: build the firmware and upload it.
echo.
rem Asking for the usage is not a failure; being handed a word nobody knows is.
if not defined BADARG (
    if defined HOLD pause
    endlocal
    exit /b 0
)

:fail
if defined HOLD pause
endlocal
exit /b 1
