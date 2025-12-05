@echo off
REM Windows batch file for running EtherCAT CLI
REM Run as Administrator!

echo ======================================
echo EtherCAT CLI - Windows Startup Script
echo ======================================
echo.

REM Check if running as administrator
net session >nul 2>&1
if %errorLevel% == 0 (
    echo [OK] Running with administrator privileges
) else (
    echo [ERROR] This script must be run as Administrator!
    echo Right-click and select "Run as administrator"
    pause
    exit /b 1
)

echo.
echo Available network adapters:
echo.

REM List available network adapters using getmac
getmac /v /fo table

echo.
echo ======================================
echo.
echo To find WinPcap device names, you can also check:
echo Control Panel ^> Network and Sharing Center ^> Change adapter settings
echo.
echo Device name format: \Device\NPF_{GUID}
echo.
echo ======================================
echo.

REM Example: set your adapter GUID here
REM set ADAPTER=\Device\NPF_{12345678-1234-1234-1234-123456789ABC}

if "%1"=="" (
    echo Usage: %0 ^<adapter_name^> [options]
    echo.
    echo Example:
    echo   %0 "\Device\NPF_{12345678-1234-1234-1234-123456789ABC}"
    echo   %0 "\Device\NPF_{12345678-1234-1234-1234-123456789ABC}" -v
    echo.
    pause
    exit /b 1
)

echo Starting EtherCAT CLI on adapter: %1
echo.

REM Run the application
build\Release\dummy-ecat-cli.exe -i %1 %2 %3 %4

if %errorLevel% neq 0 (
    echo.
    echo [ERROR] Application exited with error code %errorLevel%
    echo.
    echo Common issues:
    echo - Npcap not installed (download from https://npcap.com/)
    echo - Wrong adapter name
    echo - No EtherCAT devices connected
    echo.
)

pause
