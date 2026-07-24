<#
bootstrap.ps1 — cài TOÀN BỘ dependency phát triển trên WINDOWS (máy dev chính).
macOS/Ubuntu dùng scripts/bootstrap.sh. Gọi qua `make bootstrap`.

Cài (idempotent — có rồi thì bỏ qua):
  - Visual Studio 2026 Build Tools: MSVC x64 + CMake/Ninja + LLVM
    (clang-format cho codestyle, clang + llvm-cov cho make coverage)
  - GNU make, JDK 17 (Temurin)
  - ktlint + swiftformat bản ghim version, tải về tools\ (đã gitignore) cho codestyle.ps1
  - Android SDK/NDK khớp client/android/app/build.gradle.kts (cần sdkmanager sẵn,
    không có thì cài Android Studio và nhắc mở một lần)
#>
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')

# Version ghim cho tool tải tay — đổi ở đây thì đổi cả CI (.github/workflows/lint.yml).
$ktlintVersion = '1.5.0'
$swiftformatVersion = '0.62.1'

# winget là trình cài duy nhất script dựa vào — Windows 11 có sẵn.
if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    throw "winget not found. Install 'App Installer' from Microsoft Store, then re-run."
}

# Cài một package nếu lệnh tương ứng chưa có. Trả về $true nếu vừa cài mới.
function Install-IfMissing([string]$Cmd, [string]$WingetId, [string]$Label) {
    if (Get-Command $Cmd -ErrorAction SilentlyContinue) {
        Write-Host "[ok]      $Label ($((Get-Command $Cmd).Source))"
        return $false
    }
    Write-Host "[install] $Label ($WingetId)..."
    & winget install --id $WingetId --exact --accept-source-agreements --accept-package-agreements
    if ($LASTEXITCODE -ne 0) { throw "winget failed installing $WingetId (exit $LASTEXITCODE)" }
    return $true
}

$restartNote = $false

# --- Visual Studio 2026 Build Tools (MSVC x64 + LLVM) ----------------------
# Makefile cần component VC.Tools.x86.x64 (VsDevCmd); component VC.Llvm.Clang cấp
# clang-format (codestyle.ps1) lẫn clang++/llvm-cov/llvm-profdata (make coverage).
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsOk = $false
if (Test-Path $vswhere) {
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($vs) {
        $vsOk = $true; Write-Host "[ok]      Visual Studio C++ toolchain ($vs)"
        # VS cài tay có thể thiếu component LLVM — make lint (clang-format) lẫn
        # make coverage (clang++/llvm-cov) đều cần nó, kiểm tra và nhắc rõ.
        if (Test-Path (Join-Path $vs 'VC\Tools\Llvm\x64\bin\clang++.exe')) {
            Write-Host "[ok]      LLVM in VS (clang-format + clang++/llvm-cov)"
        } else {
            Write-Host "[action]  VS is missing the LLVM component (needed by 'make lint' + 'make coverage')."
            Write-Host "          Open Visual Studio Installer -> Modify -> add 'C++ Clang tools for Windows'."
        }
    }
}
if (-not $vsOk) {
    Write-Host "[install] Visual Studio 2026 Build Tools (C++ workload + Clang)..."
    winget install --id Microsoft.VisualStudio.BuildTools --exact --accept-source-agreements --accept-package-agreements `
        --override "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Llvm.Clang --includeRecommended"
    if ($LASTEXITCODE -ne 0) { throw "winget failed installing VS Build Tools (exit $LASTEXITCODE)" }
    $restartNote = $true
}

# --- GNU make + JDK 17 -------------------------------------------------------
# make: chạy Makefile. JDK 17: gradle (Android) + ktlint.
if (Install-IfMissing 'make' 'GnuWin32.Make' 'GNU make') { $restartNote = $true }
if (Install-IfMissing 'java' 'EclipseAdoptium.Temurin.17.JDK' 'JDK 17 (Temurin)') { $restartNote = $true }

# --- ktlint + swiftformat (bản ghim, về tools\) -----------------------------
# codestyle.ps1 chỉ DÙNG tool, không tải — mọi thứ cài đặt gom về đây.
$toolsDir = Join-Path $root 'tools'
New-Item -ItemType Directory -Force -Path $toolsDir | Out-Null

$ktlintJar = Join-Path $toolsDir 'ktlint.jar'
if (Test-Path $ktlintJar) {
    Write-Host "[ok]      ktlint ($ktlintJar)"
} else {
    Write-Host "[install] ktlint $ktlintVersion..."
    Invoke-WebRequest -Uri "https://github.com/pinterest/ktlint/releases/download/$ktlintVersion/ktlint" -OutFile $ktlintJar
}

$swiftformatExe = Join-Path $toolsDir 'swiftformat.exe'
$sfOnPath = (Get-Command swiftformat -ErrorAction SilentlyContinue).Source
if ($sfOnPath) {
    Write-Host "[ok]      swiftformat ($sfOnPath)"
} elseif (Test-Path $swiftformatExe) {
    Write-Host "[ok]      swiftformat ($swiftformatExe)"
} else {
    # Release Windows chỉ có MSI — extract bằng msiexec /a (không cài vào máy).
    Write-Host "[install] SwiftFormat $swiftformatVersion..."
    $msi = Join-Path $env:TEMP 'SwiftFormat.amd64.msi'
    $ext = Join-Path $env:TEMP 'SwiftFormatMsiExtract'
    Invoke-WebRequest -Uri "https://github.com/nicklockwood/SwiftFormat/releases/download/$swiftformatVersion/SwiftFormat.amd64.msi" -OutFile $msi
    Start-Process msiexec -ArgumentList "/a `"$msi`" /qn TARGETDIR=`"$ext`"" -Wait
    Copy-Item (Join-Path $ext 'PFiles64\nicklockwood\SwiftFormat\swiftformat.exe') $swiftformatExe
    Remove-Item $msi -Force
    Remove-Item $ext -Recurse -Force
}

