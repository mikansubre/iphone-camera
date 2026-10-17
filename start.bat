@echo off
rem Keep this file ASCII-only: cmd.exe misparses multibyte (UTF-8) text in .bat files.
cd /d "%~dp0"
if not exist .venv\Scripts\python.exe (
  echo First-time setup: creating virtual environment...
  python -m venv .venv || goto :error
  .venv\Scripts\python -m pip install -r requirements.txt || goto :error
)
.venv\Scripts\python server.py %*
pause
exit /b

:error
echo Setup failed. Make sure Python 3.10+ is installed and on PATH.
pause
