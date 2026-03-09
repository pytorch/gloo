
# Peel Testing Guide

## Overview

This document describes how to build and run the Peel test programs.

## Table of Contents

1. [Prerequisites](#prerequisites)
2. [Building Tests](#building-tests)
3. [Test Programs](#test-programs)
4. [Running Tests](#running-tests)
5. [Expected Output](#expected-output)

---

## Prerequisites

### 1. Build Gloo with Redis Support

```bash
cd ~/gloo/build
rm -rf *
cmake .. -DUSE_REDIS=ON
make -j$(nproc)
```

### 2. Install Dependencies

```
# Ubuntu/Debian
sudo apt-get install libhiredis-dev redis-server
```

### 3. Start Redis Server

```
# Start Redis on one node (accessible by all test nodes)
redis-server --bind 0.0.0.0 --port 6379 --daemonize yes

# Verify it's running
redis-cli -h <REDIS_IP> ping
# Expected output: PONG
```

### 4. Identify Network Interface

```
# List interfaces
ip addr show

# Find the interface with your network IP
# Common names: eth0, ens33, vmbr0, enp0s3
```

### 5. Verify Multicast Support

```
# Check if interface supports multicast
ip link show <IFACE> | grep MULTICAST

# Expected: MULTICAST should be in the flags
# Example: <BROADCAST,MULTICAST,UP,LOWER_UP>
```

------

## Building Tests

After building Gloo, test executables are located at:

```
~/gloo/build/gloo/transport/tcp/peel/
```

### Test Executables

| Executable                  | Description                         |
| --------------------------- | ----------------------------------- |
| test_tcp_broadcast          | TCP broadcast test                  |
| test_peel_broadcast         | Peel broadcast test                 |
| test_peel_full_mesh         | Mesh formation test (old)           |
| test_peel_gloo_context      | Gloo context integration test (old) |
| test_peel_broadcast_example | Simple broadcast example (old)      |

------

## Test Programs

### test_tcp_broadcast

**Purpose:** Baseline TCP broadcast using Gloo's standard point-to-point communication.

**Usage:**

```
./test_tcp_broadcast <rank> <world_size> <redis_host> [redis_port] [iface]
```

**Arguments:**

| Argument   | Required | Default | Description               |
| ---------- | -------- | ------- | ------------------------- |
| rank       | Yes      | -       | Process rank (0 = sender) |
| world_size | Yes      | -       | Total number of processes |
| redis_host | Yes      | -       | Redis server IP address   |
| redis_port | No       | 6379    | Redis server port         |
| iface      | No       | auto    | Network interface name    |

**What It Tests:**

- Redis rendezvous and peer discovery
- TCP mesh formation (point-to-point connections)
- Broadcast correctness (data from rank 0 reaches all others)
- Data integrity verification

------

### test_peel_broadcast

**Purpose:** Peel multicast broadcast test.

**Usage:**

```
./test_peel_broadcast <rank> <world_size> <redis_host> [redis_port] [iface] [mcast_group] [mcast_port]
```

**Arguments:**

| Argument    | Required | Default     | Description               |
| ----------- | -------- | ----------- | ------------------------- |
| rank        | Yes      | -           | Process rank (0 = sender) |
| world_size  | Yes      | -           | Total number of processes |
| redis_host  | Yes      | -           | Redis server IP address   |
| redis_port  | No       | 6379        | Redis server port         |
| iface       | No       | auto        | Network interface name    |
| mcast_group | No       | 239.255.0.1 | Multicast group address   |
| mcast_port  | No       | 5000        | Multicast port            |

**What It Tests:**

- Redis coordination for Peel
- UDP multicast socket setup
- Multicast group join
- Reliable data transmission with ACKs
- **Retransmission** on packet loss
- Data integrity verification

------

### test_peel_full_mesh

**Purpose:** Tests only the mesh formation (peer discovery) without data transfer.

**Usage:**

```
./test_peel_full_mesh <rank> <world_size> <redis_host> [redis_port]
```

**What It Tests:**

- Redis connectivity
- Peer address publication
- Peer address discovery
- Barrier synchronization

------

### test_peel_gloo_context

**Purpose:** Tests Peel integration with Gloo's TCP context.

**Usage:**

```
./test_peel_gloo_context <rank> <world_size> <redis_host> [redis_port] [iface]
```

**What It Tests:**

- TCP context creation
- enablePeel() API
- isPeelReady() API
- peelBroadcast() API
- Integration correctness

------

## Running Tests

### Multi-Node (Real Network)

#### Step 1: Clear Redis

```
redis-cli -h <REDIS_IP> FLUSHALL
```

#### Step 2: Start Node with Redis First (Recommended)

```
# Fns101
./test_peel_broadcast 0 3 10.161.159.133 6379 eno16np0 239.255.0.1 5000
```

#### Step 3: Start Other Nodes

```
# Fns102
./test_peel_broadcast 1 3 10.161.159.133 6379 enp2s0 239.255.0.1 5000

# NetX5
./test_peel_broadcast 2 3 10.161.159.133 6379 vmbr0 239.255.0.1 5000
```

**Important:** All nodes must start within the timeout period (default 30 seconds).

------

## Expected Output

### test_tcp_broadcast (Rank 0 - Sender)

```
[TCP] Rank 0/3 connecting to Redis at 10.161.159.133:6379
[TCP] Rank 0: Using interface eno16np0
[TCP] Rank 0: TCP device created
[/home/ete_bosh/gloo/gloo/transport/tcp/context.cc:237] INFO Rank 0 is connected to 2. Expected number of connected peers is: 2
[TCP] Rank 0: Full mesh connected
[TCP] Rank 0: Sender - broadcasting 1048576 uint32s (4194304 bytes)
[TCP] Rank 0: Broadcast completed in 54.758 ms (73.0487 MB/s)
[TCP] Rank 0: SUCCESS - Data verified correctly
[TCP] Rank 0: Exiting
```

### test_tcp_broadcast (Rank 1 - Receiver)

```
[TCP] Rank 1/3 connecting to Redis at 10.161.159.133:6379
[TCP] Rank 1: TCP device created
[/home/ete_bosh/gloo/gloo/transport/tcp/context.cc:237] INFO Rank 1 is connected to 2. Expected number of connected peers is: 2
[TCP] Rank 1: Full mesh connected
[TCP] Rank 1: Receiver - waiting for broadcast
[TCP] Rank 1: Broadcast completed in 70.387 ms (56.8287 MB/s)
[TCP] Rank 1: SUCCESS - Data verified correctly
[TCP] Rank 1: Exiting
```

### test_peel_broadcast (Rank 0 - Sender)

```
[PEEL] Rank 0/3 connecting to Redis at 10.161.159.133:6379
[PEEL] Rank 0: Using interface eno16np0
[PEEL] Rank 0: Multicast group 239.255.0.1:5000
[PEEL] Rank 0: Using prefix 'peel_test_177303437'
[PEEL] Rank 0: TCP device created
[/home/ete_bosh/gloo/gloo/transport/tcp/context.cc:237] INFO Rank 0 is connected to 2. Expected number of connected peers is: 2
[PEEL] Rank 0: TCP context connected
[PEEL] Rank 0: Enabling Peel with config:
       mcast_group: 239.255.0.1
       base_port:   5000
       iface_ip:    eno16np0
peel[0]: initializing...
peel_redis: connected to 10.161.159.133:6379
peel[0]: initialized
peel[0]: starting handshake...
peel[0]: all ranks ready
peel[0]: SYN attempt 1, syn=1/3, ack=1/3
peel[0]: SYN from rank 1 (10.161.159.71:5001)
peel[0]: ACK from rank 1
peel[0]: SYN from rank 2 (10.161.159.35:5002)
peel[0]: ACK from rank 2
peel[0]: sent START
peel[0]: handshake complete
peel_transport[0]: ready
peel_context[0]: initialized
peel: enabled and ready for rank 0 (world_size=3)
[PEEL] Rank 0: Peel initialized and ready
[PEEL] Rank 0: Sender - broadcasting 1048576 uint32s (4194304 bytes)
[PEEL] Rank 0: Broadcast completed in 35.031 ms (114.185 MB/s)
[PEEL] Rank 0: SUCCESS - Data verified correctly
[PEEL] Rank 0: Exiting
```

### test_peel_broadcast (Rank 2 - Receiver)

```
[PEEL] Rank 2/3 connecting to Redis at 10.161.159.133:6379
[PEEL] Rank 2: Using interface vmbr0
[PEEL] Rank 2: Multicast group 239.255.0.1:5000
[PEEL] Rank 2: Using prefix 'peel_test_177303437'
[PEEL] Rank 2: TCP device created
[/home/ete_bosh/gloo/gloo/transport/tcp/context.cc:237] INFO Rank 2 is connected to 2. Expected number of connected peers is: 2
[PEEL] Rank 2: TCP context connected
[PEEL] Rank 2: Enabling Peel with config:
       mcast_group: 239.255.0.1
       base_port:   5000
       iface_ip:    vmbr0
peel[2]: initializing...
peel_redis: connected to 10.161.159.133:6379
peel[2]: initialized
peel[2]: starting handshake...
peel[2]: all ranks ready
peel[2]: SYN attempt 1, syn=1/3, ack=1/3
peel[2]: SYN from rank 0 (10.161.159.133:5000)
peel[2]: SYN from rank 1 (10.161.159.71:5001)
peel[2]: ACK from rank 0
peel[2]: ACK from rank 1
peel[2]: sent START
peel[2]: handshake complete
peel_transport[2]: ready
peel_context[2]: initialized
peel: enabled and ready for rank 2 (world_size=3)
[PEEL] Rank 2: Peel initialized and ready
[PEEL] Rank 2: Receiver - waiting for broadcast
[PEEL] Rank 2: Broadcast completed in 35.064 ms (114.077 MB/s)
[PEEL] Rank 2: SUCCESS - Data verified correctly
[PEEL] Rank 2: Exiting
```