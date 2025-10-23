/**
 * Copyright (c) 2018-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "gloo/cuda_allreduce_bcube.h"

#include "gloo/common/log.h"
#include "gloo/cuda_collectives_device.h"
#include "gloo/cuda_collectives_host.h"
#include "gloo/cuda_private.h"

#include <sstream>
#include <string_view>
#ifndef _WIN32
#include <unistd.h>
#endif

#ifdef DEBUG
#define DEBUG_PRINT_STAGE(stage) \
  do {                           \
    printStageBuffer(stage);     \
  } while (false)
#define DEBUG_PRINT_SEND(stage)                                              \
  do {                                                                       \
    printStepBuffer(                                                         \
        stage, step, myRank_, destRank, &scratch_[0], sendCount, ptrOffset); \
  } while (false)
#define DEBUG_PRINT_RECV(stage)              \
  do {                                       \
    printStepBuffer(                         \
        stage,                               \
        step,                                \
        srcRank,                             \
        myRank_,                             \
        &recvBufs_[recvBufIdx_[srcRank]][0], \
        recvCount);                          \
  } while (false)
#else
#define DEBUG_PRINT_STAGE(stage)
#define DEBUG_PRINT_SEND(stage)
#define DEBUG_PRINT_RECV(stage)
#endif

namespace gloo {

template <typename T, typename W>
CudaAllreduceBcube<T, W>::CudaAllreduceBcube(
    const std::shared_ptr<Context>& context,
    const std::vector<T*>& ptrs,
    const int count,
    const std::vector<cudaStream_t>& streams,
    const CudaReductionFunction<T>* fn)
    : Algorithm(context),
      myRank_(this->context_->rank),
      base_(this->context_->base ? this->context_->base : 2),
      nodes_(this->contextSize_),
      totalNumElems_(count),
      bytes_(totalNumElems_ * sizeof(T)),
      steps_(computeSteps(nodes_, base_)),
      fn_(fn),
      recvBufs_(steps_ * base_) {
  auto newStream = true;
  if (streams.size() > 0) {
    GLOO_ENFORCE_EQ(streams.size(), ptrs.size());
    newStream = false;
  }
  for (auto i = 0; i < ptrs.size(); i++) {
    auto ptr = CudaDevicePointer<T>::create(ptrs[i], totalNumElems_);
    if (newStream) {
      streams_.push_back(CudaStream(ptr.getDeviceID()));
    } else {
      streams_.push_back(CudaStream(ptr.getDeviceID(), streams[i]));
    }
    devicePtrs_.push_back(std::move(ptr));
  }
  // Workspace specific initialization (see below)
  init();
  if (nodes_ == 1) {
    return;
  }
  setupNodes();
  /*
   * Reserve max needed number of context slots. Up to 2 slots per process
   * pair are needed (one for regular sends and one for notifications). For
   * simplicity, the same mapping is used on all processes so that the slots
   * trivially match across processes
   */
  int slotOffset_ = this->context_->nextSlot(
      2 * this->contextSize_ * (this->contextSize_ - 1));

  int bufIdx = 0;
  for (int step = 0; step < steps_; ++step) {
    for (int destRank : getPeersPerStep(myRank_, step)) {
      int recvSize = std::max(
          getNumElemsPerStep(myRank_, step),
          getNumElemsPerStep(destRank, step));
      auto& pair = this->context_->getPair(destRank);
      auto slot = slotOffset_ +
          2 *
              (std::min(myRank_, destRank) * nodes_ +
               std::max(myRank_, destRank));
      sendDataBufs_[destRank] = pair->createSendBuffer(slot, *scratch_, bytes_);
      recvBufs_[bufIdx] = W::Pointer::alloc(recvSize);
      recvDataBufs_[destRank] = pair->createRecvBuffer(
          slot, &recvBufs_[bufIdx][0], recvSize * sizeof(T));
      recvBufIdx_[destRank] = bufIdx;
      ++bufIdx;
      ++slot;
      sendNotificationBufs_[destRank] =
          pair->createSendBuffer(slot, &dummy_, sizeof(dummy_));
      recvNotificationBufs_[destRank] =
          pair->createRecvBuffer(slot, &dummy_, sizeof(dummy_));
    } // nodes
  } // steps
}

