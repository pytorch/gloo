#!/bin/bash
set -e

cd ~/gloo

# Ensure hiredis is installed
if ! pkg-config --exists hiredis 2>/dev/null; then
    echo "Installing hiredis..."
    sudo apt update
    sudo apt install -y libhiredis-dev
fi

# Create build directory
mkdir -p build
cd build

# Compile peel full-mesh test
echo "Compiling peel full-mesh test..."

g++ -std=c++17 -O2 -g \
    -I.. \
    ../gloo/transport/tcp/peel/peel_protocol.cc \
    ../gloo/transport/tcp/peel/peel_redis.cc \
    ../gloo/transport/tcp/peel/peel_full_mesh.cc \
    ../gloo/transport/tcp/peel/test_peel_full_mesh.cc \
    -lhiredis \
    -lpthread \
    -o test_peel_full_mesh

echo ""
echo "Build successful!"
echo ""
echo "Usage:"
echo "  Host 1 (Redis server): ./test_peel_full_mesh --rank 0 --size 2 --iface <IP1> --redis-host <IP1>"
echo "  Host 2:                ./test_peel_full_mesh --rank 1 --size 2 --iface <IP2> --redis-host <IP1>"