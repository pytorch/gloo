/**
 * Copyright (c) 2018-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "gloo/gather.h"

#include <cstring>

#include "gloo/common/enforce.h"
#include "gloo/types.h"

namespace gloo {

void gather(GatherOptions& opts) {
  const auto& context = opts.context;
  transport::UnboundBuffer* in = opts.in.get();
  transport::UnboundBuffer* out = opts.out.get();
  const auto slot = Slot::build(kGatherSlotPrefix, opts.tag);

  // Sanity checks
  GLOO_ENFORCE(opts.elementSize > 0);
  GLOO_ENFORCE(in != nullptr);

  if (context->rank == opts.root) {
    const size_t chunkSize = in->size;

    // Ensure the output buffer has the right size.
    GLOO_ENFORCE(out != nullptr);
    GLOO_ENFORCE(in->size * context->size == out->size);

    // Post receive operations from peers into out buffer
    for (size_t i = 0; i < context->size; i++) {
      if (i == context->rank) {
        continue;
      }
      out->recv(i, slot, i * chunkSize, chunkSize);
    }

    // Copy local input to output
    memcpy(
        static_cast<char*>(out->ptr) + (context->rank * chunkSize),
        in->ptr,
        chunkSize);

    // Wait for receive operations to complete
    for (size_t i = 0; i < context->size; i++) {
      if (i == context->rank) {
        continue;
      }
      out->waitRecv(opts.timeout);
    }
  } else {
    in->send(opts.root, slot);
    in->waitSend(opts.timeout);
  }
}

} // namespace gloo
