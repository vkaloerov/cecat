# EtherCAT CLI Tool

Educational EtherCAT network testing tool built with [SOEM](https://github.com/OpenEtherCATsociety/SOEM) library.

## Description

Cross-platform command-line utilities for testing and debugging EtherCAT networks:
- **dummy-ecat-cli** - Interactive CLI for EtherCAT slave configuration and PDO exchange
- **list-adapters** - Network interface discovery and diagnostic tool

## Requirements

### Windows
- CMake 3.10+
- MSVC / MinGW-w64 / Clang
- [Npcap](https://npcap.com/#download) (with WinPcap compatibility mode)
- [SOEM 2.0.0](https://github.com/OpenEtherCATsociety/SOEM) library
- [Npcap SDK](https://npcap.com/dist/npcap-sdk-1.13.zip)

### Linux (Ubuntu/Debian)
```bash
sudo apt-get install build-essential cmake libpcap-dev
```
- SOEM 2.0.0 library

## Build

### Windows (MSVC)
```cmd
cmake -B build -G "Visual Studio 17 2022" -DNPCAP_SDK_DIR="C:/npcap-sdk"
cmake --build build --config Debug
```

### Windows (MinGW)
```bash
cmake -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

### Linux
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

## Usage

### List Network Adapters
```bash
# Windows (as Administrator)
list-adapters.exe

# Linux (as root)
sudo ./list-adapters
```

### EtherCAT CLI
```bash
# Windows
dummy-ecat-cli.exe -i "\Device\NPF_{GUID}"

# Linux
sudo ./dummy-ecat-cli -i eth0
```

### Available Commands
```
help          - Show available commands
scan          - Scan EtherCAT bus
read-config   - Read slave configuration
status        - Show network status
pdo-start     - Start PDO exchange
pdo-read      - Read PDO inputs
pdo-write     - Write PDO outputs
verbose       - Toggle verbose mode
exit          - Exit program
```

## Project Structure
```
cecat/
├── ecat_cli.c       - Main CLI application
├── list_adapters.c  - Network diagnostic tool
├── CMakeLists.txt   - Build configuration
└── .zed/
    └── debug.json   - Debug configuration for Zed editor
```

## License

Educational use only. SOEM library is licensed under GPLv3.

## Notes

⚠️ **Administrator/root privileges required** for raw socket access  
⚠️ Ensure SOEM library is built and located at `../SOEM-2.0.0`
