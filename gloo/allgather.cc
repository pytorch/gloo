/**
 * Copyright (c) 2018-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "gloo/allgather.h"

#include <array>
#include <cstring>

#include "gloo/common/enforce.h"
#include "gloo/types.h"

namespace gloo {

void allgather(AllgatherOptions& opts) {
  const auto& context = opts.context;
  transport::UnboundBuffer* in = opts.in.get();
  transport::UnboundBuffer* out = opts.out.get();
  const auto slot = Slot::build(kAllgatherSlotPrefix, opts.tag);

  // Sanity checks
  GLOO_ENFORCE(opts.elementSize > 0);
  const auto recvRank = (context->size + context->rank - 1) % context->size;
  GLOO_ENFORCE(
      recvRank == context->rank || context->getPair(recvRank),
      "missing connection between rank " + std::to_string(context->rank) +
          " (this process) and rank " + std::to_string(recvRank));
  const auto sendRank = (context->size + context->rank + 1) % context->size;
  GLOO_ENFORCE(
      sendRank == context->rank || context->getPair(sendRank),
      "missing connection between rank " + std::to_string(context->rank) +
          " (this process) and rank " + std::to_string(sendRank));

  if (in != nullptr) {
    GLOO_ENFORCE_EQ(out->size, in->size * context->size);
  } else {
    GLOO_ENFORCE_EQ(out->size % context->size, 0);
  }

  const size_t inBytes = out->size / context->size;
  const size_t outBytes = out->size;

  // If the input buffer is specified, this is NOT an in place operation,
  // and the output buffer needs to be primed with the input.
  if (in != nullptr) {
    memcpy(
        static_cast<uint8_t*>(out->ptr) + context->rank * in->size,
        static_cast<uint8_t*>(in->ptr),
        in->size);
  }

  // Short circuit if there is only a single process or the output is empty.
  if (context->size == 1 || outBytes == 0) {
    return;
  }

  // The chunk size may not be divisible by 2; use dynamic lookup.
  std::array<size_t, 2> chunkSize;
  chunkSize[0] = inBytes / 2;
  chunkSize[1] = inBytes - chunkSize[0];
  std::array<size_t, 2> chunkOffset;
  chunkOffset[0] = 0;
  chunkOffset[1] = chunkSize[0];

  for (auto i = 0; i < (context->size - 1) * 2; i++) {
    const size_t sendSegment = context->size + context->rank - (i / 2);
    const size_t recvSegment = sendSegment - 1;
    size_t sendOffset =
        ((sendSegment * inBytes) + chunkOffset[i & 0x1]) % outBytes;
    size_t recvOffset =
        ((recvSegment * inBytes) + chunkOffset[i & 0x1]) % outBytes;
    size_t size = chunkSize[i & 0x1];
    if (i < 2) {
      out->send(sendRank, slot, sendOffset, size);
      out->recv(recvRank, slot, recvOffset, size);
      continue;
    }

    // Wait for pending operations to complete to synchronize with the
    // previous iteration. Because we kick off two operations before
    // getting here we always wait for the next-to-last operation.
    out->waitSend(opts.timeout);
    out->waitRecv(opts.timeout);
    out->send(sendRank, slot, sendOffset, size);
    out->recv(recvRank, slot, recvOffset, size);
  }

  // Wait for completes
  for (auto i = 0; i < 2; i++) {
    out->waitSend(opts.timeout);
    out->waitRecv(opts.timeout);
  }
}

} // namespace gloo
