@echo off
cd /d "%~dp0"
if not exist config.ini (
  if exist config.example.ini (
    echo config.ini not found - copying from config.example.ini
    copy config.example.ini config.ini >nul
  ) else (
    echo config.ini and config.example.ini not found. Please create config.ini.
    pause
    exit /b 1
  )
)
echo === KeySidekick ===
echo.
sidekick.exe
pause