template <typename T, typename W>
void CudaAllreduceBcube<T, W>::run() {
  CudaDeviceGuard guard;
  CudaStream& stream = *scratchStream_;

  localReduceOp_->run();

  if (nodes_ == 1) {
    GLOO_ENFORCE(
        localBroadcastOp_,
        "localBroadcastOp must be initialized for single machine");
    localBroadcastOp_->run();
    return;
  }

  // Reduce-scatter
  DEBUG_PRINT_STAGE("start");
  for (int step = 0; step < steps_; ++step) {
    const auto& peerRanks = getPeersPerStep(myRank_, step);
    for (int destRank : peerRanks) {
      int sendCount = getNumElemsPerStep(destRank, step);
      int ptrOffset = getPtrOffsetPerStep(destRank, step);
      DEBUG_PRINT_SEND("reduce-scatter");
      sendDataBufs_[destRank]->send(
          ptrOffset * sizeof(T), sendCount * sizeof(T));
    } // sends within group

    for (int srcRank : peerRanks) {
      int recvCount = getNumElemsPerStep(myRank_, step);
      int ptrOffset = getPtrOffsetPerStep(myRank_, step);
      recvDataBufs_[srcRank]->waitRecv();
      DEBUG_PRINT_RECV("reduce-scatter");
      auto recvBufAtOffset =
          recvBufs_[recvBufIdx_[srcRank]].range(0, recvCount);
      auto scratchAtOffset = scratch_.range(ptrOffset, recvCount);
      fn_->call(scratchAtOffset, recvBufAtOffset, recvCount, stream);
      stream.wait();
      /*
       * Send notification to the pair we just received from that
       * we're done dealing with the receive buffer.
       */
      sendNotificationBufs_[srcRank]->send();
    } // recvs within group and reduces
  } // reduce-scatter steps

  DEBUG_PRINT_STAGE("reduce-scattered");

  // All-gather
  for (int step = steps_ - 1; step >= 0; --step) {
    const auto& peerRanks = getPeersPerStep(myRank_, step);
    for (int destRank : peerRanks) {
      int sendCount = getNumElemsPerStep(myRank_, step);
      int ptrOffset = getPtrOffsetPerStep(myRank_, step);
      /*
       * Wait for notification from the peer to make sure we can send data
       * without risking any overwrites in its receive buffer.
       */
      recvNotificationBufs_[destRank]->waitRecv();
      DEBUG_PRINT_SEND("all-gather");
      sendDataBufs_[destRank]->send(
          ptrOffset * sizeof(T), sendCount * sizeof(T));
    }

    for (int srcRank : peerRanks) {
      int recvCount = getNumElemsPerStep(srcRank, step);
      int ptrOffset = getPtrOffsetPerStep(srcRank, step);
      recvDataBufs_[srcRank]->waitRecv();
      DEBUG_PRINT_RECV("all-gather");
      auto recvBufAtOffset =
          recvBufs_[recvBufIdx_[srcRank]].range(0, recvCount);
      auto scratchAtOffset = scratch_.range(ptrOffset, recvCount);
      stream.copyAsync(scratchAtOffset, recvBufAtOffset);
      stream.wait();
      if (step == 0) {
        /*
         * Send notification to the pair we just received from that
         * we're done dealing with the receive buffer.``
         */
        sendNotificationBufs_[srcRank]->send();
      }
    } // recvs within group and reduces
  } // all-gather steps

  DEBUG_PRINT_STAGE("all-reduced");

  localBroadcastOp_->runAsync();
  localBroadcastOp_->wait();

  /*
   * Wait for notifications from our peers within the block to make
   * sure we can send data immediately without risking overwriting
   * data in its receive buffer before it consumed that data.
   */
  for (int peerRank : getPeersPerStep(myRank_, 0)) {
    recvNotificationBufs_[peerRank]->waitRecv();
  }
}

template <typename T, typename W>
int CudaAllreduceBcube<T, W>::computeSteps(int nodes, int peers) {
  float lg2n = log2(nodes);
  float lg2p = log2(peers);
  return ceil(lg2n / lg2p);
}

template <typename T, typename W>
bool CudaAllreduceBcube<T, W>::printCheck(int /* rank */) {
  return false;
}

template <typename T, typename W>
void CudaAllreduceBcube<T, W>::printElems(T* p, int count, int start) {
  /* Early return if log level is not high enough, to prevent expensive code
   * running. */
  if (!spdlog::should_log(spdlog::level::trace))
    return;

  const std::size_t alignedStart = (start / wordsPerLine) * wordsPerLine;
  fmt::memory_buffer line{};

  /* Logs/flushes the line buffer - starting a new line */
  auto printLine = [&]() {
    if (!line.size())
      return;
    std::string_view sv{line.data(), line.size()};
    GLOO_TRACE("{}", sv);
    line.clear();
  };

  for (std::size_t x = alignedStart; x < start + count; ++x) {
    if (x % wordsPerLine == 0) {
      if (x != alignedStart)
        printLine();
      fmt::format_to(
          std::back_inserter(line), "{} {:05}: ", fmt::ptr(&p[x]), x);
    } else if (x % wordsPerSection == 0) {
      fmt::format_to(std::back_inserter(line), "- ");
    }

    if (x < start)
      fmt::format_to(std::back_inserter(line), "..... ");
    else
      fmt::format_to(std::back_inserter(line), "{:05} ", p[x]);
  }
  printLine();
}

