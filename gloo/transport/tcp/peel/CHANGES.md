
# Peel Integration - File Changes Summary

## Overview

This document summarizes all files added and modified to integrate the Peel multicast protocol into Gloo.

## New Files Added

### Core Peel Protocol (gloo/transport/tcp/peel/)

| File | Description |
|------|-------------|
| peel_protocol.h | Header definitions, constants, and checksum utilities |
| peel_protocol.cc | Timestamp and checksum implementation |
| peel_redis.h | Redis client wrapper for coordination |
| peel_redis.cc | Redis connection and key-value operations |
| peel_full_mesh.h | Full mesh handshake protocol |
| peel_full_mesh.cc | Peer discovery and connection establishment |
| peel_transport.h | Multicast sender/receiver transport |
| peel_transport.cc | UDP multicast data transmission (without reliability and congestion control) |
| peel_broadcast.h | High-level broadcast API |
| peel_broadcast.cc | Broadcast orchestration for sender/receivers |
| peel_context.h | Gloo integration wrapper |
| peel_context.cc | PeelContext lifecycle management |
| CMakeLists.txt | Build configuration for Peel |

### Test Files (gloo/transport/tcp/peel/)

| File | Description |
|------|-------------|
| test_peel_full_mesh.cc | Tests peer discovery and mesh formation |
| test_peel_gloo_context.cc | Tests Peel integration with Gloo context |
| test_peel_broadcast_example.cc | Tests Peel broadcast functionality |
| test_tcp_broadcast.cc | TCP broadcast test |
| test_peel_broadcast.cc | Peel broadcast test |

## Modified Files

### Gloo Transport TCP (gloo/transport/tcp/)

| File | Change |
|------|--------|
| context.h | Added enablePeel(), isPeelReady(), peelBroadcast() methods |
| context.cc | Implemented Peel integration methods |

### Gloo CMake (gloo/)

| File | Change |
|------|--------|
| CMakeLists.txt | Added add_subdirectory(transport/tcp/peel) and Peel source files |

## File Tree

```css
gloo/
├── CMakeLists.txt                    [MODIFIED]
└── transport/
    └── tcp/
        ├── context.h                 [MODIFIED]
        ├── context.cc                [MODIFIED]
        └── peel/                     [NEW]
            ├── CMakeLists.txt
            │
            │── peel_protocol.h       ─┐
            │── peel_protocol.cc       │ Protocol
            │                         ─┘
            │── peel_redis.h          ─┐
            │── peel_redis.cc          │ Redis
            │                         ─┘
            │── peel_full_mesh.h      ─┐
            │── peel_full_mesh.cc      │ Mesh
            │                         ─┘
            │── peel_transport.h      ─┐
            │── peel_transport.cc      │ Transport
            │                         ─┘
            │── peel_broadcast.h      ─┐
            │── peel_broadcast.cc      │ Broadcast
            │                         ─┘
            │── peel_context.h        ─┐
            │── peel_context.cc        │ Context
            │                         ─┘
            │── test_tcp_broadcast.cc         ─┐
            │── test_peel_broadcast.cc         │
            │── test_peel_full_mesh.cc         │ Tests
            │── test_peel_gloo_context.cc      │
            └── test_peel_broadcast_example.cc─┘
```





## Dependencies

| Dependency | Purpose | Required |
|------------|---------|----------|
| hiredis | Redis client library | Yes |
| Redis server | Coordination and peer discovery | Yes (runtime) |

### Install Dependencies

Ubuntu/Debian:
```bash
sudo apt-get install libhiredis-dev redis-server
```

## Build Configuration

### CMake Options

| Option    | Default | Description                                    |
| --------- | ------- | ---------------------------------------------- |
| USE_REDIS | OFF     | Enable Redis store support (required for Peel) |

### Build Commands

```
bashCopy codecd ~/gloo/build
rm -rf *
cmake .. -DUSE_REDIS=ON
make -j$(nproc)
```


