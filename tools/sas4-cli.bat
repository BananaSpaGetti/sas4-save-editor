@echo off
setlocal
rem The command line, as a native binary. Runs c\sas4.exe, which needs no Python at all --
rem unlike sas4.bat in the folder above, which starts the Tk editor and does need one.
rem
rem There is no py/python/python3 probe here and no "install Python" branch, because neither
rem has any meaning for a compiled executable: either the .exe is beside this file or it is
rem not, and that is the only thing worth checking.
rem
rem Everything after the command is passed straight through:
rem     sas4-cli.bat view
rem     sas4-cli.bat --file "saves\profile-000.save" view --section skills
rem     sas4-cli.bat give 129 --kind weapon
rem
rem This window stays open whatever happens, so a double-click that fails still shows why
rem rather than closing instantly -- the same reason sas4.bat pauses unconditionally.

cd /d "%~dp0\.."

if not exist "c\sas4.exe" (
    echo Cannot find c\sas4.exe.
    echo.
    echo The zip ships the C sources but not a built binary. Build it once with a C
    echo compiler -- w64devkit or MSYS2 both work:
    echo.
    echo     cd c
    echo     make
    echo.
    echo Or use the Python command line instead, which needs no compiler:
    echo.
    echo     py tools\sas4.py view
    echo.
    pause
    exit /b 1
)

"c\sas4.exe" %*
set "CODE=%ERRORLEVEL%"

echo.
if "%CODE%"=="0" (
    echo Done.
) else (
    echo Exited with code %CODE%. Any detail is in the lines above.
)
pause
exit /b %CODE%
