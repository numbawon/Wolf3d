@echo off
rem w3d_armasm64_wrapper.bat - stand-in for armasm64.exe in the Vulkan-Loader
rem build. armasm64 has no C preprocessor; the loader's MARMASM assembly
rem branches on `#if defined(_ARM64EC_)`. This preprocesses with
rem w3d_masm_preprocess.py (native arm64: no defines), then runs the real
rem armasm64 with the original arguments.
rem Two call forms are handled:
rem   -o OUT IN   (CMake build rule)
rem   IN          (loader configure check; armasm64 default output used)
rem NOTE: capture the script dir before any shift; cmd on this host rotates
rem %0 through the shift window, so %~dp0 after a shift is unreliable.
setlocal
set "SCRIPTDIR=%~dp0"
set "PASS="
set "OUT="
set "IN="
set "LAST="
:parse
if "%~1"=="" goto run
if not "%~1"=="-o" goto other
if defined LAST set "PASS=%PASS% %LAST%"
set "OUT=%~2"
shift
shift
set "IN=%~1"
goto run
:other
if defined LAST set "PASS=%PASS% %LAST%"
set "LAST=%~1"
shift
goto parse
:run
if defined OUT goto have_out
rem no -o: mimic armasm64 default (source basename + .obj next to source)
set "IN=%LAST%"
for %%F in ("%LAST%") do set "OUT=%%~dpF%%~nF.obj"
:have_out
set "PP=%SCRIPTDIR%w3d_masm_preprocess.py"
for /f "delims=" %%i in ('where armasm64 2^>nul') do (set "REAL=%%i" & goto found)
echo w3d: armasm64 not found on PATH
exit /b 1
:found
python "%PP%" "%IN%" "%IN%.w3dpp"
if not "%errorlevel%"=="0" exit /b 1
"%REAL%" %PASS% -o "%OUT%" "%IN%.w3dpp"
set "RC=%errorlevel%"
del "%IN%.w3dpp" >nul 2>&1
exit /b %RC%
