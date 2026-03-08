#!/bin/bash

# Host 2: 10.161.159.71 (connects to Redis on Host 1)

cd ~/gloo/build

# Run rank 1
./test_peel_full_mesh \
    --rank 1 \
    --size 2 \
    --iface 10.161.159.71 \
    --redis-host 10.161.159.133