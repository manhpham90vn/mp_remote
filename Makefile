# Can GNU make. Tu tim Visual Studio qua vswhere va nap VsDevCmd,
# nen chay duoc tu cmd / PowerShell / Git Bash thuong (khong can Developer prompt).
#   make            build debug
#   make release    build release
#   make run ARGS="notepad.exe --loopback"
#   make clean

SHELL := cmd.exe
.SHELLFLAGS := /c

VSWHERE := C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe
VSDIR   := $(shell "$(VSWHERE)" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
VSDEV   := $(VSDIR)\Common7\Tools\VsDevCmd.bat
DEVCMD  := call "$(VSDEV)" -arch=x64 -host_arch=x64 -no_logo &&

all: debug

debug:
	@$(DEVCMD) cmake --preset x64-debug && cmake --build --preset x64-debug

release:
	@$(DEVCMD) cmake --preset x64-release && cmake --build --preset x64-release

run: debug
	out\build\x64-debug\client\windows\client.exe $(ARGS)

clean:
	@$(DEVCMD) cmake -E rm -rf out

.PHONY: all debug release run clean
