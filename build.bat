@echo off
rem Build XPico Code: compile with Tiny C Compiler, then embed icon, version info and manifest.
rem
rem   build.bat [win95] [path\to\tcc.exe]
rem
rem Plain, this builds build\xpico-code.exe, which wants Windows 2000 or higher. With "win95"
rem it defines WIN95 and compiles src\win95.c in as well, producing build\xpico-code-win95.exe,
rem which also runs on Windows 95 -- see the Windows 95 section at the foot of src\xpcode.h for
rem what that switch changes. The two can be built side by side; they are different files.
rem
rem Needs Tiny C Compiler 0.9.27 with the "winapi-full" headers, and Python 3.4+ (for the
rem resource step). If no compiler path is given, tcc.exe is taken from PATH.
setlocal
cd /d "%~dp0"

set TARGET=win32
if /i "%~1"=="win95" set TARGET=win95
if "%TARGET%"=="win95" shift

set TCC=%~1
if "%TCC%"=="" for %%i in (tcc.exe) do set TCC=%%~$PATH:i
if "%TCC%"=="" if exist "C:\dev\tools\tcc\tcc.exe" set TCC=C:\dev\tools\tcc\tcc.exe
if "%TCC%"=="" (
    echo Tiny C Compiler not found. Put tcc.exe on PATH or run: build.bat C:\path\to\tcc.exe
    exit /b 1
)

set EXE=build\xpico-code.exe
set DEFS=
set SRC95=
if "%TARGET%"=="win95" set EXE=build\xpico-code-win95.exe
if "%TARGET%"=="win95" set DEFS=-DWIN95
if "%TARGET%"=="win95" set SRC95=src\win95.c

if not exist build\lib mkdir build\lib

rem Import definitions for system DLLs that TCC does not ship.
for %%d in (comctl32 comdlg32 shell32 ole32) do (
    if not exist build\lib\%%d.def "%TCC%" -impdef %SystemRoot%\system32\%%d.dll -o build\lib\%%d.def
)

rem A running build\xpico-code.exe cannot be overwritten but can be renamed, so XPico Code can rebuild
rem itself from its own terminal. Move the old copy to %TEMP% (same drive, so it is just a rename).
del /q "%TEMP%\xpcode-*.old" >NUL 2>&1
if exist "%EXE%" move /y "%EXE%" "%TEMP%\xpcode-%RANDOM%.old" >NUL

echo Compiling XPico Code (%TARGET%)...
"%TCC%" -Wl,-subsystem=windows %DEFS% ^
    src\main.c src\editor.c src\explorer.c src\terminal.c src\palette.c %SRC95% ^
    build\lib\comctl32.def build\lib\comdlg32.def build\lib\shell32.def build\lib\ole32.def ^
    -o "%EXE%"
if errorlevel 1 goto failed

echo Embedding resources...
python tools\embed_resources.py "%EXE%"
if errorlevel 1 goto failed

echo Build succeeded: %CD%\%EXE%
exit /b 0

:failed
echo Build FAILED
exit /b 1
