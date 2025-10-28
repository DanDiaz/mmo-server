#!/usr/bin/env bash

set -e
BUILD_DIR="build"
BUILD_TYPE="Debug"

# Parse optional arguments
if [ "$1" == "release" ] || [ "$1" == "Release" ]; then
    BUILD_TYPE="Release"
fi

echo "🛠️  Building MMOProject ($BUILD_TYPE)..."
echo

# Step 1: Configure (generate if missing)
if [ ! -d "$BUILD_DIR" ]; then
    echo "📁 Creating build directory..."
    mkdir -p "$BUILD_DIR"
fi

echo "⚙️  Configuring CMake..."
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=$BUILD_TYPE

# Step 2: Build
echo "🚧 Building..."
cmake --build "$BUILD_DIR" --config $BUILD_TYPE -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

echo
echo "✅ Build complete!"
echo "Executables are in: $BUILD_DIR/"