template <typename T, typename W>
void CudaAllreduceBcube<T, W>::printStageBuffer(const std::string& msg) {
  if (printCheck(myRank_)) {
    GLOO_TRACE("rank ({}) {}:", myRank_, msg);
    printElems(&scratch_[0], totalNumElems_);
  }
}

template <typename T, typename W>
void CudaAllreduceBcube<T, W>::printStepBuffer(
    const std::string& stage,
    int step,
    int srcRank,
    int destRank,
    T* p,
    int count,
    int start) {
  if (printCheck(myRank_)) {
    GLOO_TRACE(
        "{}: step ({}) srcRank ({}) -> destRank ({}):",
        stage,
        step,
        srcRank,
        destRank);
    printElems(p, count, start);
  }
}

template <typename T, typename W>
const std::vector<int>& CudaAllreduceBcube<T, W>::getPeersPerStep(
    int rank,
    int step) {
  return allNodes_[rank].getPeersPerStep(step);
}

template <typename T, typename W>
int CudaAllreduceBcube<T, W>::getNumElemsPerStep(int rank, int step) {
  return allNodes_[rank].getNumElemsPerStep(step);
}

template <typename T, typename W>
int CudaAllreduceBcube<T, W>::getPtrOffsetPerStep(int rank, int step) {
  return allNodes_[rank].getPtrOffsetPerStep(step);
}

template <typename T, typename W>
void CudaAllreduceBcube<T, W>::createNodes() {
  for (int rank = 0; rank < nodes_; ++rank) {
    allNodes_.emplace_back(rank, steps_);
  }
}

template <typename T, typename W>
void CudaAllreduceBcube<T, W>::updateGroupNodes(
    int step,
    const cuda::bcube::Group& group) {
  const std::vector<int>& peers = group.getNodeRanks();
  const int peersSz = peers.size();
  int ptrOffset = group.getPtrOffset();
  int count = group.getNumElems() / peersSz;
  const int countRem = group.getNumElems() % peersSz;
  if (0 == count) {
    count = 1;
  }
  for (int i = 0; i < peersSz; ++i) {
    cuda::bcube::Node& node = allNodes_[peers[i]];
    if (peersSz - 1 != i) { // if not the last node in group
      node.setPerStepAttributes(step, peers, count, ptrOffset);
      ptrOffset += count;
    } else {
      /*
       * The last node get the remainder elements if the number of
       * elements is not exactly divisible by number of peers
       */
      node.setPerStepAttributes(step, peers, count + countRem, ptrOffset);
      ptrOffset += count + countRem;
    }
    ptrOffset %= totalNumElems_;
  }
}

template <typename T, typename W>
void CudaAllreduceBcube<T, W>::setupNodes() {
  // Create all the nodes upfront
  createNodes();

  // Now we actually try to set up the nodes
  int peerDistance = 1;
  for (int step = 0; step < steps_; ++step) {
    std::vector<cuda::bcube::Group> groups;
    // Iterate over all the nodes to identify the first node of each group
    for (int rank = 0; rank < nodes_; ++rank) {
      const cuda::bcube::Node& firstNode = allNodes_[rank];
      // Only the ones with no peers would be first node
      if (0 == firstNode.getPeersPerStep(step).size()) {
        // Create a new group
        groups.emplace_back(
            step, firstNode, peerDistance, base_, nodes_, totalNumElems_);
        // check the size to keep link happy :/
        if (0 < groups.size()) {
          // Iterrate over all the peer nodes and set them up for the step
          updateGroupNodes(step, groups.back());
        }
      } // if (0 == firstNode ...
    } // for (int rank = 0..
    // Done iterating over all the nodes. Update peerDistance for next step.
    peerDistance *= base_;
  } // for (int step ...
} // setupNodes

template <typename T, typename W>
template <typename U>
void CudaAllreduceBcube<T, W>::init(
    typename std::enable_if<
        std::is_same<U, CudaHostWorkspace<T>>::value,
        typename U::Pointer>::type*) {
  // Since reduction is executed on the CPU, the scratch space
  // where they are accumulated is a new host side buffer.
  scratch_ = W::Pointer::alloc(totalNumElems_);
  scratchStream_ = &streams_[0];

  // Set up local reduction and broadcast operations on the host.
  // If devicePtrs_.size() == 1 these functions construct an op that
  // executes a memcpy such that scratch_ always holds the result.
  if (bytes_ < kOnDeviceThreshold) {
    localReduceOp_ =
        cudaHostReduce(streams_, devicePtrs_, scratch_, fn_, 0, totalNumElems_);
    localBroadcastOp_ =
        cudaHostBroadcast(streams_, devicePtrs_, scratch_, 0, totalNumElems_);
  } else {
    localReduceOp_ = cudaDeviceReduce(
        streams_, devicePtrs_, scratch_, fn_, 0, totalNumElems_);
    localBroadcastOp_ =
        cudaDeviceBroadcast(streams_, devicePtrs_, scratch_, 0, totalNumElems_);
  }
}

