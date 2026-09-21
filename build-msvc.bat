@echo off
setlocal

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Visual Studio C++ Build Tools were not found.
    exit /b 1
)

set "VSINSTALL="
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%I"
if not defined VSINSTALL (
    echo A Visual Studio installation with the C++ toolset was not found.
    exit /b 1
)

call "%VSINSTALL%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b 1

if not exist bin mkdir bin

set "VSCMAKE=%VSINSTALL%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "VSNINJA=%VSINSTALL%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
if not exist "%VSCMAKE%" (
    echo Visual Studio CMake was not found.
    exit /b 1
)
set "PATH=%VSNINJA%;%PATH%"

"%VSCMAKE%" -S . -B build\mcu-save-transfer-msvc -G Ninja -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 (
    echo.
    echo CMake configure failed. Check that third_party\wxWidgets and third_party\mbedtls are initialized.
    exit /b 1
)

"%VSCMAKE%" --build build\mcu-save-transfer-msvc
if errorlevel 1 exit /b 1

if exist build\mcu-save-transfer-msvc\Release\MCU_Save_Transfer.exe (
    copy /Y build\mcu-save-transfer-msvc\Release\MCU_Save_Transfer.exe bin\MCU_Save_Transfer.exe >nul
)
if exist build\mcu-save-transfer-msvc\MCU_Save_Transfer.exe (
    copy /Y build\mcu-save-transfer-msvc\MCU_Save_Transfer.exe bin\MCU_Save_Transfer.exe >nul
)

exit /b %errorlevel%
