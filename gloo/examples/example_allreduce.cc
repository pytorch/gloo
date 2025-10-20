/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 */

#include <array>
#include <iostream>
#include <memory>
#include <typeinfo>

#include "gloo/allreduce.h"
#include "gloo/allreduce_ring.h"
#include "gloo/rendezvous/context.h"
#include "gloo/rendezvous/file_store.h"
#include "gloo/rendezvous/prefix_store.h"
#include "gloo/transport/uv/device.h"

// Usage:
//
// Open two terminals. Run the same program in both terminals, using
// a different RANK in each. For example:
//
// A: PREFIX=test1 SIZE=2 RANK=0 example_allreduce
// B: PREFIX=test1 SIZE=2 RANK=1 example_allreduce
//
// Expected output:
//
//   data[0] = 18
//   data[1] = 18
//   data[2] = 18
//   data[3] = 18
//

void mysum(void* c_, const void* a_, const void* b_, int n) {
  printf("n=%d\r\n", n);
  int* c = static_cast<int*>(c_);
  const int* a = static_cast<const int*>(a_);
  const int* b = static_cast<const int*>(b_);
  for (auto i = 0; i < n; i++) {
    printf("a[%d]=%d\r\n", i, a[i]);
    printf("b[%d]=%d\r\n", i, b[i]);
    c[i] = a[i] + b[i];
    printf("c[%d]=%d\r\n", i, c[i]);
  }
}

int main(void) {
  if (getenv("PREFIX") == nullptr || getenv("SIZE") == nullptr ||
      getenv("RANK") == nullptr) {
    std::cerr << "Please set environment variables PREFIX, SIZE, and RANK."
              << std::endl;
    return 1;
  }

  // The following statement creates a TCP "device" for Gloo to use.
  // See "gloo/transport/device.h" for more information. For the
  // purposes of this example, it is sufficient to see the device as
  // a factory for every communication pair.
  //
  // The argument to gloo::transport::tcp::CreateDevice is used to
  // find the network interface to bind connection to. The attr struct
  // can be populated to specify exactly which interface should be
  // used, as shown below. This is useful if you have identical
  // multi-homed machines that all share the same network interface
  // name, for example.
  //
  gloo::transport::uv::attr attr;
  // attr.iface = "eth0";
  // attr.iface = "ib0";
  // attr.iface = "Wi-Fi";

  // attr.ai_family = AF_INET; // Force IPv4
  // attr.ai_family = AF_INET6; // Force IPv6
  attr.ai_family = AF_UNSPEC; // Use either (default)

  // A string is implicitly converted to an "attr" struct with its
  // hostname field populated. This will try to resolve the interface
  // to use by resolving the hostname or IP address, and finding the
  // corresponding network interface.
  //
  // Hostname "localhost" should resolve to 127.0.0.1, so using this
  // implies that all connections will be local. This can be useful
  // for single machine operation.
  //
  //   auto dev = gloo::transport::tcp::CreateDevice("localhost");
  //

  auto dev = gloo::transport::uv::CreateDevice(attr);

  // Now that we have a device, we can connect all participating
  // processes. We call this process "rendezvous". It can be performed
  // using a shared filesystem, a Redis instance, or something else by
  // extending it yourself.
  //
  // See "gloo/rendezvous/store.h" for the functionality you need to
  // implement to create your own store for performing rendezvous.
  //
  // Below, we instantiate rendezvous using the filesystem, given that
  // this example uses multiple processes on a single machine.
  //
  auto fileStore = gloo::rendezvous::FileStore("/libtmp");

  // To be able to reuse the same store over and over again and not have
  // interference between runs, we scope it to a unique prefix with the
  // PrefixStore. This wraps another store and prefixes every key before
  // forwarding the call to the underlying store.
  std::string prefix = getenv("PREFIX");
  auto prefixStore = gloo::rendezvous::PrefixStore(prefix, fileStore);

  // Using this store, we can now create a Gloo context. The context
  // holds a reference to every communication pair involving this
  // process. It is used by every collective algorithm to find the
  // current process's rank in the collective, the collective size,
  // and setup of send/receive buffer pairs.
  const int rank = atoi(getenv("RANK"));
  const int size = atoi(getenv("SIZE"));
  auto context = std::make_shared<gloo::rendezvous::Context>(rank, size);
  context->connectFullMesh(prefixStore, dev);

  // All connections are now established. We can now initialize some
  // test data, instantiate the collective algorithm, and run it.
  size_t elements = 4;
  std::vector<int*> inputPointers;
  std::vector<int*> outputPointers;
  for (size_t i = 0; i < elements; i++) {
    int* value = reinterpret_cast<int*>(malloc(sizeof(int)));
    *value = i * (rank + 1);
    inputPointers.push_back(value);
    int* value1 = reinterpret_cast<int*>(malloc(sizeof(int)));
    *value1 = 0;
    outputPointers.push_back(value1);
  }

  // Configure AllreduceOptions struct
  gloo::AllreduceOptions opts_(context);
  opts_.setInputs(inputPointers, 1);
  opts_.setOutputs(outputPointers, 1);
  opts_.setAlgorithm(gloo::AllreduceOptions::Algorithm::RING);
  void (*fn)(void*, const void*, const void*, int) = &mysum;
  opts_.setReduceFunction(fn);
  gloo::allreduce(opts_);

  // Print the result.
  std::cout << "Output: " << std::endl;
  for (int i = 0; i < outputPointers.size(); i++) {
    std::cout << "data[" << i << "] = " << *outputPointers[i] << std::endl;
  }

  return 0;
}
