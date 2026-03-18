@echo off
REM Script to generate IDE configuration files for Zed/clangd

echo Generating IDE configuration files...

REM Set paths (always use forward slashes for JSON compatibility)
set SOEM_DIR=C:/workspace/drag_carrot/SOEM-2.0.0
set NPCAP_DIR=C:/npcap-sdk
set PROJECT_DIR=C:/workspace/drag_carrot/cecat

REM Create .clangd file (backslashes are fine in YAML)
echo Creating .clangd...
(
echo CompileFlags:
echo   Add:
echo     - -IC:/workspace/drag_carrot/SOEM-2.0.0/include
echo     - -IC:/workspace/drag_carrot/SOEM-2.0.0/osal
echo     - -IC:/workspace/drag_carrot/SOEM-2.0.0/osal/win32
echo     - -IC:/workspace/drag_carrot/SOEM-2.0.0/oshw/win32
echo     - -IC:/npcap-sdk/Include
echo     - -std=c99
echo   Remove:
echo     - -W*
) > .clangd

REM Create compile_commands.json
REM IMPORTANT: Use forward slashes everywhere in JSON to avoid \n, \t etc. being
REM            interpreted as escape sequences by JSON parsers.
echo Creating compile_commands.json...
(
echo [
echo   {
echo     "directory": "%PROJECT_DIR%",
echo     "command": "clang -std=c99 -I%SOEM_DIR%/include -I%SOEM_DIR%/osal -I%SOEM_DIR%/osal/win32 -I%SOEM_DIR%/oshw/win32 -I%NPCAP_DIR%/Include -c ecat_cli.c",
echo     "file": "%PROJECT_DIR%/ecat_cli.c"
echo   },
echo   {
echo     "directory": "%PROJECT_DIR%",
echo     "command": "clang -std=c99 -I%SOEM_DIR%/include -I%SOEM_DIR%/osal -I%SOEM_DIR%/osal/win32 -I%SOEM_DIR%/oshw/win32 -I%NPCAP_DIR%/Include -c my_hex_dump.c",
echo     "file": "%PROJECT_DIR%/my_hex_dump.c"
echo   },
echo   {
echo     "directory": "%PROJECT_DIR%",
echo     "command": "clang -std=c99 -I%SOEM_DIR%/include -I%SOEM_DIR%/osal -I%SOEM_DIR%/osal/win32 -I%SOEM_DIR%/oshw/win32 -I%NPCAP_DIR%/Include -c list_adapters.c",
echo     "file": "%PROJECT_DIR%/list_adapters.c"
echo   }
echo ]
) > compile_commands.json

echo.
echo Done! IDE configuration files created.
echo.
echo  .clangd            - compiler flags for clangd fallback
echo  compile_commands.json - per-file compile commands (uses clang, forward slashes)
echo.
echo Please restart Zed / reload clangd to apply changes.
pause
