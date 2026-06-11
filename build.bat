@echo off
cd /d "%~dp0"
echo Building MATLAB Background Patcher...

:: Check for MinGW g++
where g++ >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: g++ not found. Please install MinGW-w64 or MSYS2.
    echo   https://www.msys2.org/
    echo   Then run: pacman -S mingw-w64-ucrt-x86_64-gcc
    pause
    exit /b 1
)

g++ -O2 -static -mwindows -municode matlab_bg.cpp -o matlab_bg.exe -luser32 -lgdi32 -lgdiplus -lole32 -lcomctl32 2>&1

if %ERRORLEVEL% EQU 0 (
    echo ==========================================
    echo Build SUCCESS: matlab_bg.exe
    echo ==========================================
    echo.
    echo Usage: matlab_bg.exe [image_path] [opacity] [dimming] [scale_mode]
    echo   Or edit matlab_bg.ini then run without args.
    echo.
) else (
    echo ==========================================
    echo Build FAILED. See errors above.
    echo ==========================================
)
pause