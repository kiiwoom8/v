@echo off
rem build.bat - double-click to build vim.exe with 32-bit MinGW.
rem Extra arguments go to mingw32-make (e.g. "build.bat probe").
setlocal
cd /d "%~dp0"

where mingw32-make >nul 2>nul || where gcc >nul 2>nul || (
    if exist "C:\MinGW\bin\gcc.exe" set "PATH=C:\MinGW\bin;%PATH%"
)
where mingw32-make >nul 2>nul || where gcc >nul 2>nul || (
    if exist "C:\msys64\mingw32\bin\gcc.exe" set "PATH=C:\msys64\mingw32\bin;%PATH%"
)

where mingw32-make >nul 2>nul
if not errorlevel 1 (
    mingw32-make %*
    goto done
)

where gcc >nul 2>nul
if not errorlevel 1 (
    windres -O coff -i vim.rc -o vim.res.o
    if errorlevel 1 goto done
    gcc -Wall -Wextra -O2 -std=gnu11 -o vim.exe main.c config.c input.c hint.c tray.c uia.c snippet.c update.c vim.res.o -mwindows -lgdi32 -luser32 -lshell32 -ladvapi32 -lwtsapi32 -lwinmm -lole32 -loleaut32 -luuid -lcomdlg32 -lwininet
    goto done
)

echo MinGW was not found. Install 32-bit MinGW (C:\MinGW) or MSYS2 (C:\msys64),
echo or download a ready-built vim.exe from:
echo   https://github.com/kiiwoom8/v/releases/latest
set "RC=1"
goto finish

:done
set "RC=%ERRORLEVEL%"
if "%RC%"=="0" echo Build succeeded: vim.exe
if not "%RC%"=="0" echo Build failed.

:finish
if not defined CI pause
exit /b %RC%
