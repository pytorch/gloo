#!/bin/bash

cd ~/gloo

# Pull latest code
git pull origin main

# Build
cd build
make -j$(nproc)

# Compile test
g++ -std=c++17 -O2 -g \
    -I.. \
    ../gloo/transport/tcp/peel/test_peel_gloo_context.cc \
    ./gloo/libgloo.a \
    -lpthread \
    -o test_peel_gloo_context

echo "To run as sender (rank 0):"
echo "  ./test_peel_gloo_context --rank 0 --size 2 --iface enp2s0"
echo "To run as receiver (rank 1):"
echo "  ./test_peel_gloo_context --rank 1 --size 2 --iface eno16np0"
