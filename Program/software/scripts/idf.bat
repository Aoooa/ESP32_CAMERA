@echo off
rem ESP-IDF build wrapper for ESP32_CAMERA (Camera_test_tool).
rem This is the only working build entry on this machine: calling idf.py directly
rem fails because PATH and several env vars are missing in a bare shell.
rem Keep this file ASCII-only: cmd parses .bat as ANSI, non-ASCII comments break it.
set IDF_PATH=D:\esp\v5.5.3\esp-idf
set IDF_TOOLS_PATH=C:\Espressif\tools
rem Needed when sdkconfig changes trigger a reconfigure, otherwise:
rem   OSError: ESP_ROM_ELF_DIR environment variable is not defined
set ESP_ROM_ELF_DIR=C:\Espressif\tools\esp-rom-elfs\20241011
set IDF_TARGET=esp32s3
set SYSTEMROOT=C:\Windows
set USERPROFILE=C:\Users\xeno
set HOME=C:\Users\xeno
set PATH=C:\Espressif\tools\python\v5.5.3\venv\Scripts;C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20251107\xtensa-esp-elf\bin;C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20251107\riscv32-esp-elf\bin;C:\Espressif\tools\cmake\3.30.2\bin;C:\Espressif\tools\ninja\1.12.1;%PATH%
cd /d D:\Task\ESP32_CAMERA\Program\software
python D:\esp\v5.5.3\esp-idf\tools\idf.py %*