# --- Android SDK/NDK --------------------------------------------------------
# Các version phải khớp client/android/app/build.gradle.kts (compileSdk/ndkVersion/cmake).
$sdkRoot = $env:ANDROID_HOME
if (-not $sdkRoot) { $sdkRoot = Join-Path $env:LOCALAPPDATA 'Android\Sdk' }
$sdkmanager = Get-ChildItem -Path (Join-Path $sdkRoot 'cmdline-tools\*\bin\sdkmanager.bat') -ErrorAction SilentlyContinue |
    Select-Object -First 1
if (-not $sdkmanager) {
    Write-Host "[install] Android Studio (bootstrap for SDK)..."
    winget install --id Google.AndroidStudio --exact --accept-source-agreements --accept-package-agreements
    if ($LASTEXITCODE -ne 0) { throw "winget failed installing Android Studio (exit $LASTEXITCODE)" }
    Write-Host "  NOTE: open Android Studio once (installs SDK + cmdline-tools), then re-run bootstrap."
} else {
    Write-Host "[ok]      Android SDK ($sdkRoot)"
    # Đủ package rồi thì khỏi gọi sdkmanager — nó luôn fetch repo qua mạng, chậm và ồn.
    $missing = @('platform-tools', 'platforms\android-37.0', 'ndk\26.1.10909125', 'cmake\3.22.1') |
        Where-Object { -not (Test-Path (Join-Path $sdkRoot $_)) }
    if (-not $missing) {
        Write-Host "[ok]      Android SDK packages (platform 37.0, NDK 26.1.10909125, cmake 3.22.1)"
    } else {
        Write-Host "[install] SDK packages ($($missing -join ', '))..."
        & $sdkmanager.FullName --install 'platform-tools' 'platforms;android-37.0' 'ndk;26.1.10909125' 'cmake;3.22.1'
        if ($LASTEXITCODE -ne 0) { throw "sdkmanager failed (exit $LASTEXITCODE)" }
    }
}

# --- Tổng kết ---------------------------------------------------------------
Write-Host ""
Write-Host "bootstrap: DONE"
Write-Host "  Next: 'make' (build debug), 'make test', 'make lint', 'make run-android'"
if ($restartNote) { Write-Host "  NOTE: open a NEW terminal so PATH changes take effect." }
