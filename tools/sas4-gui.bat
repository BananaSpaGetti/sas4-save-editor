@echo off
setlocal
rem Start the profile editor with a double click.
rem
rem This window stays open whatever happens. The previous version only paused when the
rem editor exited non-zero, so anything that failed while still returning 0 -- notably a
rem `py` that is Windows' placeholder for "Python is not installed yet" -- closed the
rem window instantly and told the person nothing.
rem
rem Pass a save file to edit something other than the live profile:
rem     sas4-gui.bat saves\profile-000.save

cd /d "%~dp0"

if not exist "sas4_gui.py" (
    echo Cannot find sas4_gui.py next to this file.
    echo.
    echo This usually means the .bat was started from inside the .zip. Windows will happily
    echo open a file previewed in a zip, but the rest of the folder is not there.
    echo.
    echo Extract the whole folder somewhere first -- Desktop or Downloads is fine -- then
    echo run sas4-gui.bat from the extracted copy.
    echo.
    pause
    exit /b 1
)

rem Find a Python that actually runs. `py` is the launcher and the usual answer, but it is
rem not always installed, and on a machine with no Python at all `py` and `python` can be
rem Windows' App Execution Aliases, which open the Microsoft Store instead of running
rem anything. Requiring each candidate to execute a statement first rules those out.
set "PYEXE="
for %%C in (py python python3) do (
    if not defined PYEXE (
        %%C -c "import sys" >nul 2>&1 && set "PYEXE=%%C"
    )
)

if not defined PYEXE (
    echo No working Python was found on this machine.
    echo.
    echo Install Python 3 from https://www.python.org/downloads/ and tick
    echo   [x] Add python.exe to PATH
    echo in the first screen of the installer. Then run this file again.
    echo.
    pause
    exit /b 1
)

echo Starting the editor with %PYEXE% . . .
echo.
%PYEXE% sas4_gui.py %*
set "CODE=%ERRORLEVEL%"

echo.
if "%CODE%"=="0" (
    echo The editor closed.
) else (
    echo The editor exited with code %CODE%. Any detail is in the lines above.
)
pause
exit /b %CODE%
