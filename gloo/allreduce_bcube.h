/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <math.h>
#include <stddef.h>
#include <string.h>
#include <algorithm>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "gloo/algorithm.h"
#include "gloo/common/error.h"
#include "gloo/context.h"

/**
 * This file contains following classes:
 *
 * **Node**
 *
 * This is a helper class. We create one object for each node
 * participating in allreduce operation with respective rank. It enacapsulates
 * information related to processing of elements. That is, how many elements
 * need to be sent from what offset or received by a particular node and be
 * reduced at what offset etc.
 *
 * **Group**
 *
 * This is another helper class. As part of each step of processing
 * we divide nodes into multiple groups. This class helps track properties of
 * that group. Such as, which nodes are part of the group, how many elements
 * collectively all nodes need to process and at what offset etc.
 *
 * **AllreduceBcube**
 *
 * This is the main allreduce implementation. Bcube is a scheme where nodes are
 * divided in groups. In reduce-scatter stage, in each group, a node peers with
 * `base - 1` other nodes. In the first step data is reduced between nodes
 * within the group. In the next step each node of a group peers with `base - 1`
 * nodes from other exclusively different groups. Since each node would start
 * with reduced data communicating with it would be like communicating with
 * `base` number of nodes/groups from the previous step. This process continues
 * until all the groups are covered and to be able to do that the algorithm
 * would have log_base(n) number of steps. Each step the node reduces
 * totalNumElems_ / (base^step) amount of elements. At the end of reduce-scatter
 * stage each node would have reduced a chunk of elements. Now, in all-gather
 * we follow a reverse process of reduce-scatter to communicate the reduced data
 * with other nodes.
 */
namespace gloo {

namespace bcube {

/**
 * Helps capture all information related to a node
 */
class Node {
 public:
  explicit Node(int rank, int steps) : rank_(rank) {
    for (int i = 0; i < steps; ++i) {
      peersPerStep_.emplace_back();
    }
    numElemsPerStep_.resize(steps);
    ptrOffsetPerStep_.resize(steps);
  }
  /**
   * Get the rank of this node
   */
  int getRank() const {
    return rank_;
  }
  /**
   * Used to record all the peer nodes, the number of elements to process and
   * the offset from which data in the original ptr buffer will be processed by
   * this node in a particular step. This is to be done as part of setup()
   * function only.
   * @param step The step for which we are recording attributes
   * @param peerRanks All peer ranks. This would contain self too so need to
   * @param numElems The number of elements this node will be processing in the
   * @param offset The offset in the ptrs array
   *  filter that out.
   */
  void setPerStepAttributes(
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
  /**
   * Get all the nodes this node peers with in a particular step
   * @param step The step for which we need to get peers
   * @return List of ranks of all peer nodes
   */
  const std::vector<int>& getPeersPerStep(int step) const {
    return peersPerStep_[step];
  }
  /**
   * Get count of elements this node needs to process in a specified the step
   * @param step The step for which we are querying count
   */
  int getNumElemsPerStep(int step) const {
    return numElemsPerStep_[step];
  }
  /**
   * Get offset to ptrs array this node needs to start processing from in the
   * specified step
   * @param step The step for which we are querying offset
   */
  int getPtrOffsetPerStep(int step) const {
    return ptrOffsetPerStep_[step];
  }

 private:
  /**
   * Rank of this node
   */
  const int rank_;
  /**
   * A vector of a list of ranks (value) of nodes this node would
   * peer with in a step (index)
   */
  std::vector<std::vector<int>> peersPerStep_;
  /**
   * A vector of number of elements (value) this node needs to process in a step
   * (index). This could be the number of elements to be received and reduced by
   * a node and correspondingly sent by its peers during a step of
   * reduce-scatter stage, or, similarly, the number of elements received and
   * copied in the ptrs_ array by a node and correspondingly sent by it's peer
   * during a step of all-gather stage.
   */
  std::vector<int> numElemsPerStep_;
  /**
   * A vector of offset (value) within the ptrs_ array from which data needs to
   * be processed by this node in a step (index). This would be used by peers to
   * send data from ptrs_ array to this node and used with reduce function
   * during reduce-scatter phase or during all-gather to send elements to peers
   * from ptrs_ array.
   */
  std::vector<int> ptrOffsetPerStep_;
};

/**
 * Helps capture all information related to a peer group
 */
class Group {
 public:
  Group(
      int step,
      const Node& firstNode,
      int peerDistance,
      int base,
      int nodes,
      int totalNumElems)
      : nodeRanks_(
            getNodeRanks(firstNode.getRank(), peerDistance, base, nodes)),
        ptrOffset_((0 == step) ? 0 : firstNode.getPtrOffsetPerStep(step - 1)),
        numElems_(computeNumElems(
            step,
            firstNode,
            nodeRanks_.size(),
            totalNumElems)) {}
  /**
   * Simple getter for all the nodes in the group
   * @return List of ranks of nodes in the group
   */
  const std::vector<int>& getNodeRanks() const {
    return nodeRanks_;
  }
  /**
   * Get the offset from which the group should process data
   * @return Offset in the ptrs array
   */
  int getPtrOffset() const {
    return ptrOffset_;
  }
  /**
   * Get the number of elements this group is supposed to process
   * @return Count of elements (in ptr or receive buffers)
   */
  int getNumElems() const {
    return numElems_;
  }

