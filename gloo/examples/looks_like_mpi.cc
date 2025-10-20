/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 */

#include <sys/types.h>
#include <unistd.h>

#include <iostream>
#include <memory>
#include <string>

#include <gloo/allreduce.h>
#include <gloo/barrier.h>
#include <gloo/rendezvous/context.h>
#include <gloo/rendezvous/file_store.h>
#include <gloo/rendezvous/prefix_store.h>
#include <gloo/transport/tcp/device.h>

#define ASSERT(expr)                                        \
  do {                                                      \
    if (!(expr)) {                                          \
      throw std::runtime_error("Assertion failed: " #expr); \
    }                                                       \
  } while (0);

// Global context
std::shared_ptr<gloo::Context> kContext;

// Make this example look like MPI code
using MPI_Comm = int;
const MPI_Comm MPI_COMM_WORLD = 0;

enum MPI_Datatype {
  MPI_INT,
};

enum MPI_Op {
  MPI_SUM,
};

// Same prototype as MPI API.
int MPI_Comm_rank(MPI_Comm comm, int* rank) {
  ASSERT(comm == MPI_COMM_WORLD);
  if (rank) {
    *rank = kContext->rank;
  }
}

// Same prototype as MPI API.
int MPI_Comm_size(MPI_Comm comm, int* size) {
  ASSERT(comm == MPI_COMM_WORLD);
  if (size) {
    *size = kContext->size;
  }
}

// Same prototype as MPI API.
int MPI_Barrier(MPI_Comm comm) {
  ASSERT(comm == MPI_COMM_WORLD);
  gloo::BarrierOptions opts(kContext);
  gloo::barrier(opts);
}

// Same prototype
int MPI_Allreduce(
    const void* sendbuf,
    void* recvbuf,
    int count,
    MPI_Datatype datatype,
    MPI_Op op,
    MPI_Comm comm) {
  ASSERT(datatype == MPI_INT);
  ASSERT(op == MPI_SUM);
  ASSERT(comm == MPI_COMM_WORLD);
  gloo::AllreduceOptions opts(kContext);
  opts.setInput(const_cast<int*>((const int*)sendbuf), count);
  opts.setOutput((int*)recvbuf, count);
  opts.setReduceFunction(
      static_cast<void (*)(void*, const void*, const void*, size_t)>(
          &gloo::sum<int>));
  gloo::allreduce(opts);
}

// Actual prototype:
//
// int MPI_Recv(
//     void *buf,
//     int count,
//     MPI_Datatype datatype,
//     int source,
//     int tag,
//     MPI_Comm comm,
//     MPI_Status *status);
//
// Implementation below:
//   - doesn't use MPI_Datatype
//   - doesn't take MPI_Comm argument but uses global
//   - doesn't return an MPI_Status object
//
int MPI_Recv(void* buf, ssize_t bytes, int source, int tag, MPI_Comm comm) {
  auto ubuf = kContext->createUnboundBuffer(buf, bytes);
  ubuf->recv(source, tag);
  ubuf->waitRecv();
}

// Actual prototype:
//
// int MPI_Send(
//     const void *buf,
//     int count,
//     MPI_Datatype datatype,
//     int dest,
//     int tag,
//     MPI_Comm comm);
//
// Implementation below:
//   - doesn't use MPI_Datatype
//
int MPI_Send(
    const void* cbuf,
    ssize_t bytes,
    int dest,
    int tag,
    MPI_Comm comm) {
  // Argument is logically const if we're only sending.
  auto ubuf = kContext->createUnboundBuffer(const_cast<void*>(cbuf), bytes);
  ubuf->send(dest, tag);
  ubuf->waitSend();
}

// Entrypoint of this example.
int run() {
  int rank;
  int size;

  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &size);

  // Send on rank 0
  if (rank == 0) {
    const int dst = 1;
    const int tag = 1234;
    int pid = getpid();
    MPI_Send(&pid, sizeof(pid), dst, tag, MPI_COMM_WORLD);
    std::cout << "Sent to rank " << dst << ": " << pid << std::endl;
  }

  // Recv on rank 1
  if (rank == 1) {
    const int src = 0;
    const int tag = 1234;
    int pid = -1;
    MPI_Recv(&pid, sizeof(pid), src, tag, MPI_COMM_WORLD);
    std::cout << "Received from rank " << src << ": " << pid << std::endl;
  }

  // Run allreduce on the number 1
  {
    const int input = 1;
    int output = -1;
    MPI_Allreduce(&input, &output, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    std::cout << "Result of allreduce on " << input << ": " << output
              << std::endl;
  }

  // Barrier before exit
  MPI_Barrier(MPI_COMM_WORLD);
}

// See example1.cc in this directory for a walkthrough of initialization.
void init(const std::string& path) {
  if (getenv("PREFIX") == nullptr || getenv("SIZE") == nullptr ||
      getenv("RANK") == nullptr) {
    std::cerr << "Please set environment variables PREFIX, SIZE, and RANK."
              << std::endl;
    exit(1);
  }

  const std::string prefix = getenv("PREFIX");
  const int rank = atoi(getenv("RANK"));
  const int size = atoi(getenv("SIZE"));

  // Initialize store
  auto fileStore = gloo::rendezvous::FileStore(path);
  auto prefixStore = gloo::rendezvous::PrefixStore(prefix, fileStore);

  // Initialize device
  gloo::transport::tcp::attr attr;
  attr.iface = "eth0";
  auto dev = gloo::transport::tcp::CreateDevice("localhost");

  // Initialize global context
  auto context = std::make_shared<gloo::rendezvous::Context>(rank, size);
  context->connectFullMesh(prefixStore, dev);
  kContext = std::move(context);
}

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "Usage: " << argv[0] << " PATH" << std::endl;
    exit(1);
  }
  init(argv[1]);
  return run();
}
