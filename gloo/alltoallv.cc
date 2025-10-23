/**
 * Copyright (c) 2018-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "gloo/alltoallv.h"

#include <cstring>
#include <numeric>

#include "gloo/common/enforce.h"
#include "gloo/types.h"

namespace gloo {

static void splitOffsetsAndLengths(
    const std::vector<int64_t>& elementsPerRank,
    size_t elementSize,
    std::vector<size_t>& offsets,
    std::vector<size_t>& lengths) {
  size_t offset = 0;
  for (size_t elements : elementsPerRank) {
    size_t length = elements * elementSize;
    offsets.push_back(offset);
    lengths.push_back(length);
    offset += length;
  }
}

void AlltoallvOptions::setElementSize(size_t elementSize_2) {
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

void AlltoallvOptions::setInput(
    std::unique_ptr<transport::UnboundBuffer> buf,
    std::vector<int64_t> elementsPerRank,
    size_t elementSize_2) {
  const auto totalElements = std::accumulate(
      elementsPerRank.begin(), elementsPerRank.end(), size_t(0));
  this->setElementSize(elementSize_2);
  GLOO_ENFORCE_EQ(elementsPerRank.size(), context->size);
  this->inOffsetPerRank.reserve(elementsPerRank.size());
  this->inLengthPerRank.reserve(elementsPerRank.size());
  splitOffsetsAndLengths(
      elementsPerRank,
      elementSize_2,
      this->inOffsetPerRank,
      this->inLengthPerRank);
  GLOO_ENFORCE_EQ(totalElements * elementSize_2, buf->size);
  this->in = std::move(buf);
}

void AlltoallvOptions::setInput(
    void* ptr,
    std::vector<int64_t> elementsPerRank,
    size_t elementSize_2) {
  const auto totalElements = std::accumulate(
      elementsPerRank.begin(), elementsPerRank.end(), size_t(0));
  this->setElementSize(elementSize_2);
  GLOO_ENFORCE_EQ(elementsPerRank.size(), context->size);
  this->inOffsetPerRank.reserve(elementsPerRank.size());
  this->inLengthPerRank.reserve(elementsPerRank.size());
  splitOffsetsAndLengths(
      elementsPerRank,
      elementSize_2,
      this->inOffsetPerRank,
      this->inLengthPerRank);
  this->in = context->createUnboundBuffer(ptr, totalElements * elementSize_2);
}

void AlltoallvOptions::setOutput(
    std::unique_ptr<transport::UnboundBuffer> buf,
    std::vector<int64_t> elementsPerRank,
    size_t elementSize_2) {
  const auto totalElements = std::accumulate(
      elementsPerRank.begin(), elementsPerRank.end(), size_t(0));
  this->setElementSize(elementSize_2);
  GLOO_ENFORCE_EQ(elementsPerRank.size(), context->size);
  this->outOffsetPerRank.reserve(elementsPerRank.size());
  this->outLengthPerRank.reserve(elementsPerRank.size());
  splitOffsetsAndLengths(
      elementsPerRank,
      elementSize_2,
      this->outOffsetPerRank,
      this->outLengthPerRank);
  GLOO_ENFORCE_EQ(totalElements * elementSize_2, buf->size);
  this->out = std::move(buf);
}

void AlltoallvOptions::setOutput(
    void* ptr,
    std::vector<int64_t> elementsPerRank,
    size_t elementSize_2) {
  const auto totalElements = std::accumulate(
      elementsPerRank.begin(), elementsPerRank.end(), size_t(0));
  this->setElementSize(elementSize_2);
  GLOO_ENFORCE_EQ(elementsPerRank.size(), context->size);
  this->outOffsetPerRank.reserve(elementsPerRank.size());
  this->outLengthPerRank.reserve(elementsPerRank.size());
  splitOffsetsAndLengths(
      elementsPerRank,
      elementSize_2,
      this->outOffsetPerRank,
      this->outLengthPerRank);
  this->out = context->createUnboundBuffer(ptr, totalElements * elementSize_2);
}

void alltoallv(AlltoallvOptions& opts) {
  const auto& context = opts.context;
  transport::UnboundBuffer* in = opts.in.get();
  transport::UnboundBuffer* out = opts.out.get();
  std::vector<size_t>& inOffsetPerRank = opts.inOffsetPerRank;
  std::vector<size_t>& inLengthPerRank = opts.inLengthPerRank;
  std::vector<size_t>& outOffsetPerRank = opts.outOffsetPerRank;
  std::vector<size_t>& outLengthPerRank = opts.outLengthPerRank;
  const auto slot = Slot::build(kAlltoallSlotPrefix, opts.tag);

  // Sanity checks.
  GLOO_ENFORCE(opts.elementSize > 0);
  GLOO_ENFORCE(in != nullptr);
  GLOO_ENFORCE(out != nullptr);

  int myRank = context->rank;
  int worldSize = context->size;

  // Local copy.
  GLOO_ENFORCE(inLengthPerRank[myRank] == outLengthPerRank[myRank]);
  size_t myInOffset = inOffsetPerRank[myRank];
  size_t myOutOffset = outOffsetPerRank[myRank];
  size_t myChunkSize = inLengthPerRank[myRank];
  memcpy(
      static_cast<char*>(out->ptr) + myOutOffset,
      static_cast<char*>(in->ptr) + myInOffset,
      myChunkSize);

  // Remote copy.
  for (int i = 1; i < worldSize; i++) {
    int sendRank = (myRank + i) % worldSize;
    int recvRank = (myRank + worldSize - i) % worldSize;
    in->send(
        sendRank, slot, inOffsetPerRank[sendRank], inLengthPerRank[sendRank]);
    out->recv(
        recvRank, slot, outOffsetPerRank[recvRank], outLengthPerRank[recvRank]);
  }

  for (int i = 1; i < worldSize; i++) {
    in->waitSend(opts.timeout);
    out->waitRecv(opts.timeout);
  }
}

} // namespace gloo
