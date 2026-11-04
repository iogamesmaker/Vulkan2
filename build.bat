@echo off
rem Windows / MSVC build. Usage:
rem   build.bat            -> RelWithDebInfo (default)
rem   build.bat Debug      -> Debug
rem   build.bat Release clean
rem Output: Vulkan2.exe in the project root.
setlocal

set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=RelWithDebInfo
set BUILD_DIR=build-msvc

if "%VCPKG_ROOT%"=="" set VCPKG_ROOT=C:\code\clone\vcpkg

rem load the MSVC x64 toolchain into this shell if it isn't already
rem (no if-blocks here: the ")" in "Program Files (x86)" closes them early)
where cl >nul 2>nul
if not errorlevel 1 goto :have_msvc
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
rem -all: also accept installs the installer flags as incomplete (isComplete 0) that still have a working vcvars64
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -all -products * -property installationPath`) do if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" set "VSDIR=%%i"
if not defined VSDIR echo MSVC not found, install the Visual Studio Build Tools (C++ workload) & exit /b 1
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
:have_msvc

if /i "%2"=="clean" rmdir /s /q %BUILD_DIR%

if not exist %BUILD_DIR%\build.ninja (
    cmake -B %BUILD_DIR% -G Ninja ^
      -DCMAKE_BUILD_TYPE=%CONFIG% ^
      -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl ^
      -DVCPKG_TARGET_TRIPLET=x64-windows-static ^
      -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" ^
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON || exit /b 1
) else (
    cmake -B %BUILD_DIR% -DCMAKE_BUILD_TYPE=%CONFIG% || exit /b 1
)

cmake --build %BUILD_DIR% || exit /b 1
echo.
echo Built Vulkan2.exe (%CONFIG%)
