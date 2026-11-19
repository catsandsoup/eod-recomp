@echo off
setlocal
set "SCRIPT_DIR=%~dp0"
set "VENV_PYTHONW=%SCRIPT_DIR%.venv\Scripts\pythonw.exe"
if not exist "%VENV_PYTHONW%" (
    echo [EOD World Editor] Virtual environment not found at "%VENV_PYTHONW%".
    echo Run launch_editor_console.bat for setup diagnostics.
    pause
    exit /b 1
)
start "" /D "%SCRIPT_DIR%" "%VENV_PYTHONW%" "%SCRIPT_DIR%app.py" %*
endlocal
