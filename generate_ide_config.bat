@echo off
REM Script to generate IDE configuration files for Zed/clangd

echo Generating IDE configuration files...

REM Set paths
set SOEM_DIR=C:\workspace\drag_carrot\SOEM-2.0.0
set NPCAP_DIR=C:\npcap-sdk
set PROJECT_DIR=%~dp0

REM Create .clangd file
echo Creating .clangd...
(
echo CompileFlags:
echo   Add:
echo     - -I%SOEM_DIR%\include
echo     - -I%SOEM_DIR%\osal
echo     - -I%SOEM_DIR%\osal\win32
echo     - -I%SOEM_DIR%\oshw\win32
echo     - -I%NPCAP_DIR%\Include
echo     - -std=c99
echo   Remove:
echo     - -W*
) > .clangd

REM Create compile_commands.json
echo Creating compile_commands.json...
(
echo [
echo   {
echo     "directory": "%PROJECT_DIR:~0,-1%",
echo     "command": "cl.exe /nologo /W4 /std:c99 -I%SOEM_DIR%/include -I%SOEM_DIR%/osal -I%SOEM_DIR%/osal/win32 -I%SOEM_DIR%/oshw/win32 -I%NPCAP_DIR%/Include /c ecat_cli.c",
echo     "file": "%PROJECT_DIR%ecat_cli.c"
echo   }
echo ]
) > compile_commands.json

echo Done! IDE configuration files created.
echo Please restart Zed to apply changes.
pause
