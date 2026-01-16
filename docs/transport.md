# Transport

The transport layer in Gloo abstracts network communication and provides a unified interface across different technologies, defining core abstractions (Device, Context, Pair, Buffer, UnboundBuffer, Address) with implementations in TCP, TCP\_TLS, UV, and ibverbs.

## Key Concepts

**Slots**: Unique identifiers used to match send/receive operations. Both ends must use the same slot ID for communication.

## Core API

The transport API lives in [`gloo/transport/`](../gloo/transport). These classes define the hierarchy of communication objects:

### Device
  Represents a network device.
  Create with `CreateDevice(attr)` using transport-specific attributes (e.g. interface name).
  * `createContext(rank, size)` - Creates communication context for process group

### Context**
  Process group communication state, created from a Device.
  * `createPair(rank)` - Creates communication channel with specific process
  * `createUnboundBuffer(ptr, size)` - Creates flexible buffer
  * `getPair(rank)` - Gets existing communication pair with specified rank
> **Note:**
> Internally, `gloo::Context` owns a `gloo::transport::Context`, which handles the actual transport-level communication.
> In most cases, `gloo::transport::Context` should not be used directly by users.

### Pair
  A bidirectional **connection** between two processes.
  * `createSendBuffer(slot, ptr, size)` / `createRecvBuffer(slot, ptr, size)` - Creates fixed-size buffers tied to specific Pair

### Buffer
  A fixed-size data transfer buffer associated with a `Pair`. Identified by slot ID.
  * `send(offset, length, roffset)` - Send data from local buffer at offset with length to remote buffer at roffset
  * `waitRecv()` / `waitSend()` - Wait for receive/send operations to complete

### UnboundBuffer
  A flexible buffer not tied to a specific pair or slot. Supports multiple communication patterns.
  * `send(dstRank, slot, offset, nbytes)` - Send data to destination rank using specified slot
  * `recv(srcRank, slot, offset, nbytes)` - Receive data from source rank using specified slot
  * `recv(srcRanks, slot, offset, nbytes)` - Receive data from multiple source ranks
  * `put(key, slot, offset, roffset, nbytes)` - One-sided put operation using remote key
  * `get(key, slot, offset, roffset, nbytes)` - One-sided get operation using remote key
  * `waitRecv(rank, timeout)` / `waitSend(rank, timeout)` - Wait operations with optional rank and timeout

### Address
  Represents a network address for transport endpoints.

## How to Use

### Choose a transport implementation
   * **TCP**: Linux-only, reliable sockets (default choice)
   * **TCP\_TLS**: Linux-only, encrypted TCP with OpenSSL
   * **UV**: Cross-platform (Linux/macOS/Windows), async I/O
   * **ibverbs**: HPC clusters with InfiniBand/RoCE

### Initialize a `Device` and `Context`
   For rendezvous store details, see [rendezvous](./rendezvous.md) documentation.
   ```cpp
    // Define TCP transport attributes
    gloo::transport::tcp::attr attr;
    attr.iface = "eth0";
    auto device = gloo::transport::tcp::CreateDevice(attr);

    gloo::rendezvous::FileStore store("/tmp/gloo_rendezvous");

    // Create a rendezvous context with the current process rank and world size
    auto context = std::make_shared<gloo::rendezvous::Context>(rank, size);

    // Establish a full mesh of connections between all processes
    // Uses the FileStore for rendezvous and the chosen TCP device for transport

    // NOTE: Calling connectFullMesh() will internally create and initialize
    // a gloo::transport::Context (based on the given Device).
    // Users interact with gloo::rendezvous::Context here, while the transport::Context
    // remains an implementation detail that manages the actual network connections.
    context->connectFullMesh(store, device);
   ```

### Use `Buffer`
   ```cpp
   std::array<float, processCount> data;
   std::unique_ptr<transport::Buffer> sendBuffer;
   std::unique_ptr<transport::Buffer> recvBuffer;

   if (context->rank == 0) {
     auto& other = context->getPair(1);
     sendBuffer = other->createSendBuffer(0, data.data(), data.size() * sizeof(float));
     recvBuffer = other->createRecvBuffer(1, data.data(), data.size() * sizeof(float));
   }
   if (context->rank == 1) {
     auto& other = context->getPair(0);
     recvBuffer = other->createRecvBuffer(0, data.data(), data.size() * sizeof(float));
     sendBuffer = other->createSendBuffer(1, data.data(), data.size() * sizeof(float));
   }

   // Set value indexed on this process' rank
   data[context->rank] = context->rank + 1000;

   // Send value to the remote buffer (send from local offset to remote offset)
   auto offset = context->rank * sizeof(float);
   sendBuffer->send(offset, sizeof(float), offset);
   sendBuffer->waitSend();

   // Wait for receive
   recvBuffer->waitRecv();
   ```

### Use `UnboundBuffer`
   ```cpp
   std::vector<float> bigdata(1 << 20);
   auto ubuf = context->createUnboundBuffer(bigdata.data(), bigdata.size() * sizeof(float));
   
   // Send data to rank 0 using slot 4096, starting from offset 0, sending 4KB
   ubuf->send(0, 4096, 0, 4096);
   
   // Receive data from rank 1 using slot 8192, starting from offset 4096, receiving 8KB  
   ubuf->recv(1, 8192, 4096, 8192);
   
   // Wait for operations to complete
   ubuf->waitSend();
   ubuf->waitRecv();
   ```

### Implement custom transport
   * Derive from `Device`, `Context`, `Pair`, `Buffer`, `UnboundBuffer`, `Address`
   * Implement required virtual methods with same API contract