 private:
  const std::vector<int> nodeRanks_;
  const int ptrOffset_;
  const int numElems_;
  /**
   * Computes the number of elements this group needs to process. If this is the
   * first step we start with all elements. For subsequent steps it's number
   * of elements processed by single node in previous step. If this value is
   * smaller than number of peers in the group simply use number of peers as the
   * count so that at least one element is exchanged. Also, note that in this
   * case some nodes may end up duplicating the work as the ptrOffset wraps
   * around the totalNumElems_ in updateGroupNodes() function.
   * @param step The current step
   * @param firstNode The first node in the group
   * @param peers The total number of peers in the group
   * @count The total number of elements to be processed by this node
   * @return The number of elements to be processed by this group
   */
  static int computeNumElems(
      int step,
      const Node& firstNode,
      int peers,
      int count) {
    int groupCount =
        (0 == step) ? count : firstNode.getNumElemsPerStep(step - 1);
    return std::max(groupCount, peers);
  }
  /**
   * Determines all the nodes in a group in a particular step
   * @param peerDistance This is the distance between rank of each peer in the
   *   group
   * @return List of ranks of nodes in the group
   */
  std::vector<int> getNodeRanks(
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
};

} // namespace bcube

/**
 * This is another implemenation of allreduce algorithm where-in we divide
 * nodes into group of base_ nodes instead of a factor of two used by
 * allreduce_halving_doubling. It basically shards the data based on the base
 * and does a reduce-scatter followed by all-gather very much like the
 * allreduce_halving_doubling algorithm.
 *
 * This algorithm can handle cases where we don't really have a complete
 * hypercube, i.e. number of nodes != c * base ^ x where c and x are some
 * contants; however,  the number of nodes must be divisible by base.
 */
template <typename T>
class AllreduceBcube : public Algorithm {
 public:
  AllreduceBcube(
      const std::shared_ptr<Context>& context,
      const std::vector<T*> ptrs,
      const size_t count,
      const ReductionFunction<T>* fn = ReductionFunction<T>::sum)
      : Algorithm(context),
        myRank_(this->context_->rank),
        base_(this->context_->base),
        nodes_(this->contextSize_),
        ptrs_(ptrs),
        totalNumElems_(count),
        bytes_(totalNumElems_ * sizeof(T)),
        steps_(computeSteps(nodes_, base_)),
        fn_(fn),
        recvBufs_(steps_ * base_) {
    if (totalNumElems_ == 0 || nodes_ == 1) {
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
        sendDataBufs_[destRank] =
            pair->createSendBuffer(slot, ptrs_[0], bytes_);
        recvBufs_[bufIdx].resize(recvSize);
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

#ifdef DEBUG
#define DEBUG_PRINT_STAGE(stage) \
  do {                           \
    printStageBuffer(stage);     \
  } while (false)
#define DEBUG_PRINT_SEND(stage)                                              \
  do {                                                                       \
    printStepBuffer(                                                         \
        stage, step, myRank_, destRank, &ptrs_[0][0], sendCount, ptrOffset); \
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

  void run() {
    if (totalNumElems_ == 0) {
      return;
    }
    // Local reduce operation
    for (int i = 1; i < ptrs_.size(); i++) {
      fn_->call(ptrs_[0], ptrs_[i], totalNumElems_);
    }

    if (nodes_ == 1) {
      // Broadcast ptrs_[0]
      for (int i = 1; i < ptrs_.size(); i++) {
        memcpy(ptrs_[i], ptrs_[0], bytes_);
      }
      return;
    }

    // Reduce-scatter
    DEBUG_PRINT_STAGE("start");
    for (int step = 0; step < steps_; ++step) {
      for (int destRank : getPeersPerStep(myRank_, step)) {
        int sendCount = getNumElemsPerStep(destRank, step);
        int ptrOffset = getPtrOffsetPerStep(destRank, step);
        DEBUG_PRINT_SEND("reduce-scatter");
        sendDataBufs_[destRank]->send(
            ptrOffset * sizeof(T), sendCount * sizeof(T));
      } // sends within group

      for (int srcRank : getPeersPerStep(myRank_, step)) {
        int recvCount = getNumElemsPerStep(myRank_, step);
        int ptrOffset = getPtrOffsetPerStep(myRank_, step);
        recvDataBufs_[srcRank]->waitRecv();
        DEBUG_PRINT_RECV("reduce-scatter");
        fn_->call(
            &ptrs_[0][ptrOffset],
            &recvBufs_[recvBufIdx_[srcRank]][0],
            recvCount);
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
      for (int destRank : getPeersPerStep(myRank_, step)) {
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

      for (int srcRank : getPeersPerStep(myRank_, step)) {
        int recvCount = getNumElemsPerStep(srcRank, step);
        int ptrOffset = getPtrOffsetPerStep(srcRank, step);
        recvDataBufs_[srcRank]->waitRecv();
        DEBUG_PRINT_RECV("all-gather");
        std::memcpy(
            &ptrs_[0][ptrOffset],
            &recvBufs_[recvBufIdx_[srcRank]][0],
            recvCount * sizeof(T));
        if (step == 0) {
          /*
           * Send notification to the pair we just received from that
           * we're done dealing with the receive buffer.
           */
          sendNotificationBufs_[srcRank]->send();
        }
      } // recvs within group and reduces
    } // all-gather steps

    DEBUG_PRINT_STAGE("all-reduced");

    // Broadcast ptrs_[0]
    for (int i = 1; i < ptrs_.size(); i++) {
      memcpy(ptrs_[i], ptrs_[0], bytes_);
    }

    /*
     * Wait for notifications from our peers within the block to make
     * sure we can send data immediately without risking overwriting
     * data in its receive buffer before it consumed that data.
     */
    for (int peerRank : getPeersPerStep(myRank_, 0)) {
      recvNotificationBufs_[peerRank]->waitRecv();
    }
  }

 private:
  /**
   * Number of words to be printed per section by printElems
   */
  static constexpr int wordsPerSection = 4;
  /**
   * Number of words to be printed per line by printElems
   */
  static constexpr int wordsPerLine = 4 * wordsPerSection;
  /**
   * Just a reference to current nodes rank
   */
  const int myRank_{0};
  /**
   * Number of nodes in a typical group
   */
  const int base_{2};
  /**
   * Total number of nodes
   */
  const int nodes_{0};
  /**
   * Pointer to the elements
   */
  const std::vector<T*> ptrs_{nullptr};
  /**
   * Total number of elements to process
   */
  const size_t totalNumElems_{0};
  /**
   * Total number of bytes to process
   */
  const size_t bytes_{0};
  /**
   * Total number of steps
   */
  const size_t steps_{0};
  /**
   * The reduce operation function
   */
  const ReductionFunction<T>* fn_{nullptr};
  /**
   * List of actual buffers for incoming data
   */
  std::vector<std::vector<T>> recvBufs_;
  /**
   * Map of rank to incoming buffer index in recvBufs
   */
  std::unordered_map<int, int> recvBufIdx_;
  /**
   * Map of rank to Buffer which will be used for outgoing data
   */
  std::unordered_map<int, std::unique_ptr<transport::Buffer>> sendDataBufs_;
  /**
   * Map of rank to Buffer which will be used for incoming data
   */
  std::unordered_map<int, std::unique_ptr<transport::Buffer>> recvDataBufs_;
  /**
   * Dummy data used to signal end of one setup
   */
  int dummy_;
  /**
   * Map of rank to Buffer which will be used for outgoing synchronization data
   * at end of reduce-scatter and all-gather
   */
  std::unordered_map<int, std::unique_ptr<transport::Buffer>>
      sendNotificationBufs_;
  /**
   * Map of rank to Buffer which will be used for incoming synchronization data
   * at end of reduce-scatter and all-gather
   */
  std::unordered_map<int, std::unique_ptr<transport::Buffer>>
      recvNotificationBufs_;
  /**
   * List of all the nodes
   */
  std::vector<bcube::Node> allNodes_;
  /**
   * Compute number of steps required in reduce-scatter and all-gather (each)
   * @param nodes The total number of nodes
   * @para peers The maximum number of peers in a group
   */
  static int computeSteps(int nodes, int peers) {
    float lg2n = log2(nodes);
    float lg2p = log2(peers);
    return ceil(lg2n / lg2p);
  }
  /**
   * Basically a gate to make sure only the right node(s) print logs
   * @param rank Rank of the current node
   */
  static bool printCheck(int /*rank*/) {
    return false;
  }
  /**
   * Prints a break given the offset of an element about to be printed
   * @param p Pointer to the elements
   * @param x The current offset to the pointer to words
   */
  static void printBreak(T* p, int x) {
    if (0 == x % wordsPerLine) {
      std::cout << std::endl
                << &p[x] << " " << std::setfill('0') << std::setw(5) << x
                << ": ";
    } else if (0 == x % wordsPerSection) {
      std::cout << "- ";
    }
  }
  /**
   * Pretty prints a list of elements
   * @param p Pointer to the elements
   * @param count The number of elements to be printed
   * @param start The offset from which to print
   */
  static void printElems(T* p, int count, int start = 0) {
    auto alignedStart = (start / wordsPerLine) * wordsPerLine;
    for (int x = alignedStart; x < start + count; ++x) {
      printBreak(p, x);
      if (x < start) {
        std::cout << "..... ";
      } else {
        std::cout << std::setfill('0') << std::setw(5) << p[x] << " ";
      }
    }
  }
  /**
   * Prints contents in the ptrs array at a particular stage
   * @param msg Custom message to be printed
   */
  void printStageBuffer(const std::string& msg) {
    if (printCheck(myRank_)) {
      std::cout << "rank (" << myRank_ << ") " << msg << ": ";
      printElems(&ptrs_[0][0], totalNumElems_);
      std::cout << std::endl;
    }
  }

  /**
   * Prints specified buffer during a step
   * @param step The step when the buffer is being printed
   * @param srcRank The sender of the data
   * @param destRank The receiver of data
   * @param p Poniter to the buffer to be printed
   * @param count Number of elements to be printed
   * @param start The offset from which to print
   */
  void printStepBuffer(
      const std::string& stage,
      int step,
      int srcRank,
      int destRank,
      T* p,
      int count,
      int start = 0) {
    if (printCheck(myRank_)) {
      std::cout << stage << ": step (" << step << ") " << "srcRank (" << srcRank
                << ") -> " << "destRank (" << destRank << "): ";
      printElems(p, count, start);
      std::cout << std::endl;
    }
  }
  /**
   * Get all the peers of node with specified rank
   * @param rank Rank of the node for which peers are needed
   * @param step The step for which we need to get peers
   * @return List of ranks of all peer nodes
   */
  const std::vector<int>& getPeersPerStep(int rank, int step) {
    return allNodes_[rank].getPeersPerStep(step);
  }
  /**
   * Get count of elements specified node needs to process in specified the step
   * @param rank Rank of the node for which count is requested
   * @param step The step for which we are querying count
   */
  int getNumElemsPerStep(int rank, int step) {
    return allNodes_[rank].getNumElemsPerStep(step);
  }
  /**
   * Get offset to ptrs array specified node needs to start processing from in
   * the specified step
   * @param rank Rank of the node for which offset is requested
   * @param step The step for which we are querying offset
   */
  int getPtrOffsetPerStep(int rank, int step) {
    return allNodes_[rank].getPtrOffsetPerStep(step);
  }
  /**
   * Creates all the nodes with sequential ranks
   */
  void createNodes() {
    for (int rank = 0; rank < nodes_; ++rank) {
      allNodes_.emplace_back(rank, steps_);
    }
  }
  /**
   * Updates the peer, count and offset values for all the nodes in a group
   * @param step The step for which we are updating the values
   * @param groups The group object with all peer, count and offset data
   */
  void updateGroupNodes(int step, const bcube::Group& group) {
    const std::vector<int>& peers = group.getNodeRanks();
    const int peersSz = peers.size();
    int ptrOffset = group.getPtrOffset();
    int count = group.getNumElems() / peersSz;
    const int countRem = group.getNumElems() % peersSz;
    if (0 == count) {
      count = 1;
    }
    for (int i = 0; i < peersSz; ++i) {
      bcube::Node& node = allNodes_[peers[i]];
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
  /**
   * Setup all the nodes
   * Here are the things we do in this function
   *  - Create nodes
   *  - Compute and store elements per group in each step
   *  - Step up all the nodes
   */
  void setupNodes() {
    // Create all the nodes upfront
    createNodes();

    // Now we actually try to set up the nodes
    int peerDistance = 1;
    for (int step = 0; step < steps_; ++step) {
      std::vector<bcube::Group> groups;
      // Iterate over all the nodes to identify the first node of each group
      for (int rank = 0; rank < nodes_; ++rank) {
        const bcube::Node& firstNode = allNodes_[rank];
        // Only the ones with no peers would be first node
        if (0 == firstNode.getPeersPerStep(step).size()) {
          // Create a new group
          groups.emplace_back(
              step, firstNode, peerDistance, base_, nodes_, totalNumElems_);
          // Iterrate over all the peer nodes and set them up for the step
          updateGroupNodes(step, groups.back());
        } // if (0 == firstNode ...
      } // for (int rank = 0..
      // Done iterating over all the nodes. Update peerDistance for next step.
      peerDistance *= base_;
    } // for (int step ...
  } // setupNodes
};

} // namespace gloo
