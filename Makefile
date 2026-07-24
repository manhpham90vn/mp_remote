# Cần GNU make. Tự tìm Visual Studio qua vswhere và nạp VsDevCmd,
# nên chạy được từ cmd / PowerShell / Git Bash thường (không cần Developer prompt).
#   make            build debug
#   make release    build release
#   make run ARGS="notepad.exe --loopback"
#   make test       build core_tests rồi chạy (offline, không cần client/GPU)
#   make test-ctest chạy qua CTest (--output-on-failure) — khớp cách CI chạy
#   make coverage   đo phủ core bằng OpenCppCoverage -> out\coverage\index.html
#                   (cài: choco install opencppcoverage -y)
#   make format     áp clang-format (C++) + ktlint (Kotlin) tại chỗ
#   make lint       kiểm tra style, không sửa (dùng trước khi push cho khớp CI)
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

# Test của core: offline, không cần mạng/GPU. Chỉ build target core_tests (không dựng
# client) nên nhanh, và in kết quả theo từng tầng. Exit code 0 = pass.
test:
	@$(DEVCMD) cmake --preset x64-debug >nul && cmake --build --preset x64-debug --target core_tests
	@echo.
	@echo ===== Running core_tests (offline) =====
	out\build\x64-debug\core\core_tests.exe

# Chạy qua CTest — cùng cách CI chạy. --output-on-failure in stdout của test khi rớt.
test-ctest:
	@$(DEVCMD) cmake --preset x64-debug >nul && cmake --build --preset x64-debug --target core_tests
	@$(DEVCMD) ctest --test-dir out/build/x64-debug --output-on-failure

# Đo phủ code của lõi. Cần OpenCppCoverage trên PATH (choco install opencppcoverage -y).
# Dùng bản Debug (có PDB); chỉ tính core/src + core/include, bỏ code test khỏi mẫu số.
coverage:
	@$(DEVCMD) cmake --preset x64-debug >nul && cmake --build --preset x64-debug --target core_tests
	OpenCppCoverage --sources core\src --sources core\include --excluded_sources core\tests --export_type html:out\coverage -- out\build\x64-debug\core\core_tests.exe
	@echo Report: out\coverage\index.html

# Format/lint dùng chung một script PowerShell (tự dò clang-format trong VS + ktlint).
format:
	@powershell -NoProfile -ExecutionPolicy Bypass -File scripts\codestyle.ps1

lint:
	@powershell -NoProfile -ExecutionPolicy Bypass -File scripts\codestyle.ps1 -Check

clean:
	@$(DEVCMD) cmake -E rm -rf out

.PHONY: all debug release run test test-ctest coverage format lint clean
