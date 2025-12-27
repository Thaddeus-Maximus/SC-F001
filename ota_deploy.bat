@echo off
REM ESP32 OTA Deployment Script for Windows

setlocal

REM Configuration
set ESP32_IP=192.168.4.1
set PROJECT_NAME=SC-F001
set BUILD_DIR=build
set BINARY_FILE=%BUILD_DIR%\%PROJECT_NAME%.bin

echo ========================================
echo ESP32 OTA Deployment Script
echo ========================================


REM Step 1: Check if binary exists
if not exist "%BINARY_FILE%" (
    echo Error: Binary file not found at %BINARY_FILE%
    echo Please update PROJECT_NAME in this script
    exit /b 1
)

for %%A in ("%BINARY_FILE%") do set BINARY_SIZE=%%~zA
echo Binary size: %BINARY_SIZE% bytes

REM Step 2: Upload via OTA
echo.
echo [2/3] Uploading to ESP32 at %ESP32_IP%...

curl -X POST --data-binary @"%BINARY_FILE%" -w "HTTP Code: %%{http_code}\n" -o nul "http://%ESP32_IP%/ota"

if %ERRORLEVEL% NEQ 0 (
    echo Upload failed! Is the ESP32 reachable at %ESP32_IP%?
    exit /b 1
)

echo Upload successful!
echo ESP32 should be rebooting now...

REM Step 3: Wait for reboot
echo.
echo [3/3] Waiting for ESP32 to reboot...
timeout /t 5 /nobreak > nul

echo Deployment complete!
echo Check your device to verify it's running the new firmware.

exit /b 0