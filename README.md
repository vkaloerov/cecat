# EtherCAT CLI Tool

Educational EtherCAT network testing tool built with [SOEM](https://github.com/OpenEtherCATsociety/SOEM) library.

## Description

Cross-platform command-line utilities for testing and debugging EtherCAT networks:
- **dummy-ecat-cli** - Interactive CLI for EtherCAT slave configuration and PDO exchange
- **list-adapters** - Network interface discovery and diagnostic tool

**Supports:** Leadshine EM3E-556 stepper motor control via CiA 402 profile

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

## Motor Control (Leadshine EM3E-556)

### Quick Start
```bash
# 1. Connect and scan
scan
pdo-start

# 2. Enable motor drive
motor-enable 1

# 3. Run motor for 10 seconds at 100 RPM
motor-run 1 100 10

# 4. Change velocity (forward/reverse)
motor-velocity 1 200      # 200 RPM forward
motor-velocity 1 -150     # 150 RPM reverse

# 5. Stop motor
motor-stop 1
motor-disable 1
```

### Motor Commands
```
motor-enable <idx>           - Enable motor drive
motor-disable <idx>          - Disable motor drive
motor-run <idx> <rpm> <sec>  - Run for specified time
motor-velocity <idx> <rpm>   - Set velocity (+ forward, - reverse)
motor-stop <idx>             - Emergency stop
motor-status <idx>           - Show motor status
```

📖 **See [EM3E_QUICKSTART.md](EM3E_QUICKSTART.md) for detailed motor control guide**

## Project Structure
```
cecat/
├── ecat_cli.c           - Main CLI application with EM3E-556 control
├── list_adapters.c      - Network diagnostic tool
├── CMakeLists.txt       - Build configuration
├── EM3E_QUICKSTART.md   - Motor control guide
└── .zed/
    └── debug.json       - Debug configuration for Zed editor
```

## Supported Devices

- ✅ Leadshine EM3E-556 EtherCAT Stepper Drive
- ✅ Generic EtherCAT slaves (via PDO read/write)

## License

Educational use only. SOEM library is licensed under GPLv2.

## Notes

⚠️ **Administrator/root privileges required** for raw socket access  
⚠️ Ensure SOEM library is built and located at `../SOEM-2.0.0`
