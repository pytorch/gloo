/**
 * Copyright (c) 2018-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <math.h>
#include <stddef.h>
#include <string.h>

#include "gloo/algorithm.h"
#include "gloo/common/error.h"
#include "gloo/context.h"

namespace gloo {

template <typename T>
class ReduceScatterHalvingDoubling : public Algorithm {
  void initBinaryBlocks() {
    uint32_t offset = this->contextSize_;
    uint32_t blockSize = 1;
    uint32_t currentBlockSize = 0;
    uint32_t prevBlockSize = 0;
    do {
      if (this->contextSize_ & blockSize) {
        prevBlockSize = currentBlockSize;
        currentBlockSize = blockSize;
        offset -= blockSize;
        if (myBinaryBlockSize_ != 0) {
          nextLargerBlockSize_ = currentBlockSize;
          break;
        }
        if (offset <= this->context_->rank) {
          offsetToMyBinaryBlock_ = offset;
          myBinaryBlockSize_ = currentBlockSize;
          nextSmallerBlockSize_ = prevBlockSize;
        }
      }
      blockSize <<= 1;
    } while (offset != 0);

    stepsWithinBlock_ = log2(myBinaryBlockSize_);
    rankInBinaryBlock_ = this->context_->rank % myBinaryBlockSize_;
  }

  // returns the last n bits of ctr reversed
  uint32_t reverseLastNBits(uint32_t ctr, uint32_t n) {
    uint32_t bitMask = 1;
    uint32_t reversed = 0;
    while (bitMask < (static_cast<uint32_t>(1) << n)) {
      reversed <<= 1;
      if (ctr & bitMask) {
        reversed |= 1;
      }
      bitMask <<= 1;
    }
    return reversed;
  }

  struct DistributionMap {
    int rank;
    size_t offset;
    size_t itemCount;
    DistributionMap(int dRank, size_t dOffset, size_t dItemCount)
        : rank(dRank), offset(dOffset), itemCount(dItemCount) {}
  };

  void getDistributionMap(
      size_t srcOffset,
      size_t srcCount,
      const std::vector<int>& recvCounts,
      bool reorder,
      std::vector<DistributionMap>& distributionMap) {
    if (srcCount == 0) {
      return;
    }

    size_t destOffset = 0;
    auto size =
        reorder ? 1 << (int)log2(this->contextSize_) : this->contextSize_;
    int start = 0;
    for (; start < size; ++start) {
      if (destOffset + recvCounts[start] > srcOffset)
        break;
      destOffset += recvCounts[start];
    }
    destOffset = srcOffset - destOffset;

    auto totalCount = srcCount;
    for (int i = start; i < size; ++i) {
      auto recvCount = recvCounts[i];
      if (destOffset != 0) {
        recvCount -= destOffset;
        destOffset = 0;
      }
      auto rank = reorder ? reverseLastNBits(i, log2(this->contextSize_)) : i;
      recvCount = recvCount < totalCount ? recvCount : totalCount;
      distributionMap.emplace_back(rank, srcOffset, recvCount);
      srcOffset += recvCount;
      totalCount -= recvCount;
      if (totalCount <= 0) {
        break;
      }
    }
  }

 public:
  ReduceScatterHalvingDoubling(
      const std::shared_ptr<Context>& context,
      const std::vector<T*> ptrs,
      const int count,
      const std::vector<int> recvElems,
      const ReductionFunction<T>* fn = ReductionFunction<T>::sum)
      : Algorithm(context),
        ptrs_(ptrs),
        count_(count),
        recvElems_(recvElems),
        bytes_(count_ * sizeof(T)),
        steps_(log2(this->contextSize_)),
        chunks_(1 << steps_),
        chunkSize_((count_ + chunks_ - 1) / chunks_),
        chunkBytes_(chunkSize_ * sizeof(T)),
        fn_(fn),
        recvBuf_(chunkSize_ << steps_),
        recvBufDist_(count_),
        sendOffsets_(steps_),
        recvOffsets_(steps_),
        sendCounts_(steps_, 0),
        recvCounts_(steps_, 0),
        sendCountToLargerBlock_(0),
        offsetToMyBinaryBlock_(0),
        myBinaryBlockSize_(0),
        stepsWithinBlock_(0),
        rankInBinaryBlock_(0),
        nextSmallerBlockSize_(0),
        nextLargerBlockSize_(0) {
    if (this->contextSize_ == 1) {
      return;
    }

    initBinaryBlocks();
    sendDataBufs_.reserve(stepsWithinBlock_);
    recvDataBufs_.reserve(stepsWithinBlock_);
    // Reserve max needed number of context slots. Up to 4 slots per process
    // pair are needed (two for regular sends and two for notifications). For
    // simplicity, the same mapping is used on all processes so that the slots
    // trivially match across processes
    slotOffset_ = this->context_->nextSlot(
        4 * this->contextSize_ * (this->contextSize_ - 1));

    size_t bitmask = 1;
    size_t stepChunkSize = chunkSize_ << (steps_ - 1);
    size_t stepChunkBytes = stepChunkSize * sizeof(T);
    size_t sendOffset = 0;
    size_t recvOffset = 0;
    size_t bufferOffset = 0; // offset into recvBuf_
    for (int i = 0; i < stepsWithinBlock_; i++) {
      const int destRank = (this->context_->rank) ^ bitmask;
      auto& pair = this->context_->getPair(destRank);
      sendOffsets_[i] = sendOffset + ((destRank & bitmask) ? stepChunkSize : 0);
      recvOffsets_[i] =
          recvOffset + ((this->context_->rank & bitmask) ? stepChunkSize : 0);
      if (sendOffsets_[i] < count_) {
        // specifies number of elements to send in each step
        if (sendOffsets_[i] + stepChunkSize > count_) {
          sendCounts_[i] = count_ - sendOffsets_[i];
        } else {
          sendCounts_[i] = stepChunkSize;
        }
      }
      int myRank = this->context_->rank;
      auto slot = slotOffset_ +
          2 *
              (std::min(myRank, destRank) * this->contextSize_ +
               std::max(myRank, destRank));
      sendDataBufs_.push_back(pair->createSendBuffer(slot, ptrs_[0], bytes_));
      if (recvOffsets_[i] < count_) {
        // specifies number of elements received in each step
        if (recvOffsets_[i] + stepChunkSize > count_) {
          recvCounts_[i] = count_ - recvOffsets_[i];
        } else {
          recvCounts_[i] = stepChunkSize;
        }
      }
      recvDataBufs_.push_back(pair->createRecvBuffer(
          slot, &recvBuf_[bufferOffset], stepChunkBytes));
      bufferOffset += stepChunkSize;
      if (this->context_->rank & bitmask) {
        sendOffset += stepChunkSize;
        recvOffset += stepChunkSize;
      }
      bitmask <<= 1;
      stepChunkSize >>= 1;
      stepChunkBytes >>= 1;

      ++slot;
      sendNotificationBufs_.push_back(
          pair->createSendBuffer(slot, &dummy_, sizeof(dummy_)));
      recvNotificationBufs_.push_back(
          pair->createRecvBuffer(slot, &dummy_, sizeof(dummy_)));
    }

    const auto myRank = this->context_->rank;
    if (nextSmallerBlockSize_ != 0) {
      const auto offsetToSmallerBlock =
          offsetToMyBinaryBlock_ + myBinaryBlockSize_;
      const int destRank =
          offsetToSmallerBlock + rankInBinaryBlock_ % nextSmallerBlockSize_;
      auto& destPair = this->context_->getPair(destRank);
      auto slot = slotOffset_ +
          2 *
              (std::min(myRank, destRank) * this->contextSize_ +
               std::max(myRank, destRank));
      const auto itemCount = recvCounts_[stepsWithinBlock_ - 1];
      if (itemCount > 0) {
        smallerBlockRecvDataBuf_ = destPair->createRecvBuffer(
            slot, &recvBuf_[bufferOffset], itemCount * sizeof(T));
      }
    }
    if (nextLargerBlockSize_ != 0) {
      // Due to the design decision of sending large messages to nearby ranks,
      // after the reduce-scatter the reduced chunks end up in an order
      // according to the reversed bit pattern of each proc's rank within the
      // block. So, instead of ranks 0, 1, 2, ... 7 having blocks A, B, C, D, E,
      // F, G, H etc. what you get is A, E, C, G, B, F, D, H. Taking this
      // example further, if there is also a smaller binary block of size 2
      // (with the reduced blocks A - D, E - H), rank 0 within the smaller block
      // will need to send chunks of its buffer to ranks 0, 4, 2, 6 within the
      // larger block (in that order) and rank 1 will send to 1, 5, 3, 7. Within
      // the reversed bit patterns, this communication is actually 0 to [0, 1,
      // 2, 3] and 1 to [4, 5, 6, 7].
      const auto offsetToLargerBlock =
          offsetToMyBinaryBlock_ - nextLargerBlockSize_;
      const auto numSendsAndReceivesToLargerBlock =
          nextLargerBlockSize_ / myBinaryBlockSize_;
      sendCountToLargerBlock_ = stepChunkSize >>
          (static_cast<size_t>(log2(numSendsAndReceivesToLargerBlock)) - 1);
      auto srcOrdinal =
          reverseLastNBits(rankInBinaryBlock_, log2(myBinaryBlockSize_));
      auto destOrdinal = srcOrdinal * numSendsAndReceivesToLargerBlock;
      for (int i = 0; i < numSendsAndReceivesToLargerBlock; i++) {
        const int destRank = offsetToLargerBlock +
            reverseLastNBits(destOrdinal, log2(nextLargerBlockSize_));
        auto& destPair = this->context_->getPair(destRank);
        auto slot = slotOffset_ +
            2 *
                (std::min(myRank, destRank) * this->contextSize_ +
                 std::max(myRank, destRank));
        largerBlockSendDataBufs_.push_back(
            destPair->createSendBuffer(slot, ptrs[0], bytes_));
        destOrdinal++;
      }
    }

    // Distribution phase: Scatter/distribute based on user-specified
    // distribution. Note that, due to nature of recursive halving algorithm
    // in the largest binary block, the blocks are not ordered in correct order.
    // Enforce correct order by exchanging data between processes p and p',
    // where p' is the bit-reverse of p.

    // Sends: The largest binary block ends up having the scattered data.
    // Therefore, only those ranks participate in sending messages.
    if (nextLargerBlockSize_ == 0 && stepsWithinBlock_ > 0) {
      getDistributionMap(
          recvOffsets_[stepsWithinBlock_ - 1],
          recvCounts_[stepsWithinBlock_ - 1],
          recvElems_,
          false,
          distMapForSend_);
      for (const auto& distMap : distMapForSend_) {
        const int destRank = distMap.rank;
        if (myRank != destRank) {
          auto& destPair = this->context_->getPair(destRank);
          auto slot = slotOffset_ + 2 +
              2 *
                  (std::min(myRank, destRank) * this->contextSize_ +
                   std::max(myRank, destRank));
          distSendDataBufs_.push_back(
              destPair->createSendBuffer(slot, ptrs_[0], bytes_));
          ++slot;
          recvNotificationBufs_.push_back(
              destPair->createRecvBuffer(slot, &dummy_, sizeof(dummy_)));
        }
      }
    }

    // Recvs: Recv the data from the largest binary block. Based on the
    // user-specified distribution, the receivers identify which ranks in the
    // binary block they should receive from. Since the data in
    // largest binary block is reordered after recursive-halving, the receivers
    // reorder the sender info here.
    if (recvElems_[myRank] > 0) {
      std::vector<int> srcCounts;
      size_t rem = count_;
      for (int i = 0; i < this->contextSize_; ++i) {
        srcCounts.push_back(std::min(chunkSize_, rem));
        rem = rem > chunkSize_ ? rem - chunkSize_ : 0;
      }
      size_t offset = 0;
      for (int i = 0; i < myRank; ++i) {
        offset += recvElems_[i];
      }
      getDistributionMap(
          offset, recvElems_[myRank], srcCounts, true, distMapForRecv_);
      for (const auto& distMap : distMapForRecv_) {
        const int srcRank = distMap.rank;
        if (myRank != srcRank) {
          auto& destPair = this->context_->getPair(srcRank);
          auto slot = slotOffset_ + 2 +
              2 *
                  (std::min(myRank, srcRank) * this->contextSize_ +
                   std::max(myRank, srcRank));
          distRecvDataBufs_.push_back(destPair->createRecvBuffer(
              slot,
              &recvBufDist_[distMap.offset],
              distMap.itemCount * sizeof(T)));
          ++slot;
          sendNotificationBufs_.push_back(
              destPair->createSendBuffer(slot, &dummy_, sizeof(dummy_)));
        }
      }
    }
  }

  void run() {
    size_t bufferOffset = 0;
    size_t numItems =
        stepsWithinBlock_ > 0 ? chunkSize_ << (steps_ - 1) : count_;

    for (int i = 1; i < ptrs_.size(); i++) {
      fn_->call(ptrs_[0], ptrs_[i], count_);
    }
    if (this->contextSize_ == 1) {
      // Broadcast ptrs_[0]
      for (int i = 1; i < ptrs_.size(); i++) {
        memcpy(ptrs_[i], ptrs_[0], bytes_);
      }
      return;
    }

    // Reduce-scatter (within binary block).
    for (int i = 0; i < stepsWithinBlock_; i++) {
      if (sendOffsets_[i] < count_) {
        sendDataBufs_[i]->send(
            sendOffsets_[i] * sizeof(T), sendCounts_[i] * sizeof(T));
      }
      if (recvOffsets_[i] < count_) {
        recvDataBufs_[i]->waitRecv();
        fn_->call(
            &ptrs_[0][recvOffsets_[i]],
            &recvBuf_[bufferOffset],
            recvCounts_[i]);
      }
      bufferOffset += numItems;
      sendNotificationBufs_[i]->send();
      numItems >>= 1;
    }

    // Communication across binary blocks for non-power-of-two number of
    // processes

    // receive from smaller block
    // data sizes same as in the last step of intrablock reduce-scatter above
    int sendNotifyOffset = stepsWithinBlock_;
    if (nextSmallerBlockSize_ != 0 && smallerBlockRecvDataBuf_ != nullptr) {
      smallerBlockRecvDataBuf_->waitRecv();
      fn_->call(
          &ptrs_[0][recvOffsets_[stepsWithinBlock_ - 1]],
          &recvBuf_[bufferOffset],
          recvCounts_[stepsWithinBlock_ - 1]);
    }

    const auto totalItemsToSend =
        stepsWithinBlock_ > 0 ? recvCounts_[stepsWithinBlock_ - 1] : count_;
    if (nextLargerBlockSize_ != 0 && totalItemsToSend != 0) {
      // scatter to larger block
      const auto offset =
          stepsWithinBlock_ > 0 ? recvOffsets_[stepsWithinBlock_ - 1] : 0;
      const auto numSendsAndReceivesToLargerBlock =
          nextLargerBlockSize_ / myBinaryBlockSize_;
      for (int i = 0; i < numSendsAndReceivesToLargerBlock; i++) {
        if (sendCountToLargerBlock_ * i < totalItemsToSend) {
          largerBlockSendDataBufs_[i]->send(
              (offset + i * sendCountToLargerBlock_) * sizeof(T),
              std::min(
                  sendCountToLargerBlock_,
                  totalItemsToSend - sendCountToLargerBlock_ * i) *
                  sizeof(T));
        }
      }
    }

    // Distribution phase: Scatter/distribute based on user specified
    // distribution.
    int index = 0;
    for (const auto& distMap : distMapForSend_) {
      const auto myRank = this->context_->rank;
      const int destRank = distMap.rank;
      if (myRank != destRank) {
        distSendDataBufs_[index++]->send(
            distMap.offset * sizeof(T), distMap.itemCount * sizeof(T));
      }
    }
    index = 0;
    bufferOffset = 0;
    for (const auto& distMap : distMapForRecv_) {
      const auto myRank = this->context_->rank;
      const int srcRank = distMap.rank;
      if (myRank != srcRank) {
        distRecvDataBufs_[index++]->waitRecv();
        memcpy(
            &ptrs_[0][bufferOffset],
            &recvBufDist_[distMap.offset],
            distMap.itemCount * sizeof(T));
        sendNotificationBufs_[sendNotifyOffset++]->send();
      } else {
        if (myRank != 0) { // Data already in-place for rank 0.
          memcpy(
              &ptrs_[0][bufferOffset],
              &ptrs_[0][distMap.offset],
              distMap.itemCount * sizeof(T));
        }
      }
      bufferOffset += distMap.itemCount;
    }

    // Broadcast ptrs_[0]
    for (int i = 1; i < ptrs_.size(); i++) {
      memcpy(ptrs_[i], ptrs_[0], bytes_);
    }

    // Wait for all notifications to make sure we can send data immediately
    // without risking overwriting data in its receive buffer before it
    // consumed that data.
    for (auto& recvNotificationBuf : recvNotificationBufs_) {
      recvNotificationBuf->waitRecv();
    }
  }

 protected:
  std::vector<T*> ptrs_;
  const int count_;
  const std::vector<int> recvElems_;
  const int bytes_;
  const size_t steps_;
  const size_t chunks_;
  const size_t chunkSize_;
  const size_t chunkBytes_;
  const ReductionFunction<T>* fn_;

  // buffer where data is received prior to being reduced
  std::vector<T> recvBuf_;

  // buffer where data is received during distribution phase
  std::vector<T> recvBufDist_;

  // offsets into the data buffer from which to send during the reduce-scatter
  // these become the offsets at which the process receives during the allgather
  // indexed by step
  std::vector<size_t> sendOffsets_;

  // offsets at which data is reduced during the reduce-scatter and sent from in
  // the allgather
  std::vector<size_t> recvOffsets_;

  std::vector<std::unique_ptr<transport::Buffer>> sendDataBufs_;
  std::vector<std::unique_ptr<transport::Buffer>> recvDataBufs_;

  std::unique_ptr<transport::Buffer> smallerBlockRecvDataBuf_;
  std::vector<std::unique_ptr<transport::Buffer>> largerBlockSendDataBufs_;

  std::unique_ptr<transport::Buffer> xchgBlockSendDataBuf_;
  std::unique_ptr<transport::Buffer> xchgBlockRecvDataBuf_;

  std::vector<std::unique_ptr<transport::Buffer>> distSendDataBufs_;
  std::vector<std::unique_ptr<transport::Buffer>> distRecvDataBufs_;

  std::vector<DistributionMap> distMapForSend_;
  std::vector<DistributionMap> distMapForRecv_;

  std::vector<size_t> sendCounts_;
  std::vector<size_t> recvCounts_;
  size_t sendCountToLargerBlock_;

  int dummy_;
  std::vector<std::unique_ptr<transport::Buffer>> sendNotificationBufs_;
  std::vector<std::unique_ptr<transport::Buffer>> recvNotificationBufs_;

  // for non-power-of-two number of processes, partition the processes into
  // binary blocks and keep track of which block each process is in, as well as
  // the adjoining larger and smaller blocks (with which communication will be
  // required)
  uint32_t offsetToMyBinaryBlock_;
  uint32_t myBinaryBlockSize_;
  uint32_t stepsWithinBlock_;
  uint32_t rankInBinaryBlock_;
  uint32_t nextSmallerBlockSize_;
  uint32_t nextLargerBlockSize_;

  int slotOffset_;
};

} // namespace gloo
