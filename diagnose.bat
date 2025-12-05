@echo off
REM Diagnostic script for EtherCAT network adapter issues
REM This script helps diagnose why ecx_init fails to open network interfaces

echo ========================================
echo EtherCAT Network Adapter Diagnostics
echo ========================================
echo.

REM Check if running as Administrator
net session >nul 2>&1
if %errorLevel% == 0 (
    echo [OK] Running as Administrator
) else (
    echo [WARNING] NOT running as Administrator!
    echo.
    echo Many network operations require Administrator privileges.
    echo Please right-click this script and select "Run as Administrator"
    echo.
    pause
    exit /b 1
)

echo.
echo ========================================
echo Step 1: Checking Npcap Installation
echo ========================================
echo.

REM Check if Npcap service is running
sc query npcap >nul 2>&1
if %errorLevel% == 0 (
    echo [OK] Npcap service found
    sc query npcap | findstr "STATE"
) else (
    echo [ERROR] Npcap service not found!
    echo.
    echo Please install Npcap from: https://npcap.com/#download
    echo Make sure to check "Install Npcap in WinPcap API-compatible Mode"
    echo.
    pause
    exit /b 1
)

echo.
echo ========================================
echo Step 2: Building diagnostic tool
echo ========================================
echo.

if not exist "list-adapters.exe" (
    echo Building list-adapters utility...
    cmake --build . --config Debug --target list-adapters
    if %errorLevel% neq 0 (
        echo [ERROR] Failed to build diagnostic tool
        pause
        exit /b 1
    )
)

REM Find the executable
set DIAG_TOOL=
if exist "Debug\list-adapters.exe" set DIAG_TOOL=Debug\list-adapters.exe
if exist "list-adapters.exe" set DIAG_TOOL=list-adapters.exe

if "%DIAG_TOOL%"=="" (
    echo [ERROR] Could not find list-adapters.exe
    echo Please build the project first: cmake --build . --config Debug
    pause
    exit /b 1
)

echo [OK] Found diagnostic tool: %DIAG_TOOL%

echo.
echo ========================================
echo Step 3: Listing Network Adapters
echo ========================================
echo.

"%DIAG_TOOL%"

echo.
echo ========================================
echo Diagnostics Complete
echo ========================================
echo.
echo Common solutions if ecx_init fails:
echo.
echo 1. Run your application as Administrator
echo 2. Reinstall Npcap with "WinPcap API-compatible Mode" enabled
echo 3. Make sure the network adapter is UP and RUNNING
echo 4. Temporarily disable antivirus/firewall
echo 5. Use the correct NPF device path shown above
echo 6. Check if another application is using the adapter
echo.
echo To test a specific interface, run:
echo   %DIAG_TOOL% -t "\Device\NPF_{YOUR-ADAPTER-GUID}"
echo.
pause
