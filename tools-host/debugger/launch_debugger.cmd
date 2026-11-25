@echo off
rem Launch ix68k-debugger with a Python that has tkinter (Windows version of launch_debugger.sh).
rem Used by ix68k File > Open Debugger... and from a command prompt.
rem Extra arguments are passed to ix68k_debugger.py (--socket PATH / --port PORT).
rem
rem Python used: %IX68K_PYTHON% if set. Otherwise the first of "py -3" and "python"
rem for which "import tkinter" succeeds.
setlocal
set "DEBUGGER=%~dp0ix68k_debugger.py"

if defined IX68K_PYTHON goto run_custom

py -3 -c "import tkinter" >nul 2>&1
if not errorlevel 1 goto run_py

python -c "import tkinter" >nul 2>&1
if not errorlevel 1 goto run_python

echo ix68k-debugger: no Python with tkinter was found. 1>&2
echo   Install Python for Windows with the "tcl/tk and IDLE" option, 1>&2
echo   or set IX68K_PYTHON to the path of a Python that has tkinter. 1>&2
exit /b 1

:run_custom
"%IX68K_PYTHON%" "%DEBUGGER%" %*
exit /b %ERRORLEVEL%

:run_py
py -3 "%DEBUGGER%" %*
exit /b %ERRORLEVEL%

:run_python
python "%DEBUGGER%" %*
exit /b %ERRORLEVEL%
