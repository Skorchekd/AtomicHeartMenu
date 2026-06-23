@echo off
REM One-click build. Produces bin\AtomicHeartMenu.dll
setlocal
cd /d "%~dp0"

echo === Configuring (VS 2022, x64) ===
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
if errorlevel 1 goto :err

echo === Building (Release) ===
cmake --build build --config Release
if errorlevel 1 goto :err

echo.
echo === DONE ===
echo Output: %~dp0bin\AtomicHeartMenu.dll
goto :eof

:err
echo.
echo BUILD FAILED - see messages above.
exit /b 1
