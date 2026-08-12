@echo off
REM Build KeySidekick with MinGW-w64 g++.
REM Toolchain lookup order: g++/windres on PATH, else C:\MinGW64\bin, else hard error.
setlocal EnableExtensions

REM --- Locate the toolchain: prefer PATH, fall back to C:\MinGW64\bin ---
set "GXX="
set "WINDRES="
for /f "delims=" %%G in ('where g++ 2^>nul') do if not defined GXX set "GXX=%%G"
for /f "delims=" %%W in ('where windres 2^>nul') do if not defined WINDRES set "WINDRES=%%W"
if not defined GXX if exist "C:\MinGW64\bin\g++.exe" set "GXX=C:\MinGW64\bin\g++.exe"
if not defined WINDRES if exist "C:\MinGW64\bin\windres.exe" set "WINDRES=C:\MinGW64\bin\windres.exe"

if not defined GXX (
  echo ERROR: g++ not found. Add MinGW-w64 bin to PATH or install MinGW-w64 to C:\MinGW64\bin.
  exit /b 1
)
if not defined WINDRES (
  echo ERROR: windres not found. Add MinGW-w64 binutils to PATH or install MinGW-w64 to C:\MinGW64\bin.
  exit /b 1
)
echo Using g++: %GXX%
echo Using windres: %WINDRES%

set "DASHBOARD_GENERATOR=%~dp0..\web\generate_dashboard.ps1"

echo === Generating embedded dashboard ===
if /I "%~1"=="--check-dashboard" (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%DASHBOARD_GENERATOR%" -Check
  if errorlevel 1 exit /b 1
  exit /b 0
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%DASHBOARD_GENERATOR%"
if errorlevel 1 (
  echo Dashboard generation FAILED.
  exit /b 1
)

echo === Building resources ===
"%WINDRES%" -O coff -o resources.o resources.rc 2>nul
if errorlevel 1 (
  echo Resources compilation FAILED.
  exit /b 1
)
echo === Building sidekick.exe ===
"%GXX%" -O2 -fno-strict-aliasing -D_WIN32_WINNT=0x0600 -o sidekick.exe sidekick.cpp app_instance.cpp input_ledger.cpp targeted_input.cpp runtime_storage.cpp config_domain_bridge.cpp config_v3.cpp domain_model.cpp windows_targets.cpp http_security.cpp action_parser.cpp resources.o -lsetupapi -lwinusb -luser32 -lws2_32 -lshell32 -lgdi32 -lbcrypt -lole32 -luuid -static
if errorlevel 1 (
  echo Build FAILED.
  exit /b 1
)

echo === Building probe_device.exe ===
"%GXX%" -O2 -o probe_device.exe probe_device.cpp -lsetupapi -lwinusb -static
if errorlevel 1 (
  echo Build probe_device FAILED.
  exit /b 1
)

echo === Building ks_driver.exe ===
"%GXX%" -O2 -municode -D_WIN32_WINNT=0x0600 -o ks_driver.exe ks_driver.cpp -lsetupapi -lnewdev -lshell32 -luser32 -static
if errorlevel 1 (
  echo Build ks_driver FAILED.
  exit /b 1
)
if errorlevel 1 (
  echo Build probe_device FAILED.
  exit /b 1
)

echo.
echo === Build OK ===
echo Copy config.example.ini to config.ini and edit it before running.
endlocal
