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

REM --- Гейт предупреждений: компиляция без линковки, падение при любом warning,
REM     error или ненулевом коде компилятора. Отдельный режим (а не -Werror в
REM     обычной сборке), чтобы новая версия компилятора не ломала релизную
REM     сборку, но гейт в CI оставался строгим.
REM     Грабли, на которые я уже наступил: нельзя опираться на «findstr ничего не
REM     нашёл» как на признак успеха — если лог не создан или компилятор не
REM     запустился, это выглядит точно так же. Поэтому проверяются код возврата
REM     КАЖДОЙ компиляции и код возврата самого findstr (2 = файл не открылся).
if /I "%~1"=="--check-warnings" (
  echo === Warning gate: %WARNFLAGS% ===
  REM Лог рядом со скриптом; путь подставляется инлайном (%~dp0...) — переменная,
  REM заданная внутри блока в скобках, раскрылась бы здесь пустой строкой.
  if exist "%~dp0warnings.log" del "%~dp0warnings.log"
  for %%F in (%SRCS% probe_device.cpp) do (
    "%GXX%" -O2 -fno-strict-aliasing -D_WIN32_WINNT=0x0600 %WARNFLAGS% -fsyntax-only "%%F" >> "%~dp0warnings.log" 2>&1
    if errorlevel 1 echo GATE_COMPILE_FAILED %%F>>"%~dp0warnings.log"
  )
  if not exist "%~dp0warnings.log" (
    echo Warning gate FAILED: no compiler output at all — is g++ runnable?
    exit /b 1
  )
  REM Наличие строк вида "file:line:col: warning:" — признак того, что компиляция
  REM действительно прошла и что-то нашла. Отдельно ловим ненулевой код компилятора
  REM и нечитаемый лог: без этого «ничего не скомпилировалось» выглядит как успех.
  findstr /C:"GATE_COMPILE_FAILED" "%~dp0warnings.log" >nul
  if errorlevel 2 (
    echo Warning gate FAILED: cannot read the log at %~dp0warnings.log
    exit /b 1
  )
  if not errorlevel 1 (
    echo Compilation failed for:
    findstr /C:"GATE_COMPILE_FAILED" "%~dp0warnings.log"
    echo --- compiler diagnostics ---
    findstr /C:"error:" "%~dp0warnings.log"
    exit /b 1
  )
  findstr /C:"error:" "%~dp0warnings.log" >nul
  if errorlevel 2 (
    echo Warning gate FAILED: cannot read the log at %~dp0warnings.log
    exit /b 1
  )
  if not errorlevel 1 (
    echo Compilation errors found:
    findstr /C:"error:" "%~dp0warnings.log"
    exit /b 1
  )
  findstr /C:"warning:" "%~dp0warnings.log" >nul
  if errorlevel 2 (
    echo Warning gate FAILED: cannot read the log at %~dp0warnings.log
    exit /b 1
  )
  if not errorlevel 1 (
    echo Warnings found:
    findstr /C:"warning:" "%~dp0warnings.log"
    exit /b 1
  )
  del "%~dp0warnings.log"
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
