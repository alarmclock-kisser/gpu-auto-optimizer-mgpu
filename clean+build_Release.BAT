@echo off
setlocal
chcp 65001 >nul

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

set "DEPLOY_DIR=%ROOT%\deploy"
set "RELEASE_DIR=%ROOT%\build\Release"
set "GUI_EXE=%DEPLOY_DIR%\GpuAutoOptimizer.exe"
set "LNK_PATH=%ROOT%\GPU Auto Optimizer (Release).lnk"

rem Request the tray Exit path and wait; never force-kill GPU control processes.
if not exist "%ROOT%\request-clean-exit.ps1" goto :shutdown_helper_missing
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%\request-clean-exit.ps1" -TimeoutSeconds 180
if errorlevel 1 goto :app_running

rem Find CMake on PATH, or use the Visual Studio 2026 installation.
set "CMAKE_EXE="
for /f "delims=" %%I in ('where cmake.exe 2^>nul') do if not defined CMAKE_EXE set "CMAKE_EXE=%%I"

if not defined CMAKE_EXE (
    set "CMAKE_EXE=%ProgramFiles%\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
)

if not exist "%CMAKE_EXE%" goto :cmake_missing

for %%I in ("%CMAKE_EXE%") do set "CMAKE_DIR=%%~dpI"
set "CTEST_EXE=%CMAKE_DIR%ctest.exe"
if not exist "%CTEST_EXE%" goto :ctest_missing

cd /d "%ROOT%" || goto :root_failed

rem Remove the deployment directory completely.
echo.
echo Removing deploy directory...
if exist "%DEPLOY_DIR%" rmdir /s /q "%DEPLOY_DIR%"
if exist "%DEPLOY_DIR%" goto :deploy_locked

rem Configure and rebuild Release from clean outputs.
echo.
echo Configuring the x64 build...
"%CMAKE_EXE%" --preset default
if errorlevel 1 goto :build_failed

echo.
echo Building Release...
"%CMAKE_EXE%" --build --preset release --clean-first
if errorlevel 1 goto :build_failed

rem Run the Release tests.
echo.
echo Running Release tests...
"%CTEST_EXE%" --preset release
if errorlevel 1 goto :test_failed

rem Stage both native executables and the release documentation.
echo.
echo Creating deployment directory...
mkdir "%DEPLOY_DIR%"
if errorlevel 1 goto :deploy_failed

copy /y "%RELEASE_DIR%\gao.exe" "%DEPLOY_DIR%\gao.exe" >nul
if errorlevel 1 goto :deploy_failed

copy /y "%RELEASE_DIR%\GpuAutoOptimizer.exe" "%DEPLOY_DIR%\GpuAutoOptimizer.exe" >nul
if errorlevel 1 goto :deploy_failed

copy /y "%ROOT%\README.md" "%DEPLOY_DIR%\README.md" >nul
if errorlevel 1 goto :deploy_failed

copy /y "%ROOT%\LICENSE" "%DEPLOY_DIR%\LICENSE" >nul
if errorlevel 1 goto :deploy_failed

if not exist "%GUI_EXE%" goto :deploy_failed

rem Create a shortcut to the GUI in the repository root.
echo.
echo Creating shortcut...
if exist "%LNK_PATH%" del /f /q "%LNK_PATH%"

powershell -NoProfile -Command ^
    "$ws = New-Object -ComObject WScript.Shell; " ^
    "$s = $ws.CreateShortcut('%LNK_PATH%'); " ^
    "$s.TargetPath = '%GUI_EXE%'; " ^
    "$s.WorkingDirectory = '%DEPLOY_DIR%'; " ^
    "$s.Description = 'GPU Auto Optimizer (Release)'; " ^
    "$s.Save()"

if errorlevel 1 goto :shortcut_failed

rem Launch the main GUI executable.
echo.
echo Starting GpuAutoOptimizer.exe...
start "" /D "%DEPLOY_DIR%" "%GUI_EXE%"

echo.
echo Done. Release files are in "%DEPLOY_DIR%".
exit /b 0

:app_running
echo.
echo ERROR: An optimizer process did not exit within the timeout.
echo No force-kill was attempted. Close the GUI with Tray > Exit or let gao.exe finish.
echo For gao --optimize, Ctrl+C requests a safe abort; then retry this batch file.
pause
exit /b 1

:shutdown_helper_missing
echo.
echo ERROR: Could not find request-clean-exit.ps1 next to this batch file.
pause
exit /b 1

:cmake_missing
echo.
echo ERROR: Could not find cmake.exe.
pause
exit /b 1

:ctest_missing
echo.
echo ERROR: Could not find ctest.exe next to CMake.
pause
exit /b 1

:root_failed
echo.
echo ERROR: Could not switch to the repository directory.
pause
exit /b 1

:deploy_locked
echo.
echo ERROR: Could not remove "%DEPLOY_DIR%".
echo Close any app running from that directory, then retry.
pause
exit /b 1

:build_failed
echo.
echo ERROR: CMake configure or Release build failed.
pause
exit /b 1

:test_failed
echo.
echo ERROR: Release tests failed.
pause
exit /b 1

:deploy_failed
echo.
echo ERROR: Could not stage all deployment files.
pause
exit /b 1

:shortcut_failed
echo.
echo ERROR: Could not create the shortcut.
pause
exit /b 1