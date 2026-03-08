#!/bin/bash

# Host 1: 10.161.159.133 (also runs Redis)

cd ~/gloo/build

# Make sure Redis is running
sudo systemctl start redis-server

# Clean old keys (optional)
redis-cli KEYS "peel/*" | xargs -r redis-cli DEL

# Run rank 0
./test_peel_full_mesh \
    --rank 0 \
    --size 2 \
    --iface 10.161.159.133 \
    --redis-host 10.161.159.133 \
    --cleanup