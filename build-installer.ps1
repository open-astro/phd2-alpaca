# PHD2 Installer Build Script for Windows
# This script builds PHD2 and creates an installer using InnoSetup
#
# Builds x86 only to match official PHD2 (C:\Program Files (x86)\PHDGuiding2\) and to
# get the full camera list in the wizard (Atik, LE webcams, WDM, etc. are 32-bit only in cameras.h).

$ErrorActionPreference = "Stop"

Write-Host "Building PHD2 Installer (x86)" -ForegroundColor Green

# Determine paths
if ($PSScriptRoot) {
    $RootDir = $PSScriptRoot
} else {
    $RootDir = Split-Path -Parent $MyInvocation.MyCommand.Path
}
Set-Location $RootDir

# Extract version from version.md
Write-Host "Extracting version from version.md..." -ForegroundColor Yellow
$versionFilePath = Join-Path $RootDir "version.md"
if (-not (Test-Path $versionFilePath)) {
    Write-Error "Cannot find version.md at: $versionFilePath"
    Write-Host "Current directory: $(Get-Location)" -ForegroundColor Yellow
    exit 1
}
$versionFile = Get-Content $versionFilePath -Raw
$versionMatch = [regex]::Match($versionFile, '(?m)^\s*([0-9]+\.[0-9]+\.[0-9]+([A-Za-z0-9._-]*))\s*$')
if (-not $versionMatch.Success) {
    Write-Error "Could not extract version from version.md (expected line like 1.2.3 or 1.2.3rc1)"
    exit 1
}
$fullVersion = $versionMatch.Groups[1].Value

Write-Host "Detected version: $fullVersion" -ForegroundColor Green

# x86 only (matches official PHD2 release)
$BuildDir = "tmp"
$CMakeArch = "Win32"
$InstallerArch = ""
$InstallerTemplate = "phd2-x86.iss.in"

# Clean build directory to ensure a fresh build each run
if (Test-Path $BuildDir) {
    Write-Host "Cleaning build directory: $BuildDir" -ForegroundColor Yellow
    Remove-Item -Path $BuildDir -Recurse -Force
}

# Check for required tools
Write-Host "Checking for required tools..." -ForegroundColor Yellow

# Check CMake
$cmakePath = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmakePath) {
    $cmakePath = "C:\Program Files\CMake\bin\cmake.exe"
    if (-not (Test-Path $cmakePath)) {
        Write-Error "CMake not found. Please install CMake and ensure it's in your PATH, or install it to the default location."
        exit 1
    }
}
Write-Host "  CMake: $cmakePath" -ForegroundColor Green

# Check InnoSetup
$isccPath = "C:\Program Files (x86)\Inno Setup 5\ISCC.exe"
if (-not (Test-Path $isccPath)) {
    $isccPath = "C:\Program Files\Inno Setup 5\ISCC.exe"
    if (-not (Test-Path $isccPath)) {
        Write-Error "InnoSetup not found. Please install Inno Setup 5 to one of the default locations."
        Write-Host "  Expected locations:" -ForegroundColor Yellow
        Write-Host "    C:\Program Files (x86)\Inno Setup 5\ISCC.exe" -ForegroundColor Yellow
        Write-Host "    C:\Program Files\Inno Setup 5\ISCC.exe" -ForegroundColor Yellow
        exit 1
    }
}
Write-Host "  InnoSetup: $isccPath" -ForegroundColor Green

# Create build directory
Write-Host "Creating build directory: $BuildDir" -ForegroundColor Yellow
if (-not (Test-Path $BuildDir)) {
    New-Item -ItemType Directory -Path $BuildDir | Out-Null
}

Set-Location $BuildDir

# Configure CMake
Write-Host "Configuring CMake for $CMakeArch..." -ForegroundColor Yellow
$cmakeArgs = @(
    "-Wno-dev",
    "-A", $CMakeArch,
    ".."
)

# Add vcpkg if available
if ($env:VCPKG_ROOT) {
    $cmakeArgs += "-DVCPKG_ROOT=$env:VCPKG_ROOT"
    Write-Host "  Using VCPKG_ROOT: $env:VCPKG_ROOT" -ForegroundColor Cyan
}

& $cmakePath $cmakeArgs
if ($LASTEXITCODE -ne 0) {
    Write-Error "CMake configuration failed"
    exit 1
}

# Build the project
Write-Host "Building PHD2 in Release configuration..." -ForegroundColor Yellow
& $cmakePath --build . --config Release
if ($LASTEXITCODE -ne 0) {
    Write-Error "Build failed"
    exit 1
}

# Run tests (optional, but good practice)
Write-Host "Running tests..." -ForegroundColor Yellow
$ctestPath = Get-Command ctest -ErrorAction SilentlyContinue
if (-not $ctestPath) {
    $ctestPath = "C:\Program Files\CMake\bin\ctest.exe"
}
if (Test-Path $ctestPath) {
    & $ctestPath --build-config Release
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "Some tests failed, but continuing with installer creation..."
    }
} else {
    Write-Warning "CTest not found, skipping tests"
}

# Generate installer script from template
Write-Host "Generating installer script..." -ForegroundColor Yellow
$issTemplate = Join-Path $RootDir $InstallerTemplate
$issContent = Get-Content $issTemplate -Raw
$issContent = $issContent -replace '@VERSION@', $fullVersion
$issOutput = Join-Path (Get-Location) "phd2.iss"
# Use UTF-8 without BOM (InnoSetup doesn't like BOM)
$utf8NoBom = New-Object System.Text.UTF8Encoding $false
[System.IO.File]::WriteAllText($issOutput, $issContent, $utf8NoBom)

# Generate README from template
Write-Host "Generating README..." -ForegroundColor Yellow
$readmeTemplate = Join-Path $RootDir "README-PHD2.txt.in"
if (Test-Path $readmeTemplate) {
    $readmeContent = Get-Content $readmeTemplate -Raw
    $readmeContent = $readmeContent -replace '@VERSION@', $fullVersion
    $readmeOutput = Join-Path (Get-Location) "README-PHD2.txt"
    # Use UTF-8 without BOM
    $utf8NoBom = New-Object System.Text.UTF8Encoding $false
    [System.IO.File]::WriteAllText($readmeOutput, $readmeContent, $utf8NoBom)
}

# Create installer
Write-Host "Creating installer..." -ForegroundColor Yellow
$installerName = "phd2$InstallerArch-v$fullVersion-installer"
& $isccPath $issOutput "/F$installerName"

if ($LASTEXITCODE -ne 0) {
    Write-Error "Installer creation failed"
    exit 1
}

# Check if installer was created
$installerPath = Join-Path (Get-Location) "$installerName.exe"
if (Test-Path $installerPath) {
    Write-Host ""
    Write-Host "========================================" -ForegroundColor Green
    Write-Host "Installer created successfully!" -ForegroundColor Green
    Write-Host "Location: $installerPath" -ForegroundColor Cyan
    Write-Host "========================================" -ForegroundColor Green
    Write-Host ""
    
    # Get file size
    $fileInfo = Get-Item $installerPath
    $fileSizeMB = [math]::Round($fileInfo.Length / 1MB, 2)
    Write-Host "Installer size: $fileSizeMB MB" -ForegroundColor Cyan
} else {
    Write-Error "Installer file not found at expected location: $installerPath"
    exit 1
}
