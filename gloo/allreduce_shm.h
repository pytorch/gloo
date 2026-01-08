
#pragma once

#include <condition_variable>
#include <mutex>

#include "gloo/allreduce.h"

namespace gloo {

struct AllreduceSharedMemoryData {
  enum CollState {
    coll_begin = 0,
    coll_allreduce_naive__copy_in_done,
    coll_allreduce_naive__reduce_done,
    // alternative state when allreduce is working on alternative buffer
    // of the double buffer.
    coll_alt1_allreduce_naive__copy_in_done,
    coll_alt2_allreduce_naive__copy_in_done,
    coll_alt1_allreduce_naive__reduce_done,
  };

  struct AllreduceWorkspace {
    static constexpr size_t MAX_BUF_SIZE = 1048576 * 32;
    static constexpr size_t NAIVE_ALLREDUCE_THRESHOLD = 1048576;
    static constexpr int NAME_BUF_SIZE = 1000;

    int fd;
    enum CollState states[2]; // idx=0 -- state for symmetric_naive_all_reduce
                              // idx=1 -- state for distributed_naive_all_reduce
    // double buffer to avoid syncing between rounds
    // offset=0 -- 2*NAIVE_ALLREDUCE_THRESHOLD : buffer for
    // symmetric_naive_all_reduce after that : buffer for
    // distributed_naive_all_reduce
    char name[NAME_BUF_SIZE];
    char buffer[2 * NAIVE_ALLREDUCE_THRESHOLD + 2 * MAX_BUF_SIZE];
  };

  AllreduceSharedMemoryData(int rank, int world_size)
      : rank(rank),
        world_size(world_size),
        current_buffer(0),
        state_idx(0),
        is_initialized(false) {}
  ~AllreduceSharedMemoryData();
  void initialize();

  int rank;
  int world_size;
  int current_buffer;
  int state_idx;
  bool is_initialized;

  AllreduceWorkspace* cur_workspace;
  AllreduceWorkspace** workspace;
  // buffer for small messages, double buffer
  char** symmetric_buffer[2];
  // buffer for large messages, double buffer
  char** distributed_buffer[2];
  std::vector<int> shm_fd;
  std::string shm_buffer_name;

  std::mutex m;
  std::condition_variable cv;
  bool wait_done;
  bool shutdown;
};

void shm(const detail::AllreduceOptionsImpl& opts);

} // namespace gloo
