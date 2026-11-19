@echo off
setlocal
set "SCRIPT_DIR=%~dp0"
set "VENV_PYTHON=%SCRIPT_DIR%.venv\Scripts\python.exe"
if not exist "%VENV_PYTHON%" (
    echo [EOD World Editor] Virtual environment not found at "%VENV_PYTHON%".
    echo Create it with:
    echo   python -m venv "%SCRIPT_DIR%.venv"
    echo   "%SCRIPT_DIR%.venv\Scripts\python.exe" -m pip install PySide6 numpy Pillow PyOpenGL
    pause
    exit /b 1
)
cd /d "%SCRIPT_DIR%"
"%VENV_PYTHON%" "%SCRIPT_DIR%app.py" %*
set "EXIT_CODE=%ERRORLEVEL%"
if not "%EXIT_CODE%"=="0" (
    echo.
    echo [EOD World Editor] Exited with code %EXIT_CODE%.
    pause
)
endlocal
