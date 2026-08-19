@echo off
rem Start the profile editor with a double click.
rem
rem `py` is the Python launcher, which is the one that works on this machine -- a bare
rem `python` is the Microsoft Store stub and fails with "Python was not found".
rem
rem Pass a save file to edit something other than the live profile:
rem     sas4-gui.bat saves\profile-000.save

cd /d "%~dp0"
py sas4_gui.py %*
if errorlevel 1 (
    echo.
    echo The editor exited with an error. The window above has the detail.
    pause
)
