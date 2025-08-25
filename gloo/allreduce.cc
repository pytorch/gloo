/**
 * Copyright (c) 2018-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "gloo/allreduce.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "gloo/common/logging.h"
#include "gloo/math.h"
#include "gloo/types.h"
#include "gloo/allreduce_shm.h"

namespace gloo {

namespace {

using BufferVector = std::vector<std::unique_ptr<transport::UnboundBuffer>>;
using ReductionFunction = AllreduceOptions::Func;
using ReduceRangeFunction = std::function<void(size_t, size_t)>;
using BroadcastRangeFunction = std::function<void(size_t, size_t)>;

// Forward declaration of ring algorithm implementation.
void ring(
    const detail::AllreduceOptionsImpl& opts,
    ReduceRangeFunction reduceInputs,
    BroadcastRangeFunction broadcastOutputs);

// Forward declaration of bcube algorithm implementation.
void bcube(
    const detail::AllreduceOptionsImpl& opts,
    ReduceRangeFunction reduceInputs,
    BroadcastRangeFunction broadcastOutputs);

// Returns function that computes local reduction over inputs and
// stores it in the output for a given range in those buffers.
// This is done prior to either sending a region to a neighbor, or
// reducing a region received from a neighbor.
ReduceRangeFunction genLocalReduceFunction(
    const BufferVector& in,
    const BufferVector& out,
    size_t elementSize,
    ReductionFunction fn) {
  if (in.size() > 0) {
    if (in.size() == 1) {
      return [&in, &out](size_t offset, size_t length) {
        memcpy(
            static_cast<uint8_t*>(out[0]->ptr) + offset,
            static_cast<const uint8_t*>(in[0]->ptr) + offset,
            length);
      };
    } else {
      return [&in, &out, elementSize, fn](size_t offset, size_t length) {
        fn(static_cast<uint8_t*>(out[0]->ptr) + offset,
           static_cast<const uint8_t*>(in[0]->ptr) + offset,
           static_cast<const uint8_t*>(in[1]->ptr) + offset,
           length / elementSize);
        for (size_t i = 2; i < in.size(); i++) {
          fn(static_cast<uint8_t*>(out[0]->ptr) + offset,
             static_cast<const uint8_t*>(out[0]->ptr) + offset,
             static_cast<const uint8_t*>(in[i]->ptr) + offset,
             length / elementSize);
        }
      };
    }
  } else {
    return [&out, elementSize, fn](size_t offset, size_t length) {
      for (size_t i = 1; i < out.size(); i++) {
        fn(static_cast<uint8_t*>(out[0]->ptr) + offset,
           static_cast<const uint8_t*>(out[0]->ptr) + offset,
           static_cast<const uint8_t*>(out[i]->ptr) + offset,
           length / elementSize);
      }
    };
  }
}

// Returns function that performs a local broadcast over outputs for a
// given range in the buffers. This is executed after receiving every
// globally reduced chunk.
BroadcastRangeFunction genLocalBroadcastFunction(const BufferVector& out) {
  return [&out](size_t offset, size_t length) {
    for (size_t i = 1; i < out.size(); i++) {
      memcpy(
          static_cast<uint8_t*>(out[i]->ptr) + offset,
          static_cast<const uint8_t*>(out[0]->ptr) + offset,
          length);
    }
  };
}

void allreduce(const detail::AllreduceOptionsImpl& opts) {
  if (opts.elements == 0) {
    return;
  }

  const auto& context = opts.context;
  const std::vector<std::unique_ptr<transport::UnboundBuffer>>& in = opts.in;
  const std::vector<std::unique_ptr<transport::UnboundBuffer>>& out = opts.out;

  // Sanity checks
  GLOO_ENFORCE_GT(out.size(), 0);
  GLOO_ENFORCE(opts.elementSize > 0);
  GLOO_ENFORCE(opts.reduce != nullptr);

  // Assert the size of all inputs and outputs is identical.
  const size_t totalBytes = opts.elements * opts.elementSize;
  for (size_t i = 0; i < out.size(); i++) {
    GLOO_ENFORCE_EQ(out[i]->size, totalBytes);
  }
  for (size_t i = 0; i < in.size(); i++) {
    GLOO_ENFORCE_EQ(in[i]->size, totalBytes);
  }

  // Initialize local reduction and broadcast functions.
  // Note that these are a no-op if only a single output is specified
  // and is used as both input and output.
  const auto reduceInputs =
      genLocalReduceFunction(in, out, opts.elementSize, opts.reduce);
  const auto broadcastOutputs = genLocalBroadcastFunction(out);

  // Simple circuit if there is only a single process.
  if (context->size == 1) {
    reduceInputs(0, totalBytes);
    broadcastOutputs(0, totalBytes);
    return;
  }

  auto algorithm = opts.algorithm;
  if (is_intra_node(opts)) {
    algorithm = detail::AllreduceOptionsImpl::SHM;
  }


  switch (algorithm) {
    case detail::AllreduceOptionsImpl::UNSPECIFIED:
    case detail::AllreduceOptionsImpl::RING:
      ring(opts, reduceInputs, broadcastOutputs);
      break;
    case detail::AllreduceOptionsImpl::BCUBE:
      bcube(opts, reduceInputs, broadcastOutputs);
      break;
    case detail::AllreduceOptionsImpl::SHM:
      shm(opts);
      break;
    default:
      GLOO_ENFORCE(false, "Algorithm not handled.");
  }
}

void ring(
    const detail::AllreduceOptionsImpl& opts,
    ReduceRangeFunction reduceInputs,
    BroadcastRangeFunction broadcastOutputs) {
  const auto& context = opts.context;
  const std::vector<std::unique_ptr<transport::UnboundBuffer>>& out = opts.out;
  const auto slot = Slot::build(kAllreduceSlotPrefix, opts.tag);
  const size_t totalBytes = opts.elements * opts.elementSize;


  // Note: context->size > 1
  const auto recvRank = (context->size + context->rank + 1) % context->size;
  const auto sendRank = (context->size + context->rank - 1) % context->size;
  GLOO_ENFORCE(
      context->getPair(recvRank),
      "missing connection between rank " + std::to_string(context->rank) +
          " (this process) and rank " + std::to_string(recvRank));
  GLOO_ENFORCE(
      context->getPair(sendRank),
      "missing connection between rank " + std::to_string(context->rank) +
          " (this process) and rank " + std::to_string(sendRank));

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

  // Ensure that maximum segment size is a multiple of the element size.
  // Otherwise, the segment size can exceed the maximum segment size after
  // rounding it up to the nearest multiple of the element size.
  // For example, if maxSegmentSize = 10, and elementSize = 4,
  // then after rounding up: segmentSize = 12;
  const size_t maxSegmentBytes = opts.elementSize *
      std::max((size_t)1, opts.maxSegmentSize / opts.elementSize);

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
          (totalBytes + (maxSegmentBytes - 1)) / maxSegmentBytes,
          (size_t)context->size * 2),
      (size_t)context->size);
  GLOO_ENFORCE_EQ(numSegments % context->size, 0);
  GLOO_ENFORCE_GE(numSegments, context->size * 2);
  const size_t numSegmentsPerRank = numSegments / context->size;
  const size_t segmentBytes =
      roundUp((totalBytes + numSegments - 1) / numSegments, opts.elementSize);

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

  // Function computes the offsets and lengths of the segments to be
  // sent and received for a given iteration during reduce/scatter.
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

  // Ring reduce/scatter.
  //
  // Number of iterations is computed as follows:
  // - Take `numSegments` for the total number of segments,
  // - Subtract `numSegmentsPerRank` because the final segments hold
  //   the partial result and must not be forwarded in this phase.
  // - Add 2 because we pipeline send and receive operations (we issue
  //   send/recv operations on iterations 0 and 1 and wait for them to
  //   complete on iterations 2 and 3).
  //
  for (auto i = 0; i < (numSegments - numSegmentsPerRank + 2); i++) {
    if (i >= 2) {
      // Compute send and receive offsets and lengths two iterations
      // ago. Needed so we know when to wait for an operation and when
      // to ignore (when the offset was out of bounds), and know where
      // to reduce the contents of the temporary buffer.
      auto prev = computeReduceScatterOffsets(i - 2);
      if (prev.recvLength > 0) {
        // Prepare out[0]->ptr to hold the local reduction
        reduceInputs(prev.recvOffset, prev.recvLength);
        // Wait for segment from neighbor.
        tmp->waitRecv(opts.timeout);
        // Reduce segment from neighbor into out->ptr.
        opts.reduce(
            static_cast<uint8_t*>(out[0]->ptr) + prev.recvOffset,
            static_cast<const uint8_t*>(out[0]->ptr) + prev.recvOffset,
            static_cast<const uint8_t*>(tmp->ptr) + segmentOffset[i & 0x1],
            prev.recvLength / opts.elementSize);
      }
      if (prev.sendLength > 0) {
        out[0]->waitSend(opts.timeout);
      }
    }

    // Issue new send and receive operation in all but the final two
    // iterations. At that point we have already sent all data we
    // needed to and only have to wait for the final segments to be
    // reduced into the output.
    if (i < (numSegments - numSegmentsPerRank)) {
      // Compute send and receive offsets and lengths for this iteration.
      auto cur = computeReduceScatterOffsets(i);
      if (cur.recvLength > 0) {
        tmp->recv(recvRank, slot, segmentOffset[i & 0x1], cur.recvLength);
      }
      if (cur.sendLength > 0) {
        // Prepare out[0]->ptr to hold the local reduction for this segment
        if (i < numSegmentsPerRank) {
          reduceInputs(cur.sendOffset, cur.sendLength);
        }
        out[0]->send(sendRank, slot, cur.sendOffset, cur.sendLength);
      }
    }
  }

  // Function computes the offsets and lengths of the segments to be
  // sent and received for a given iteration during allgather.
  auto computeAllgatherOffsets = [&](size_t i) {
    struct {
      size_t sendOffset;
      size_t recvOffset;
      ssize_t sendLength;
      ssize_t recvLength;
    } result;

    result.sendOffset =
        ((((context->rank) * numSegmentsPerRank) + i) * segmentBytes) %
        (numSegments * segmentBytes);
    result.recvOffset =
        ((((context->rank + 1) * numSegmentsPerRank) + i) * segmentBytes) %
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

  // Ring allgather.
  //
  // Beware: totalBytes <= (numSegments * segmentBytes), which is
  // incompatible with the generic allgather algorithm where the
  // contribution is identical across processes.
  //
  // See comment prior to reduce/scatter loop on how the number of
  // iterations for this loop is computed.
  //
  for (auto i = 0; i < (numSegments - numSegmentsPerRank + 2); i++) {
    if (i >= 2) {
      auto prev = computeAllgatherOffsets(i - 2);
      if (prev.recvLength > 0) {
        out[0]->waitRecv(opts.timeout);
        // Broadcast received segments to output buffers.
        broadcastOutputs(prev.recvOffset, prev.recvLength);
      }
      if (prev.sendLength > 0) {
        out[0]->waitSend(opts.timeout);
      }
    }

    // Issue new send and receive operation in all but the final two
    // iterations. At that point we have already sent all data we
    // needed to and only have to wait for the final segments to be
    // sent to the output.
    if (i < (numSegments - numSegmentsPerRank)) {
      auto cur = computeAllgatherOffsets(i);
      if (cur.recvLength > 0) {
        out[0]->recv(recvRank, slot, cur.recvOffset, cur.recvLength);
      }
      if (cur.sendLength > 0) {
        out[0]->send(sendRank, slot, cur.sendOffset, cur.sendLength);
        // Broadcast first segments to outputs buffers.
        if (i < numSegmentsPerRank) {
          broadcastOutputs(cur.sendOffset, cur.sendLength);
        }
      }
    }
  }
}

// For a given context size and desired group size, compute the actual group
// size per step. Note that the group size per step is n for all steps, only
// if n^(#steps) == size. Otherwise, the final group size is != n.
std::vector<size_t> computeGroupSizePerStep(size_t size, const size_t n) {
  std::vector<size_t> result;
  GLOO_ENFORCE_GT(n, 1);
  while (size % n == 0) {
    result.push_back(n);
    size /= n;
  }
  if (size > 1) {
    result.push_back(size);
  }
  return result;
}

// The bcube algorithm implements a hypercube-like strategy for reduction. The
// constraint is that the number of processes can be factorized. If the minimum
// component in the factorization is 2, and the number of processes is equal to
// a power of 2, the algorithm is identical to recursive halving/doubling. The
// number of elements in the factorization determines the number of steps of the
// algorithm. Each element of the factorization determines the number of
// processes each process communicates with at that particular step of the
// algorithm. If the number of processes is not factorizable, the algorithm is
// identical to a direct reduce-scatter followed by allgather.
//
// For example, if #processes == 8, and we factorize as 4 * 2, the algorithm
// runs in 2 steps. In the first step, 2 groups of 4 processes exchange data
// such that all processes have 1/4th of the partial result (with process 0
// having the first quarter, 1 having the second quarter, and so forth). In the
// second step, 4 groups of 2 processes exchange their partial result such that
// all processes have 1/8th of the result. Then, the same factorization is
// followed in reverse to perform an allgather.
//
void bcube(
    const detail::AllreduceOptionsImpl& opts,
    ReduceRangeFunction reduceInputs,
    BroadcastRangeFunction broadcastOutputs) {
  const auto& context = opts.context;
  const auto slot = Slot::build(kAllreduceSlotPrefix, opts.tag);
  const auto elementSize = opts.elementSize;
  auto& out = opts.out[0];

  constexpr auto n = 2;

  // Figure out the number of steps in this algorithm.
  const auto groupSizePerStep = computeGroupSizePerStep(context->size, n);

  struct group {
    // Distance between peers in this group.
    size_t peerDistance;

    // Segment that this group is responsible for reducing.
    size_t bufferOffset;
    size_t bufferLength;

    // The process ranks that are a member of this group.
    std::vector<size_t> ranks;

    // Upper bound of the length of the chunk that each process has the
    // reduced values for by the end of the reduction for this group.
    size_t chunkLength;

    // Chunk within the segment that this process is responsible for reducing.
    size_t myChunkOffset;
    size_t myChunkLength;
  };

  // Compute the details of a group at every algorithm step.
  // We keep this in a vector because we iterate through it in forward order in
  // the reduce/scatter phase and in backward order in the allgather phase.
  std::vector<struct group> groups;
  {
    struct group group;
    group.peerDistance = 1;
    group.bufferOffset = 0;
    group.bufferLength = opts.elements;
    for (const size_t groupSize : groupSizePerStep) {
      const size_t groupRank = (context->rank / group.peerDistance) % groupSize;
      const size_t baseRank = context->rank - (groupRank * group.peerDistance);
      group.ranks.reserve(groupSize);
      for (size_t i = 0; i < groupSize; i++) {
        group.ranks.push_back(baseRank + i * group.peerDistance);
      }

      // Compute the length of the chunk we're exchanging at this step.
      group.chunkLength = ((group.bufferLength + (groupSize - 1)) / groupSize);

      // This process is computing the reduction of the chunk positioned at
      // <rank>/<size> within the current segment.
      group.myChunkOffset =
          group.bufferOffset + (groupRank * group.chunkLength);
      group.myChunkLength = std::min(
          size_t(group.chunkLength),
          size_t(std::max(
              int64_t(0),
              int64_t(group.bufferLength) -
                  int64_t(groupRank * group.chunkLength))));

      // Store a const copy of this group in the vector.
      groups.push_back(group);

      // Initialize with updated peer distance and segment offset and length.
      struct group nextGroup;
      nextGroup.peerDistance = group.peerDistance * groupSize;
      nextGroup.bufferOffset = group.myChunkOffset;
      nextGroup.bufferLength = group.myChunkLength;
      std::swap(group, nextGroup);
    }
  }

  // The chunk length is rounded up, so the maximum scratch space we need
  // might be larger than the size of the output buffer. Compute the maximum
  size_t bufferLength = opts.elements;
  for (const auto& group : groups) {
    bufferLength =
        std::max(bufferLength, group.ranks.size() * group.chunkLength);
  }

  // Allocate scratch space to receive data from peers.
  const size_t bufferSize = bufferLength * elementSize;
  std::unique_ptr<uint8_t[]> buffer(new uint8_t[bufferSize]);
  std::unique_ptr<transport::UnboundBuffer> tmp =
      context->createUnboundBuffer(buffer.get(), bufferSize);

  // Reduce/scatter.
  for (size_t step = 0; step < groups.size(); step++) {
    const auto& group = groups[step];

    // Issue receive operations for chunks from peers.
    for (size_t i = 0; i < group.ranks.size(); i++) {
      const auto src = group.ranks[i];
      if (src == context->rank) {
        continue;
      }
      tmp->recv(
          src,
          slot,
          i * group.chunkLength * elementSize,
          group.myChunkLength * elementSize);
    }

    // Issue send operations for local chunks to peers.
    for (size_t i = 0; i < group.ranks.size(); i++) {
      const auto dst = group.ranks[i];
      if (dst == context->rank) {
        continue;
      }
      const size_t currentChunkOffset =
          group.bufferOffset + i * group.chunkLength;
      const size_t currentChunkLength = std::min(
          size_t(group.chunkLength),
          size_t(std::max(
              int64_t(0),
              int64_t(group.bufferLength) - int64_t(i * group.chunkLength))));
      // Compute the local reduction only in the first step of the algorithm.
      // In subsequent steps, we already have a partially reduced result.
      if (step == 0) {
        reduceInputs(
            currentChunkOffset * elementSize, currentChunkLength * elementSize);
      }
      out->send(
          dst,
          slot,
          currentChunkOffset * elementSize,
          currentChunkLength * elementSize);
    }

    // Wait for send and receive operations to complete.
    for (size_t i = 0; i < group.ranks.size(); i++) {
      const auto peer = group.ranks[i];
      if (peer == context->rank) {
        continue;
      }
      tmp->waitRecv();
      out->waitSend();
    }

    // In the first step, prepare the chunk this process is responsible for
    // with the reduced version of its inputs (if multiple are specified).
    if (step == 0) {
      reduceInputs(
          group.myChunkOffset * elementSize, group.myChunkLength * elementSize);
    }

    // Reduce chunks from peers.
    for (size_t i = 0; i < group.ranks.size(); i++) {
      const auto src = group.ranks[i];
      if (src == context->rank) {
        continue;
      }
      opts.reduce(
          static_cast<uint8_t*>(out->ptr) + (group.myChunkOffset * elementSize),
          static_cast<const uint8_t*>(out->ptr) +
              (group.myChunkOffset * elementSize),
          static_cast<const uint8_t*>(tmp->ptr) +
              (i * group.chunkLength * elementSize),
          group.myChunkLength);
    }
  }

  // There is one chunk that contains the final result and this chunk
  // can already be broadcast locally to out[1..N], if applicable.
  // Doing so means we only have to broadcast locally to out[1..N] all
  // chunks as we receive them from our peers during the allgather phase.
  {
    const auto& group = groups.back();
    broadcastOutputs(
        group.myChunkOffset * elementSize, group.myChunkLength * elementSize);
  }

  // Allgather.
  for (auto it = groups.rbegin(); it != groups.rend(); it++) {
    const auto& group = *it;

    // Issue receive operations for reduced chunks from peers.
    for (size_t i = 0; i < group.ranks.size(); i++) {
      const auto src = group.ranks[i];
      if (src == context->rank) {
        continue;
      }
      const size_t currentChunkOffset =
          group.bufferOffset + i * group.chunkLength;
      const size_t currentChunkLength = std::min(
          size_t(group.chunkLength),
          size_t(std::max(
              int64_t(0),
              int64_t(group.bufferLength) - int64_t(i * group.chunkLength))));
      out->recv(
          src,
          slot,
          currentChunkOffset * elementSize,
          currentChunkLength * elementSize);
    }

    // Issue send operations for reduced chunk to peers.
    for (size_t i = 0; i < group.ranks.size(); i++) {
      const auto dst = group.ranks[i];
      if (dst == context->rank) {
        continue;
      }
      out->send(
          dst,
          slot,
          group.myChunkOffset * elementSize,
          group.myChunkLength * elementSize);
    }

    // Wait for operations to complete.
    for (size_t i = 0; i < group.ranks.size(); i++) {
      const auto peer = group.ranks[i];
      if (peer == context->rank) {
        continue;
      }
      out->waitRecv();
      out->waitSend();
    }

    // Broadcast result to multiple output buffers, if applicable.
    for (size_t i = 0; i < group.ranks.size(); i++) {
      const auto peer = group.ranks[i];
      if (peer == context->rank) {
        continue;
      }
      const size_t currentChunkOffset =
          group.bufferOffset + i * group.chunkLength;
      const size_t currentChunkLength = std::min(
          size_t(group.chunkLength),
          size_t(std::max(
              int64_t(0),
              int64_t(group.bufferLength) - int64_t(i * group.chunkLength))));
      broadcastOutputs(
          currentChunkOffset * elementSize, currentChunkLength * elementSize);
    }
  }
}

} // namespace

void allreduce(const AllreduceOptions& opts) {
  allreduce(opts.impl_);
}

} // namespace gloo
