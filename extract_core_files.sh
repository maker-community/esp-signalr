#!/bin/bash
# extract_core_files.sh
# Script to extract SignalR-Client-Cpp core files

set -e

if [ $# -ne 1 ]; then
    echo "Usage: $0 <path-to-SignalR-Client-Cpp-repo>"
    echo "Example: $0 /tmp/SignalR-Client-Cpp"
    exit 1
fi

SOURCE_REPO="$1"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TARGET_DIR="$SCRIPT_DIR"

if [ ! -d "$SOURCE_REPO" ]; then
    echo "Error: Source repository not found at $SOURCE_REPO"
    echo ""
    echo "Please clone the SignalR-Client-Cpp repository first:"
    echo "  git clone https://github.com/aspnet/SignalR-Client-Cpp.git $SOURCE_REPO"
    exit 1
fi

echo "Extracting SignalR core files from $SOURCE_REPO"
echo "Target directory: $TARGET_DIR"
echo ""

# Create directories
mkdir -p "$TARGET_DIR/src/signalrclient"
mkdir -p "$TARGET_DIR/include/signalrclient"
mkdir -p "$TARGET_DIR/third_party_code/cpprestsdk"

# Extract core source files
echo "Copying core source files..."
CORE_FILES=(
    "callback_manager.cpp"
    "cancellation_token.cpp"
    "cancellation_token_source.cpp"
    "connection_impl.cpp"
    "handshake_protocol.cpp"
    "hub_connection.cpp"
    "hub_connection_builder.cpp"
    "hub_connection_impl.cpp"
    "json_helpers.cpp"
    "json_hub_protocol.cpp"
    "logger.cpp"
    "negotiate.cpp"
    "signalr_client_config.cpp"
    "signalr_value.cpp"
    "trace_log_writer.cpp"
    "transport.cpp"
    "transport_factory.cpp"
    "url_builder.cpp"
    "websocket_transport.cpp"
    "signalr_default_scheduler.cpp"
)

for file in "${CORE_FILES[@]}"; do
    if [ -f "$SOURCE_REPO/src/signalrclient/$file" ]; then
        cp "$SOURCE_REPO/src/signalrclient/$file" "$TARGET_DIR/src/signalrclient/"
        echo "  ✓ $file"
    else
        echo "  ✗ $file (not found)"
    fi
done

# Extract header files
echo ""
echo "Copying header files..."
if [ -d "$SOURCE_REPO/include/signalrclient" ]; then
    cp -r "$SOURCE_REPO/include/signalrclient/"*.h "$TARGET_DIR/include/signalrclient/" 2>/dev/null || true
    echo "  ✓ Header files copied"
fi

# Extract URI utilities
echo ""
echo "Copying URI utilities..."
URI_FILES=(
    "uri.cpp"
    "uri_builder.cpp"
    "uri.h"
    "uri_builder.h"
)

for file in "${URI_FILES[@]}"; do
    if [ -f "$SOURCE_REPO/third_party_code/cpprestsdk/$file" ]; then
        cp "$SOURCE_REPO/third_party_code/cpprestsdk/$file" "$TARGET_DIR/third_party_code/cpprestsdk/"
        echo "  ✓ $file"
    else
        echo "  ✗ $file (not found)"
    fi
done

echo ""
echo "Extraction complete!"
echo ""
echo "Next steps:"
echo "1. Review the extracted files in $TARGET_DIR"
echo "2. Uncomment the source files in CMakeLists.txt"
echo "3. Apply JSON adapter modifications (Phase 3)"
echo "4. Apply scheduler modifications (Phase 4)"
echo "5. Build with: idf.py build"
