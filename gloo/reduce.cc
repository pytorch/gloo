/**
 * Copyright (c) 2018-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "gloo/reduce.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "gloo/common/enforce.h"
#include "gloo/math.h"
#include "gloo/types.h"

namespace gloo {

void reduce(ReduceOptions& opts) {
  if (opts.elements == 0) {
    return;
  }
  const auto& context = opts.context;
  transport::UnboundBuffer* in = opts.in.get();
  transport::UnboundBuffer* out = opts.out.get();
  const auto slot = Slot::build(kReduceSlotPrefix, opts.tag);

  // Sanity checks
  GLOO_ENFORCE(opts.elementSize > 0);
  GLOO_ENFORCE(opts.root >= 0 && opts.root < context->size);
  GLOO_ENFORCE(opts.reduce != nullptr);
  const auto recvRank = (context->size + context->rank + 1) % context->size;
  GLOO_ENFORCE(
      recvRank == context->rank || context->getPair(recvRank),
      "missing connection between rank " + std::to_string(context->rank) +
          " (this process) and rank " + std::to_string(recvRank));
  const auto sendRank = (context->size + context->rank - 1) % context->size;
  GLOO_ENFORCE(
      sendRank == context->rank || context->getPair(sendRank),
      "missing connection between rank " + std::to_string(context->rank) +
          " (this process) and rank " + std::to_string(sendRank));

  // If input buffer is not specified, the output is also the input
  if (in == nullptr) {
    in = out;
  }

  GLOO_ENFORCE_EQ(in->size, opts.elements * opts.elementSize);
  GLOO_ENFORCE_EQ(out->size, opts.elements * opts.elementSize);

  // Short circuit if there is only a single process.
  if (context->size == 1) {
    if (in != out) {
      memcpy(out->ptr, in->ptr, opts.elements * opts.elementSize);
    }
    return;
  }

  // The ring algorithm works as follows.
  //
  // The given input is split into a number of chunks equal to the
  // number of processes. Once the algorithm has finished, every
  // process hosts one chunk of reduced output, in sequential order
  // (rank 0 has chunk 0, rank 1 has chunk 1, etc.). As the input may
  // not be divisible by the number of processes, the chunk on the
  // final ranks may have partial output or may be empty.
  //
  // As a chunk is passed along the ring and contains the reduction of
  // successively more ranks, we have to alternate between performing
  // I/O for that chunk and computing the reduction between the
  // received chunk and the local chunk. To avoid this alternating
  // pattern, we split up a chunk into multiple segments (>= 2), and
  // ensure we have one segment in flight while computing a reduction
  // on the other. The segment size has an upper bound to minimize
  // memory usage and avoid poor cache behavior. This means we may
  // have many segments per chunk when dealing with very large inputs.
  //
  // The nomenclature here is reflected in the variable naming below
  // (one chunk per rank and many segments per chunk).
  //
  const size_t totalBytes = opts.elements * opts.elementSize;

  // Ensure that maximum segment size is a multiple of the element size.
  // Otherwise, the segment size can exceed the maximum segment size after
  // rounding it up to the nearest multiple of the element size.
  // For example, if maxSegmentSize = 10, and elementSize = 4,
  // then after rounding up: segmentSize = 12;
  const size_t maxSegmentSize =
      opts.elementSize * (opts.maxSegmentSize / opts.elementSize);

  // The number of bytes per segment must be a multiple of the bytes
  // per element for the reduction to work; round up if necessary.
  const size_t segmentBytes = roundUp(
      std::min(
          // Rounded division to have >= 2 segments per chunk.
          (totalBytes + (context->size * 2 - 1)) / (context->size * 2),
          // Configurable segment size limit
          maxSegmentSize),
      opts.elementSize);

  // Compute how many segments make up the input buffer.
  //
  // Round up to the nearest multiple of the context size such that
  // there is an equal number of segments per process and execution is
  // symmetric across processes.
  //
  // The minimum is twice the context size, because the algorithm
  // below overlaps sending/receiving a segment with computing the
  // reduction of the another segment.
  //
  const size_t numSegments = roundUp(
      std::max(
          (totalBytes + (segmentBytes - 1)) / segmentBytes,
          (size_t)context->size * 2),
      (size_t)context->size);
  GLOO_ENFORCE_EQ(numSegments % context->size, 0);
  GLOO_ENFORCE_GE(numSegments, context->size * 2);
  const size_t numSegmentsPerRank = numSegments / context->size;
  const size_t chunkBytes = numSegmentsPerRank * segmentBytes;

  // Allocate scratch space to hold two chunks
  std::unique_ptr<uint8_t[]> tmpAllocation(new uint8_t[segmentBytes * 2]);
  std::unique_ptr<transport::UnboundBuffer> tmpBuffer =
      context->createUnboundBuffer(tmpAllocation.get(), segmentBytes * 2);
  transport::UnboundBuffer* tmp = tmpBuffer.get();

  // Use dynamic lookup for chunk offset in the temporary buffer.
  // With two operations in flight we need two offsets.
  // They can be indexed using the loop counter.
  std::array<size_t, 2> segmentOffset;
  segmentOffset[0] = 0;
  segmentOffset[1] = segmentBytes;

  // Function computes the offsets and lengths of the chunks to be
  // sent and received for a given chunk iteration.
  auto computeReduceScatterOffsets = [&](size_t i) {
    struct {
      size_t sendOffset;
      size_t recvOffset;
      ssize_t sendLength;
      ssize_t recvLength;
    } result;

    // Compute segment index to send from (to rank - 1) and segment
    // index to receive into (from rank + 1). Multiply by the number
    // of bytes in a chunk to get to an offset. The offset is allowed
    // to be out of range (>= totalBytes) and this is taken into
    // account when computing the associated length.
    result.sendOffset =
        ((((context->rank + 1) * numSegmentsPerRank) + i) * segmentBytes) %
        (numSegments * segmentBytes);
    result.recvOffset =
        ((((context->rank + 2) * numSegmentsPerRank) + i) * segmentBytes) %
        (numSegments * segmentBytes);

    // If the segment is entirely in range, the following statement is
    // equal to segmentBytes. If it isn't, it will be less, or even
    // negative. This is why the ssize_t typecasts are needed.
    result.sendLength = std::min(
        (ssize_t)segmentBytes,
        (ssize_t)totalBytes - (ssize_t)result.sendOffset);
    result.recvLength = std::min(
        (ssize_t)segmentBytes,
        (ssize_t)totalBytes - (ssize_t)result.recvOffset);

    return result;
  };

  for (auto i = 0; i < numSegments; i++) {
    if (i >= 2) {
      // Compute send and receive offsets and lengths two iterations
      // ago. Needed so we know when to wait for an operation and when
      // to ignore (when the offset was out of bounds), and know where
      // to reduce the contents of the temporary buffer.
      auto prev = computeReduceScatterOffsets(i - 2);
      if (prev.recvLength > 0) {
        tmp->waitRecv(opts.timeout);
        opts.reduce(
            static_cast<uint8_t*>(out->ptr) + prev.recvOffset,
            static_cast<const uint8_t*>(in->ptr) + prev.recvOffset,
            static_cast<const uint8_t*>(tmp->ptr) + segmentOffset[i & 0x1],
            prev.recvLength / opts.elementSize);
      }
      if (prev.sendLength > 0) {
        if ((i - 2) < numSegmentsPerRank) {
          in->waitSend(opts.timeout);
        } else {
          out->waitSend(opts.timeout);
        }
      }
    }

    // Issue new send and receive operation in all but the final two
    // iterations. At that point we have already sent all data we
    // needed to and only have to wait for the final segments to be
    // reduced into the output.
    if (i < (numSegments - 2)) {
      // Compute send and receive offsets and lengths for this iteration.
      auto cur = computeReduceScatterOffsets(i);
      if (cur.recvLength > 0) {
        tmp->recv(recvRank, slot, segmentOffset[i & 0x1], cur.recvLength);
      }
      if (cur.sendLength > 0) {
        if (i < numSegmentsPerRank) {
          in->send(sendRank, slot, cur.sendOffset, cur.sendLength);
        } else {
          out->send(sendRank, slot, cur.sendOffset, cur.sendLength);
        }
      }
    }
  }

  // Gather to root rank.
  //
  // Beware: totalBytes <= (numSegments * segmentBytes), which is
  // incompatible with the generic gather algorithm where the
  // contribution is identical across processes.
  //
  if (context->rank == opts.root) {
    size_t numRecv = 0;
    for (size_t rank = 0; rank < context->size; rank++) {
      if (rank == context->rank) {
        continue;
      }
      size_t recvOffset = rank * numSegmentsPerRank * segmentBytes;
      ssize_t recvLength = std::min(
          (ssize_t)chunkBytes, (ssize_t)totalBytes - (ssize_t)recvOffset);
      if (recvLength > 0) {
        out->recv(rank, slot, recvOffset, recvLength);
        numRecv++;
      }
    }
    for (size_t i = 0; i < numRecv; i++) {
      out->waitRecv(opts.timeout);
    }
  } else {
    size_t sendOffset = context->rank * numSegmentsPerRank * segmentBytes;
    ssize_t sendLength = std::min(
        (ssize_t)chunkBytes, (ssize_t)totalBytes - (ssize_t)sendOffset);
    if (sendLength > 0) {
      out->send(opts.root, slot, sendOffset, sendLength);
      out->waitSend(opts.timeout);
    }
  }
}

} // namespace gloo
