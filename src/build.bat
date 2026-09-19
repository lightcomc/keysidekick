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

REM --- Источники продукта: один список на сборку и на гейт предупреждений ---
set "SRCS=sidekick.cpp app_instance.cpp input_ledger.cpp targeted_input.cpp runtime_storage.cpp config_domain_bridge.cpp config_v3.cpp domain_model.cpp windows_targets.cpp http_security.cpp action_parser.cpp"

REM --- Предупреждения включены всегда: без них в релиз уже уехали клавиша ';',
REM     которая вообще не инжектилась (строка таблицы была съедена комментарием
REM     с обратным слэшем), и падение на power-resume. missing-field-initializers
REM     подавлен осознанно: это >100 мест вида `= {0}` / `{ sizeof(x) }` по
REM     Win32-структурам, где массовая замена на `{}` обнулила бы, например,
REM     SP_DEVINFO_DATA.cbSize.
set "WARNFLAGS=-Wall -Wextra -Wno-missing-field-initializers"

set "DASHBOARD_GENERATOR=%~dp0..\web\generate_dashboard.ps1"

echo === Generating embedded dashboard ===
if /I "%~1"=="--check-dashboard" (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%DASHBOARD_GENERATOR%" -Check
  if errorlevel 1 exit /b 1
  exit /b 0
)

REM --- Гейт предупреждений: компиляция без линковки, падение при любом warning.
REM     Отдельный режим (а не -Werror в обычной сборке), чтобы новая версия
REM     компилятора не ломала сборку, но гейт в CI оставался строгим.
if /I "%~1"=="--check-warnings" (
  echo === Warning gate: %WARNFLAGS% ===
  REM Путь к логу — через %TEMP% напрямую: переменная, заданная ВНУТРИ блока
  REM в скобках, подставляется только с отложенным расширением, и `%WLOG%` здесь
  REM раскрылся бы в пустую строку (гейт молча ничего не компилировал).
  if exist "%TEMP%\keysidekick_warnings.log" del "%TEMP%\keysidekick_warnings.log"
  for %%F in (%SRCS% probe_device.cpp) do "%GXX%" -O2 -fno-strict-aliasing -D_WIN32_WINNT=0x0600 %WARNFLAGS% -fsyntax-only "%%F" >> "%TEMP%\keysidekick_warnings.log" 2>&1
  findstr /C:"warning:" "%TEMP%\keysidekick_warnings.log" >nul
  if not errorlevel 1 (
    echo Warnings found:
    findstr /C:"warning:" "%TEMP%\keysidekick_warnings.log"
    exit /b 1
  )
  findstr /C:"error:" "%TEMP%\keysidekick_warnings.log" >nul
  if not errorlevel 1 (
    echo Compilation errors found:
    findstr /C:"error:" "%TEMP%\keysidekick_warnings.log"
    exit /b 1
  )
  echo Warning gate OK — no warnings in 12 translation units.
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
"%GXX%" -O2 -fno-strict-aliasing -D_WIN32_WINNT=0x0600 %WARNFLAGS% -o sidekick.exe %SRCS% resources.o -lsetupapi -lwinusb -luser32 -lws2_32 -lshell32 -lgdi32 -lbcrypt -lole32 -luuid -lnewdev -static
if errorlevel 1 (
  echo Build FAILED.
  exit /b 1
)

echo === Building probe_device.exe ===
"%GXX%" -O2 -D_WIN32_WINNT=0x0600 %WARNFLAGS% -o probe_device.exe probe_device.cpp -lsetupapi -lwinusb -static
if errorlevel 1 (
  echo Build probe_device FAILED.
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
