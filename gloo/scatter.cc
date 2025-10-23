/**
 * Copyright (c) 2018-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "gloo/scatter.h"

#include <algorithm>
#include <cstring>

#include "gloo/common/enforce.h"
#include "gloo/types.h"

namespace gloo {

void scatter(ScatterOptions& opts) {
  const auto& context = opts.context;
  std::vector<std::unique_ptr<transport::UnboundBuffer>>& in = opts.in;
  std::unique_ptr<transport::UnboundBuffer>& out = opts.out;
  const auto slot = Slot::build(kScatterSlotPrefix, opts.tag);

  // Sanity checks
  GLOO_ENFORCE(opts.elementSize > 0);
  GLOO_ENFORCE(opts.root >= 0 && opts.root < context->size);
  GLOO_ENFORCE(out);
  if (context->rank == opts.root) {
    // Assert there are as many inputs as ranks to send to.
    GLOO_ENFORCE_EQ(in.size(), context->size);
    // Assert the size of all inputs is identical to the output.
    for (size_t i = 0; i < in.size(); i++) {
      GLOO_ENFORCE_EQ(in[i]->size, out->size);
    }
  }

  if (context->rank == opts.root) {
    // Post send operations to peers.
    for (size_t i = 0; i < context->size; i++) {
      if (i == context->rank) {
        continue;
      }
      in[i]->send(i, slot);
    }

    // Copy local input to output
    memcpy(out->ptr, in[context->rank]->ptr, out->size);

    // Wait for send operations to complete
    for (size_t i = 0; i < context->size; i++) {
      if (i == context->rank) {
        continue;
      }
      in[i]->waitSend(opts.timeout);
    }
  } else {
    out->recv(opts.root, slot);
    out->waitRecv(opts.timeout);
  }
}

} // namespace gloo
