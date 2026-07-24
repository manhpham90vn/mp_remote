# Cần GNU make. Chạy được trên Windows, macOS và Ubuntu — nhánh theo $(OS):
# Windows dùng cmd + VsDevCmd (tự tìm Visual Studio qua vswhere nên gọi được từ
# cmd / PowerShell / Git Bash thường), macOS/Linux dùng sh + toolchain hệ thống.
#   make bootstrap      cài toàn bộ dependency dev cho OS hiện tại (cả Android SDK, coverage)
#   make                build debug cây desktop OS hiện tại (Windows: client + core; Unix: core)
#   make release        build release cây desktop OS hiện tại
#
# Build/release RÕ theo từng nền tảng (sau này thêm nền tảng nào thì thêm cặp mới):
#   make build-windows   / release-windows   client desktop Windows (chỉ chạy trên Windows)
#   make build-android   / release-android   APK debug / APK release (chưa ký — xem ghi chú)
#   make build-ios       / release-ios       app iOS cho Simulator (cần macOS + Xcode)
#
#   make run            chạy client desktop (mới có Windows), ARGS="notepad.exe --loopback"
#   make run-android    build + cài + mở app Android trên máy/emulator đang kết nối (adb)
#   make run-ios        build + cài + mở app iOS trên Simulator (cần macOS + Xcode)
#   make test         build core_tests rồi chạy (offline, không cần client/GPU)
#   make test-ctest   chạy qua CTest (--output-on-failure) — khớp cách CI chạy
#   make coverage     đo phủ core (clang + llvm-cov — chạy trên cả Windows/macOS/Ubuntu)
#
# Format/lint — cả 3 ngôn ngữ hoặc rõ từng ngôn ngữ:
#   make format         áp format tại chỗ cho cả C++ + Kotlin + Swift
#   make lint           kiểm tra style cả 3, không sửa (dùng trước khi push cho khớp CI)
#   make format-cpp     / lint-cpp      chỉ C++ (clang-format: core/ platform/ client/)
#   make format-kotlin  / lint-kotlin   chỉ Kotlin (ktlint: client/android)
#   make format-swift   / lint-swift    chỉ Swift (swiftformat: client/ios)
#
#   make clean

ifeq ($(OS),Windows_NT)
# --- Windows: mọi lệnh cmake/ctest đi qua VsDevCmd để có MSVC + CMake + Ninja của VS.
SHELL := cmd.exe
.SHELLFLAGS := /c

