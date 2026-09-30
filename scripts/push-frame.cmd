@echo off
setlocal

if /I "%~1"=="--help" goto :help
if /I "%~1"=="-h" goto :help

set "FRAME_TARGET=%FRAME_HOST%"
if not defined FRAME_TARGET set "FRAME_TARGET=steamos@frame"
set "REMOTE_DIR=%FRAME_REMOTE_DIR%"
if not defined REMOTE_DIR set "REMOTE_DIR=frame-notify"

set "RUN_AFTER_BUILD=0"
set "RUN_OPTION="
if /I "%~1"=="--run" set "RUN_AFTER_BUILD=1"
if /I "%~1"=="-Run" set "RUN_AFTER_BUILD=1"
if /I "%~1"=="--native-notification" set "RUN_AFTER_BUILD=1"
if /I "%~1"=="--native-notification" set "RUN_OPTION= --native-notification"
if /I "%~1"=="-NativeNotification" set "RUN_AFTER_BUILD=1"
if /I "%~1"=="-NativeNotification" set "RUN_OPTION= --native-notification"
if not "%~1"=="" if "%RUN_AFTER_BUILD%"=="0" goto :help_error

for %%I in ("%~dp0..") do set "PROJECT_DIR=%%~fI"
set "ARCHIVE=%TEMP%\frame-notify-%RANDOM%-%RANDOM%.tar.gz"
set "RESULT=1"
set "SSH_OPTS="
set "SSH_KEY=%USERPROFILE%\.ssh\id_ed25519_frame_notify"
if exist "%SSH_KEY%" set SSH_OPTS=-i "%SSH_KEY%" -o IdentitiesOnly=yes

where tar.exe >nul 2>nul
if errorlevel 1 (
    echo error: Windows tar.exe was not found.
    goto :cleanup
)
where scp.exe >nul 2>nul
if errorlevel 1 (
    echo error: Windows OpenSSH scp.exe was not found.
    goto :cleanup
)
where ssh.exe >nul 2>nul
if errorlevel 1 (
    echo error: Windows OpenSSH ssh.exe was not found.
    goto :cleanup
)

echo [Push] Packaging source files...
pushd "%PROJECT_DIR%"
if errorlevel 1 goto :cleanup
tar.exe -czf "%ARCHIVE%" CMakeLists.txt README.md LICENSE .gitignore src scripts tests docs
if errorlevel 1 (
    popd
    echo error: tar failed.
    goto :cleanup
)
popd

echo [Push] Uploading to %FRAME_TARGET%...
scp.exe %SSH_OPTS% "%ARCHIVE%" "%FRAME_TARGET%:frame-notify-upload.tar.gz"
if errorlevel 1 (
    echo error: scp failed.
    goto :cleanup
)

echo [Push] Extracting, building, and running tests on Steam Frame...
ssh.exe %SSH_OPTS% "%FRAME_TARGET%" "mkdir -p ~/%REMOTE_DIR% && tar -xzf ~/frame-notify-upload.tar.gz -C ~/%REMOTE_DIR% && cd ~/%REMOTE_DIR% && bash ./scripts/build-frame.sh"
if errorlevel 1 (
    echo error: remote build failed.
    goto :cleanup
)

echo [Push] Steam Frame build succeeded.
set "RESULT=0"
if "%RUN_AFTER_BUILD%"=="1" (
    echo [Run] Starting Frame Notify. Press Ctrl+C to stop.
    ssh.exe %SSH_OPTS% -t "%FRAME_TARGET%" "cd ~/%REMOTE_DIR% && ./build-frame/frame-notify-test%RUN_OPTION%"
) else (
    echo Run the new build with: scripts\push-frame.cmd -Run
)

:cleanup
if exist "%ARCHIVE%" del /q "%ARCHIVE%"
exit /b %RESULT%

:help_error
echo error: unsupported argument: %~1
:help
echo Usage: scripts\push-frame.cmd [-Run ^| -NativeNotification]
echo.
echo Environment overrides:
echo   FRAME_HOST=steamos@frame
echo   FRAME_REMOTE_DIR=frame-notify
exit /b 2
