/**
 * Copyright (c) 2018-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "gloo/broadcast.h"

#include <algorithm>
#include <cstring>

#include "gloo/common/enforce.h"
#include "gloo/math.h"
#include "gloo/types.h"

namespace gloo {

void broadcast(BroadcastOptions& opts) {
  const auto& context = opts.context;
  transport::UnboundBuffer* in = opts.in.get();
  transport::UnboundBuffer* out = opts.out.get();
  const auto slot = Slot::build(kBroadcastSlotPrefix, opts.tag);

  // Sanity checks
  GLOO_ENFORCE(opts.elementSize > 0);
  GLOO_ENFORCE(opts.root >= 0 && opts.root < context->size);
  GLOO_ENFORCE(out);
  if (context->rank == opts.root) {
    if (in) {
      GLOO_ENFORCE_EQ(in->size, out->size);
    } else {
      // Broadcast in place
      in = out;
    }
  } else {
    GLOO_ENFORCE(!in, "Non-root may not specify input");

    // Broadcast in place (for forwarding)
    in = out;
  }

  // Map rank to new rank where root process has rank 0.
  const size_t vsize = context->size;
  const size_t vrank = (context->rank + vsize - opts.root) % vsize;
  const size_t dim = log2ceil(vsize);

  // Track number of pending send operations.
  // Send operations can complete asynchronously because there is dependency
  // between iterations. This unlike recv operations that must complete
  // before any send operations can be queued.
  size_t numSends = 0;

  // Create mask with all 1's where we progressively set bits to 0
  // starting with the LSB. When the mask applied to the virtual rank
  // equals 0 we know the process must participate. This results in
  // exponential participation starting with virtual ranks 0 and 1.
  size_t mask = (1 << dim) - 1;

  for (size_t i = 0; i < dim; i++) {
    // Clear bit `i`. In the first iteration, virtual ranks 0 and 1 participate.
    // In the second iteration 0, 1, 2, and 3 participate, and so on.
    mask ^= (1 << i);
    if ((vrank & mask) != 0) {
      continue;
    }

    // The virtual rank of the peer in this iteration has opposite bit `i`.
    auto vpeer = vrank ^ (1 << i);
    if (vpeer >= vsize) {
      continue;
    }

    // Map virtual rank of peer to actual rank of peer.
    auto peer = (vpeer + opts.root) % vsize;
    if ((vrank & (1 << i)) == 0) {
      in->send(peer, slot);
      numSends++;
    } else {
      out->recv(peer, slot);
      out->waitRecv(opts.timeout);
    }
  }

  // Copy local input to output if applicable.
  if (context->rank == opts.root && in != out) {
    memcpy(out->ptr, in->ptr, out->size);
  }

  // Wait on pending sends.
  for (auto i = 0; i < numSends; i++) {
    in->waitSend(opts.timeout);
  }
}

} // namespace gloo
