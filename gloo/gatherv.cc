/**
 * Copyright (c) 2019-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "gloo/gatherv.h"

#include <cstring>
#include <numeric>

#include "gloo/common/logging.h"
#include "gloo/types.h"

namespace gloo {

void GathervOptions::setElementSize(size_t elementSize_2) {
  if (this->elementSize == 0) {
    this->elementSize = elementSize_2;
  } else {
    GLOO_ENFORCE_EQ(
        elementSize_2,
        this->elementSize,
        "Element size does not match existing value. ",
        "Please double check that the input and output types match.");
  }
}

void GathervOptions::setInput(
    std::unique_ptr<transport::UnboundBuffer> buf,
    size_t elementSize_2) {
  this->setElementSize(elementSize_2);
  this->in = std::move(buf);
}

void GathervOptions::setInput(
    void* ptr,
    size_t elements,
    size_t elementSize_2) {
  this->setElementSize(elementSize_2);
  this->in = context->createUnboundBuffer(ptr, elements * elementSize_2);
}

void GathervOptions::setOutput(
    std::unique_ptr<transport::UnboundBuffer> buf,
    std::vector<size_t> elementsPerRank_2,
    size_t elementSize_2) {
  const auto totalElements = std::accumulate(
      elementsPerRank_2.begin(), elementsPerRank_2.end(), size_t(0));
  this->setElementSize(elementSize_2);
  GLOO_ENFORCE_EQ(elementsPerRank_2.size(), context->size);
  this->elementsPerRank = std::move(elementsPerRank_2);
  GLOO_ENFORCE_EQ(totalElements * elementSize_2, buf->size);
  this->out = std::move(buf);
}

void GathervOptions::setOutput(
    void* ptr,
    std::vector<size_t> elementsPerRank_2,
    size_t elementSize_2) {
  const auto totalElements = std::accumulate(
      elementsPerRank_2.begin(), elementsPerRank_2.end(), size_t(0));
  this->setElementSize(elementSize_2);
  GLOO_ENFORCE_EQ(elementsPerRank_2.size(), context->size);
  this->elementsPerRank = std::move(elementsPerRank_2);
  this->out = context->createUnboundBuffer(ptr, totalElements * elementSize_2);
}

void gatherv(GathervOptions& opts) {
  const auto& context = opts.context;
  transport::UnboundBuffer* in = opts.in.get();
  transport::UnboundBuffer* out = opts.out.get();
  const auto slot = Slot::build(kGatherSlotPrefix, opts.tag);

  // Sanity checks
  GLOO_ENFORCE(opts.elementSize > 0);
  GLOO_ENFORCE(in != nullptr);

  if (context->rank == opts.root) {
    size_t offset = 0;
    for (int i = 0; i < context->size; i++) {
      size_t copyLength = opts.elementSize * opts.elementsPerRank[i];
      if (i != context->rank) {
        // Remote memory copy
        out->recv(i, slot, offset, copyLength);
      } else {
        // Local memory copy
        GLOO_ENFORCE_EQ(copyLength, in->size);
        if (copyLength > 0) {
          memcpy(static_cast<char*>(out->ptr) + offset, in->ptr, in->size);
        }
      }
      offset += copyLength;
    }
    // Wait for receive operations to complete
    for (int i = 0; i < context->size - 1; i++) {
      out->waitRecv(opts.timeout);
    }
  } else {
    size_t sendLength = opts.elementSize * opts.elementsPerRank[context->rank];
    GLOO_ENFORCE_GE(in->size, sendLength);
    in->send(opts.root, slot, 0, sendLength);
    in->waitSend(opts.timeout);
  }
}

} // namespace gloo
