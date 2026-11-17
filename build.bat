@echo off
rem Cairn build script for Windows. No make required.
rem
rem   build.bat         build build\cairn.exe
rem   build.bat test    build and run the core tests (build\core_test.exe)
rem   build.bat clean   delete the build folder
rem
rem Needs MinGW-w64 (gcc, g++, windres) on PATH. Easiest install:
rem   winget install -e --id BrechtSanders.WinLibs.POSIX.UCRT
rem then open a NEW terminal so PATH updates.

setlocal
cd /d "%~dp0"

if /i "%1"=="clean" (
  if exist build rmdir /s /q build
  echo Cleaned.
  exit /b 0
)

for %%T in (gcc g++ windres) do (
  %%T --version >nul 2>nul
  if errorlevel 1 (
    set MISSING=%%T
    goto :nocompiler
  )
)

if not exist build mkdir build

set SQLFLAGS=-O2 -DSQLITE_THREADSAFE=0 -DSQLITE_ENABLE_FTS5 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_DEPRECATED -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0
set INCS=-Isrc/core -Isrc/win -Ivendor/sqlite
set CORE=src\core\document.cpp src\core\model.cpp src\core\rich.cpp src\core\export.cpp src\core\markdown.cpp src\core\table.cpp src\core\media.cpp src\core\script.cpp src\core\arrange.cpp

rem SQLite is large; compile it once and reuse the object file.
if not exist build\sqlite3.o (
  echo [1/3] Compiling SQLite ^(one time only, may take a minute^)...
  gcc %SQLFLAGS% -c vendor\sqlite\sqlite3.c -o build\sqlite3.o
  if errorlevel 1 goto :fail
) else (
  echo [1/3] SQLite already compiled.
)

if /i "%1"=="test" (
  echo [2/3] Building core tests...
  g++ -std=c++17 -O1 -Wall -Wextra -Isrc/core -Ivendor/sqlite tests\core_test.cpp %CORE% build\sqlite3.o -o build\core_test.exe
  if errorlevel 1 goto :fail
  echo [3/3] Running tests...
  build\core_test.exe
  exit /b %errorlevel%
)

echo [2/3] Compiling resources...
windres -O coff -Ires res\cairn.rc -o build\cairn_res.o
if errorlevel 1 goto :fail

echo [3/3] Compiling and linking Cairn...
g++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter %INCS% -DUNICODE -D_UNICODE -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 %CORE% src\win\main.cpp src\win\canvas.cpp src\win\propform.cpp src\win\images.cpp build\sqlite3.o build\cairn_res.o -o build\cairn.exe -static -mwindows -municode -lcomctl32 -lcomdlg32 -lshell32 -lgdi32 -luser32 -lole32 -lwindowscodecs -luuid -lmsimg32 -lwinmm
if errorlevel 1 goto :fail

if not exist build\cairn.exe goto :fail
echo.
echo Done: build\cairn.exe
exit /b 0

:fail
echo.
echo BUILD FAILED. Scroll up for the first error message.
exit /b 1

:nocompiler
echo ERROR: %MISSING% was not found on PATH.
echo Install MinGW-w64, for example:
echo   winget install -e --id BrechtSanders.WinLibs.POSIX.UCRT
echo Then open a NEW terminal and run build.bat again.
exit /b 1