template <typename T, typename W>
template <typename U>
void CudaAllreduceBcube<T, W>::init(
    typename std::enable_if<
        std::is_same<U, CudaDeviceWorkspace<T>>::value,
        typename U::Pointer>::type*) {
  // The networking adapter does DMA to/from GPU memory, so we should reduce
  // onto the device that's closest to the networking adapter bound
  // to our context. This uses PCI distance to find closest GPU.
  auto index = findCudaDevicePointerClosestToDevice(
      devicePtrs_, this->context_->getDevice());
  scratch_ = CudaDevicePointer<T>::create(devicePtrs_[index]);
  scratchStream_ = &streams_[index];

  // Set up local reduction and broadcast operations on the device.
  // When running with a device workspace we intend to never leave the device.

  if (devicePtrs_.size() > 1) {
    localReduceOp_ = cudaDeviceReduce(
        streams_, devicePtrs_, scratch_, fn_, 0, totalNumElems_);
    localBroadcastOp_ =
        cudaDeviceBroadcast(streams_, devicePtrs_, scratch_, 0, totalNumElems_);
  }
}

namespace cuda {
namespace bcube {

Node::Node(int rank, int steps) : rank_(rank) {
  for (int i = 0; i < steps; ++i) {
    peersPerStep_.emplace_back();
  }
  numElemsPerStep_.reserve(steps);
  ptrOffsetPerStep_.reserve(steps);
}

int Node::getRank() const {
  return rank_;
}

void Node::setPerStepAttributes(
    int step,
    const std::vector<int>& peerRanks,
    int numElems,
    int offset) {
  for (int peerRank : peerRanks) {
    if (peerRank != rank_) {
      peersPerStep_[step].emplace_back(peerRank);
    }
  }
  numElemsPerStep_[step] = numElems;
  ptrOffsetPerStep_[step] = offset;
}

const std::vector<int>& Node::getPeersPerStep(int step) const {
  return peersPerStep_[step];
}

int Node::getNumElemsPerStep(int step) const {
  return numElemsPerStep_[step];
}

int Node::getPtrOffsetPerStep(int step) const {
  return ptrOffsetPerStep_[step];
}

Group::Group(
    int step,
    const Node& firstNode,
    int peerDistance,
    int base,
    int nodes,
    int totalNumElems)
    : nodeRanks_(getNodeRanks(firstNode.getRank(), peerDistance, base, nodes)),
      ptrOffset_((0 == step) ? 0 : firstNode.getPtrOffsetPerStep(step - 1)),
      numElems_(
          computeNumElems(step, firstNode, nodeRanks_.size(), totalNumElems)) {}

const std::vector<int>& Group::getNodeRanks() const {
  return nodeRanks_;
}

int Group::getPtrOffset() const {
  return ptrOffset_;
}

int Group::getNumElems() const {
  return numElems_;
}

int Group::computeNumElems(
    int step,
    const Node& firstNode,
    int peers,
    int count) {
  int groupCount = (0 == step) ? count : firstNode.getNumElemsPerStep(step - 1);
  return std::max(groupCount, peers);
}

std::vector<int> Group::getNodeRanks(
    int firstNodeRank,
    int peerDistance,
    int base,
    int nodes) const {
  std::vector<int> groupPeers;
  for (int i = 0; i < base; ++i) {
    int peerRank = firstNodeRank + i * peerDistance;
    if (peerRank < nodes) {
      groupPeers.emplace_back(peerRank);
    }
  }
  return groupPeers;
}

} // namespace bcube
} // namespace cuda

#define INSTANTIATE_TEMPLATE(T)                               \
  template class CudaAllreduceBcube<T, CudaHostWorkspace<T>>; \
  template class CudaAllreduceBcube<T, CudaDeviceWorkspace<T>>;

INSTANTIATE_TEMPLATE(int8_t);
INSTANTIATE_TEMPLATE(uint8_t);
INSTANTIATE_TEMPLATE(int32_t);
INSTANTIATE_TEMPLATE(int64_t);
INSTANTIATE_TEMPLATE(uint64_t);
INSTANTIATE_TEMPLATE(float);
INSTANTIATE_TEMPLATE(double);
INSTANTIATE_TEMPLATE(float16);

#if GLOO_USE_TORCH_DTYPES
INSTANTIATE_TEMPLATE(c10::BFloat16);
INSTANTIATE_TEMPLATE(c10::Half);
#endif

} // namespace gloo
