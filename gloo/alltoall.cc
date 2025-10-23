/**
 * Copyright (c) 2018-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "gloo/alltoall.h"

#include <cstring>

#include "gloo/common/enforce.h"
#include "gloo/types.h"

namespace gloo {

void alltoall(AlltoallOptions& opts) {
  const auto& context = opts.context;
  transport::UnboundBuffer* in = opts.in.get();
  transport::UnboundBuffer* out = opts.out.get();
  const auto slot = Slot::build(kAlltoallSlotPrefix, opts.tag);

  // Sanity checks.
  // Number of elements should be evenly split in input and output buffers.
  GLOO_ENFORCE(opts.elementSize > 0);
  GLOO_ENFORCE(in != nullptr);
  GLOO_ENFORCE(out != nullptr);
  GLOO_ENFORCE(in->size % context->size == 0);
  GLOO_ENFORCE(in->size == out->size);

  size_t chunkSize = in->size / context->size;
  int myRank = context->rank;
  int worldSize = context->size;

  // Local copy.
  memcpy(
      static_cast<char*>(out->ptr) + myRank * chunkSize,
      static_cast<char*>(in->ptr) + myRank * chunkSize,
      chunkSize);

  // Remote copy.
  for (int i = 1; i < worldSize; i++) {
    int sendRank = (myRank + i) % worldSize;
    int recvRank = (myRank + worldSize - i) % worldSize;
    in->send(sendRank, slot, sendRank * chunkSize, chunkSize);
    out->recv(recvRank, slot, recvRank * chunkSize, chunkSize);
  }

  for (int i = 1; i < worldSize; i++) {
    in->waitSend(opts.timeout);
    out->waitRecv(opts.timeout);
  }
}

} // namespace gloo