VSWHERE := C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe
VSDIR   := $(shell "$(VSWHERE)" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
VSDEV   := $(VSDIR)\Common7\Tools\VsDevCmd.bat
DEVCMD  := call "$(VSDEV)" -arch=x64 -host_arch=x64 -no_logo &&

BOOTSTRAP  := powershell -NoProfile -ExecutionPolicy Bypass -File scripts\bootstrap.ps1
CODESTYLE  := powershell -NoProfile -ExecutionPolicy Bypass -File scripts\codestyle.ps1
CHECKFLAG  := -Check
ONLYFLAG   := -Only
CORE_TESTS := out\build\x64-debug\core\core_tests.exe
NULDEV     := nul
# LLVM đi kèm VS (component VC.Llvm.Clang — bootstrap cài): clang++ + llvm-cov/profdata.
# VsDevCmd không tự thêm thư mục này vào PATH nên coverage tự prepend.
LLVMPATH   := set "PATH=$(VSDIR)\VC\Tools\Llvm\x64\bin;%PATH%" &&
LLVM       :=
COV_TESTS  := out\build\coverage\core\core_tests.exe
COV_RAW    := out\build\coverage\core_tests.profraw
COV_DATA   := out\build\coverage\core_tests.profdata
GRADLEW    := cd client\android && .\gradlew.bat
ADB        := $(if $(ANDROID_HOME),$(ANDROID_HOME)\platform-tools\adb.exe,$(LOCALAPPDATA)\Android\Sdk\platform-tools\adb.exe)
else
# --- macOS/Ubuntu: cmake/ninja/clang(gcc) lấy thẳng từ PATH (scripts/bootstrap.sh cài).
UNAME      := $(shell uname -s)
DEVCMD     :=
BOOTSTRAP  := scripts/bootstrap.sh
CODESTYLE  := scripts/codestyle.sh
CHECKFLAG  := --check
ONLYFLAG   := --only
CORE_TESTS := out/build/x64-debug/core/core_tests
NULDEV     := /dev/null
# macOS: llvm-cov/llvm-profdata nằm trong toolchain Xcode, gọi qua xcrun.
# Ubuntu: gói llvm cài thẳng vào PATH (bootstrap.sh cài clang + llvm).
LLVMPATH   :=
LLVM       := $(if $(filter Darwin,$(UNAME)),xcrun)
COV_TESTS  := out/build/coverage/core/core_tests
COV_RAW    := out/build/coverage/core_tests.profraw
COV_DATA   := out/build/coverage/core_tests.profdata
GRADLEW    := cd client/android && ./gradlew
ADB        := $(if $(ANDROID_HOME),$(ANDROID_HOME)/platform-tools/adb,adb)
endif

all: debug

# Cài toàn bộ dependency dev (idempotent — có rồi thì bỏ qua).
bootstrap:
	@$(BOOTSTRAP)

debug:
	@$(DEVCMD) cmake --preset x64-debug && cmake --build --preset x64-debug

release:
	@$(DEVCMD) cmake --preset x64-release && cmake --build --preset x64-release

# --- Build/release từng nền tảng --------------------------------------------
# Windows: chính là cây CMake desktop ở trên — đặt tên rõ để đối xứng với các
# nền tảng khác; trên máy không phải Windows thì báo lỗi thay vì build nhầm core.
ifeq ($(OS),Windows_NT)
build-windows: debug
release-windows: release
else
build-windows release-windows:
	@echo "make $@: run on a Windows machine (desktop client is Windows-only for now)"; exit 1
endif

# Android: Gradle tự dựng cả libdeskhub.so (NDK + core) lẫn APK — không đi qua
# cây CMake desktop. Chỉ cần SDK, không cần máy/emulator, chạy được trên cả 3 OS.
# release-android ra app-release-unsigned.apk: dự án CHƯA khai signingConfig,
# khi phát hành thật thì ký bằng apksigner hoặc thêm signingConfig vào gradle.
build-android:
	$(GRADLEW) assembleDebug

release-android:
	$(GRADLEW) assembleRelease

# iOS: build cho Simulator bằng xcodebuild (target `app`, không cần scheme/signing).
# Sản phẩm ra out/build/ios/<Config>-iphonesimulator/app.app. Chỉ chạy trên macOS.
# Bản chạy máy thật/App Store cần signing team + archive qua Xcode — để sau.
ifeq ($(UNAME),Darwin)
build-ios:
	xcodebuild -project client/ios/Deskhub.xcodeproj -target app -configuration Debug -sdk iphonesimulator SYMROOT=$(CURDIR)/out/build/ios build

release-ios:
	xcodebuild -project client/ios/Deskhub.xcodeproj -target app -configuration Release -sdk iphonesimulator SYMROOT=$(CURDIR)/out/build/ios build
else
build-ios release-ios:
	@echo "make $@: needs macOS + Xcode"; exit 1
endif

# Desktop (Windows): client.exe chứa cả vai host lẫn client.
ifeq ($(OS),Windows_NT)
run: debug
	out\build\x64-debug\client\windows\client.exe $(ARGS)
else
run:
	@echo "make run: desktop client is Windows-only for now (see docs/05-roadmap.md)"; exit 1
endif

# Android: gradle tự build libdeskhub.so (NDK) + APK, cài qua adb rồi mở app.
# Cần máy thật/emulator đang hiện trong `adb devices`.
run-android:
	$(GRADLEW) installDebug
	"$(ADB)" shell am start -n com.android.deskhub/com.deskhub.app.MainActivity

# iOS: mở Simulator, đợi boot xong rồi cài + launch bản vừa build.
ifeq ($(UNAME),Darwin)
run-ios: build-ios
	open -a Simulator
	xcrun simctl bootstatus booted -b
	xcrun simctl install booted out/build/ios/Debug-iphonesimulator/app.app
	xcrun simctl launch booted com.ios.deskhub
else
run-ios:
	@echo make run-ios: needs macOS + Xcode
	@exit 1
endif

# Test của core: offline, không cần mạng/GPU. Chỉ build target core_tests (không dựng
# client) nên nhanh. Exit code 0 = pass.
test:
	@$(DEVCMD) cmake --preset x64-debug >$(NULDEV) && cmake --build --preset x64-debug --target core_tests
	@echo ===== Running core_tests offline =====
	$(CORE_TESTS)

# Chạy qua CTest — cùng cách CI chạy. --output-on-failure in stdout của test khi rớt.
test-ctest:
	@$(DEVCMD) cmake --preset x64-debug >$(NULDEV) && cmake --build --preset x64-debug --target core_tests
	@$(DEVCMD) ctest --test-dir out/build/x64-debug --output-on-failure

# Đo phủ code của lõi — cùng một cách trên cả 3 OS: build cây riêng preset
# `coverage` bằng clang (instrument -fprofile-instr-generate/-fcoverage-mapping),
# chạy core_tests sinh .profraw rồi xuất báo cáo qua llvm-profdata + llvm-cov.
# Nguồn tool: Windows = LLVM kèm VS (VC.Llvm.Clang), macOS = Xcode (xcrun),
# Ubuntu = gói clang + llvm. Chỉ tính core/src + core/include (positional filter
# của llvm-cov) nên code test tự động nằm ngoài mẫu số.
ifeq ($(OS),Windows_NT)
coverage:
	@$(DEVCMD) $(LLVMPATH) cmake --preset coverage >$(NULDEV) && cmake --build --preset coverage --target core_tests
	@$(DEVCMD) set "LLVM_PROFILE_FILE=$(COV_RAW)" && $(COV_TESTS)
	@$(DEVCMD) $(LLVMPATH) llvm-profdata merge -sparse $(COV_RAW) -o $(COV_DATA)
	@$(DEVCMD) $(LLVMPATH) llvm-cov show $(COV_TESTS) -instr-profile=$(COV_DATA) -format=html -output-dir=out\coverage core\src core\include
	@$(DEVCMD) $(LLVMPATH) llvm-cov report $(COV_TESTS) -instr-profile=$(COV_DATA) core\src core\include
	@echo Report: out\coverage\index.html
else
coverage:
	@cmake --preset coverage >$(NULDEV) && cmake --build --preset coverage --target core_tests
	LLVM_PROFILE_FILE=$(COV_RAW) $(COV_TESTS)
	@$(LLVM) llvm-profdata merge -sparse $(COV_RAW) -o $(COV_DATA)
	@$(LLVM) llvm-cov show $(COV_TESTS) -instr-profile=$(COV_DATA) -format=html -output-dir=out/coverage core/src core/include
	@$(LLVM) llvm-cov report $(COV_TESTS) -instr-profile=$(COV_DATA) core/src core/include
	@echo "Report: out/coverage/index.html"
endif

# Format/lint mỗi OS một script cùng hành vi: Windows codestyle.ps1, Unix codestyle.sh.
# Tool (clang-format + ktlint + swiftformat bản ghim) do `make bootstrap` cài sẵn.
# format/lint chạy cả 3 ngôn ngữ; các biến thể -cpp/-kotlin/-swift giới hạn một ngôn ngữ.
format:
	@$(CODESTYLE)

format-cpp:
	@$(CODESTYLE) $(ONLYFLAG) cpp

format-kotlin:
	@$(CODESTYLE) $(ONLYFLAG) kotlin

format-swift:
	@$(CODESTYLE) $(ONLYFLAG) swift

lint:
	@$(CODESTYLE) $(CHECKFLAG)

lint-cpp:
	@$(CODESTYLE) $(CHECKFLAG) $(ONLYFLAG) cpp

lint-kotlin:
	@$(CODESTYLE) $(CHECKFLAG) $(ONLYFLAG) kotlin

lint-swift:
	@$(CODESTYLE) $(CHECKFLAG) $(ONLYFLAG) swift

clean:
ifeq ($(OS),Windows_NT)
	@$(DEVCMD) cmake -E rm -rf out
else
	@rm -rf out
endif

.PHONY: all bootstrap debug release build-windows release-windows build-android release-android \
        build-ios release-ios run run-android run-ios test test-ctest coverage \
        format format-cpp format-kotlin format-swift lint lint-cpp lint-kotlin lint-swift clean
