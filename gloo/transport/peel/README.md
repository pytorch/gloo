# Gloo Peel transport

Peel is a standalone Gloo transport-style module. It is intentionally placed in
`gloo/transport/peel` rather than below `gloo/transport/tcp` because its data
path is UDP/raw-socket multicast plus PEEL routing rules, not TCP.

## Layout

```text
gloo/transport/peel/
  peel_protocol.{h,cc}                 packet format and constants
  peel_redis.{h,cc}                    Redis helper used by discovery
  peel_discovery.{h,cc}                rank -> interface IP discovery
  peel_full_mesh.{h,cc}                AF_PACKET sockets and handshake
  peel_tree.{h,cc}                     topology parsing and PEEL/CIDR routing
  peel_transport.{h,cc}                data-path send/receive worker
  peel_context.{h,cc}                  lifecycle owner for Peel transports
  peel_broadcast*.{h,cc}               Peel broadcast collectives
  peel_allgather*.{h,cc}               Peel allgather/allreduce collectives
```

The public namespace is:

```cpp
gloo::transport::peel
```

## CMake

Peel is built when `USE_PEEL=ON` and the platform is Linux. It is added from
`gloo/transport/CMakeLists.txt` as a sibling of `tcp`, `ibverbs`, and `uv`.

```bash
cmake ... -DUSE_PEEL=ON
```

Peel currently requires `hiredis` because `PeelDiscovery` uses Redis for
rendezvous/discovery.

## Minimal invocation pattern

```cpp
#include "gloo/transport/peel/peel_context.h"
#include "gloo/transport/peel/peel_discovery.h"

namespace peel = gloo::transport::peel;

peel::PeelDiscoveryConfig dc;
dc.rank = rank;
dc.world_size = worldSize;
dc.redis_host = redisHost;
dc.redis_port = redisPort;
dc.redis_prefix = prefix + "/peel_ip";
dc.iface_name = iface;

peel::PeelDiscovery discovery(dc);
GLOO_ENFORCE(discovery.run(), "PeelDiscovery failed");

peel::PeelContextConfig cfg;
cfg.rank = rank;
cfg.world_size = worldSize;
cfg.peer_ips = discovery.peerIps();
cfg.mcast_group = mcastGroup;
cfg.base_port = basePort;
cfg.iface_name = iface;
cfg.topology_file = topologyFile;

peel::PeelContext ctx(cfg);
GLOO_ENFORCE(ctx.init(), "PeelContext init failed");
GLOO_ENFORCE(ctx.broadcast(root, buffer, bytes), "Peel broadcast failed");
ctx.cleanup();
```

## Compatibility note

The previous layout embedded Peel under `gloo/transport/tcp/peel` and exposed
TCP-context methods such as `tcpCtx->enablePeel()` and `tcpCtx->peelBroadcast()`.
Those methods have been removed from `gloo::transport::tcp::Context`; callers
should create and own a `gloo::transport::peel::PeelContext` directly.
