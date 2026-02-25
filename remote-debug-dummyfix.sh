#!/bin/bash

# Script to set capabilities for remote debugging tools
# Run this script with sudo: sudo ./remote-debug-dummyfix.sh

setcap cap_net_raw,cap_net_admin+eip /home/rpy/workspace/cecat/dummy-ecat-cli
setcap cap_net_raw,cap_net_admin+eip /home/rpy/.zed_server/zed-remote-server-stable-0.217.3+stable.105.80433cb239e868271457ac376673a5f75bc4adb1
setcap cap_net_raw,cap_net_admin+eip /home/rpy/.local/share/zed/debug_adapters/CodeLLDB/CodeLLDB_v1.12.0/extension/adapter/codelldb
setcap cap_net_raw,cap_net_admin+eip /home/rpy/.local/share/zed/debug_adapters/CodeLLDB/CodeLLDB_v1.12.0/extension/bin/codelldb-launch
setcap cap_net_raw,cap_net_admin+eip /home/rpy/.local/share/zed/debug_adapters/CodeLLDB/CodeLLDB_v1.12.0/extension/lldb/bin/lldb-server

echo "Capabilities set successfully for all specified binaries."
