# extract_core_files.ps1
# Script to extract SignalR-Client-Cpp core files for Windows

param(
    [Parameter(Mandatory=$true)]
    [string]$SourceRepo
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $SourceRepo)) {
    Write-Host "Error: Source repository not found at $SourceRepo" -ForegroundColor Red
    Write-Host ""
    Write-Host "Please clone the SignalR-Client-Cpp repository first:"
    Write-Host "  git clone https://github.com/aspnet/SignalR-Client-Cpp.git $SourceRepo"
    exit 1
}

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$TargetDir = $ScriptDir

Write-Host "Extracting SignalR core files from $SourceRepo"
Write-Host "Target directory: $TargetDir"
Write-Host ""

# Create directories
New-Item -ItemType Directory -Force -Path "$TargetDir\src\signalrclient" | Out-Null
New-Item -ItemType Directory -Force -Path "$TargetDir\include\signalrclient" | Out-Null
New-Item -ItemType Directory -Force -Path "$TargetDir\third_party_code\cpprestsdk" | Out-Null

# Extract core source files
Write-Host "Copying core source files..."
$CoreFiles = @(
    "callback_manager.cpp",
    "cancellation_token.cpp",
    "cancellation_token_source.cpp",
    "connection_impl.cpp",
    "handshake_protocol.cpp",
    "hub_connection.cpp",
    "hub_connection_builder.cpp",
    "hub_connection_impl.cpp",
    "json_helpers.cpp",
    "json_hub_protocol.cpp",
    "logger.cpp",
    "negotiate.cpp",
    "signalr_client_config.cpp",
    "signalr_value.cpp",
    "trace_log_writer.cpp",
    "transport.cpp",
    "transport_factory.cpp",
    "url_builder.cpp",
    "websocket_transport.cpp",
    "signalr_default_scheduler.cpp"
)

foreach ($file in $CoreFiles) {
    $sourcePath = Join-Path $SourceRepo "src\signalrclient\$file"
    if (Test-Path $sourcePath) {
        Copy-Item $sourcePath "$TargetDir\src\signalrclient\" -Force
        Write-Host "  ✓ $file" -ForegroundColor Green
    } else {
        Write-Host "  ✗ $file (not found)" -ForegroundColor Yellow
    }
}

# Extract header files
Write-Host ""
Write-Host "Copying header files..."
$headerPath = Join-Path $SourceRepo "include\signalrclient"
if (Test-Path $headerPath) {
    Copy-Item "$headerPath\*.h" "$TargetDir\include\signalrclient\" -Force -ErrorAction SilentlyContinue
    Write-Host "  ✓ Header files copied" -ForegroundColor Green
}

# Extract URI utilities
Write-Host ""
Write-Host "Copying URI utilities..."
$UriFiles = @(
    "uri.cpp",
    "uri_builder.cpp",
    "uri.h",
    "uri_builder.h"
)

foreach ($file in $UriFiles) {
    $sourcePath = Join-Path $SourceRepo "third_party_code\cpprestsdk\$file"
    if (Test-Path $sourcePath) {
        Copy-Item $sourcePath "$TargetDir\third_party_code\cpprestsdk\" -Force
        Write-Host "  ✓ $file" -ForegroundColor Green
    } else {
        Write-Host "  ✗ $file (not found)" -ForegroundColor Yellow
    }
}

Write-Host ""
Write-Host "Extraction complete!" -ForegroundColor Green
Write-Host ""
Write-Host "Next steps:"
Write-Host "1. Review the extracted files in $TargetDir"
Write-Host "2. Uncomment the source files in CMakeLists.txt"
Write-Host "3. Apply JSON adapter modifications (Phase 3)"
Write-Host "4. Apply scheduler modifications (Phase 4)"
Write-Host "5. Build with: idf.py build"
