#include "gloo/allreduce_shm.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <immintrin.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <thread>

namespace gloo {

namespace {

using ReductionFunction = AllreduceOptions::Func;
using CollState = AllreduceSharedMemoryData::CollState;
using Allreduceworkspace = AllreduceSharedMemoryData::AllreduceWorkspace;

constexpr int VECTOR_LENGTH_IN_BYTES = 32;

#define BUFFER0_OFFSET(current_buffer) \
  current_buffer* Allreduceworkspace::NAIVE_ALLREDUCE_THRESHOLD
#define BUFFER1_OFFSET(current_buffer)                \
  2 * Allreduceworkspace::NAIVE_ALLREDUCE_THRESHOLD + \
      current_buffer* Allreduceworkspace::MAX_BUF_SIZE

// SHM building blocks
struct SharedData {
  const char* name;
  int descriptor;
  void* bytes;
  size_t nbytes;
};

void shared_open(SharedData* data, const char* name, size_t nbytes) {
  int d = shm_open(name, O_RDWR, S_IRUSR | S_IWUSR);
  if (d != -1) {
    void* bytes = mmap(NULL, nbytes, PROT_READ | PROT_WRITE, MAP_SHARED, d, 0);
    data->name = name;
    data->descriptor = d;
    data->bytes = bytes;
    data->nbytes = nbytes;
  } else {
    if (errno != ENOENT) {
      // don't print if shm can not be found because we want to loop over from
      // caller again until the other ranks created the shm
      printf("shared_open %s failed, errno=%d\n", name, errno);
    }
    data->descriptor = -1;
  }
}

void shared_create(
    SharedData* data,
    const char* name,
    void* bytes,
    size_t nbytes) {
  int d = shm_open(name, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
  if (d != -1) {
    if (nbytes = write(d, bytes, nbytes)) {
      shared_open(data, name, nbytes);
    }
  } else {
    printf("shared_create %s failed\n", name);
  }
}

void wait_buffer_state(
    CollState state0,
    CollState state1,
    int state_group,
    std::chrono::milliseconds timeout,
    std::shared_ptr<AllreduceSharedMemoryData> shm_data) {
  // Create a new thread
  auto workspace = shm_data->workspace;
  const int rank = shm_data->rank;
  const int world_size = shm_data->world_size;

  for (int i = 0; i < world_size; i++) {
    if (i == rank) {
      continue;
    }
    volatile CollState* state_ptr = &(workspace[i]->states[state_group]);

    while (true) {
      volatile CollState cur_state = *state_ptr;
      if (cur_state == state0 || cur_state == state1) {
        break;
      }
      if (shm_data->shutdown) {
        return;
      }
    }
  }

  std::unique_lock lock(shm_data->m);
  shm_data->wait_done = true;
  lock.unlock();
  shm_data->cv.notify_one();
}

void wait_buffer_state_until_2(
    CollState state0,
    CollState state1,
    int state_group,
    std::chrono::milliseconds timeout,
    std::shared_ptr<AllreduceSharedMemoryData> shm_data) {
  shm_data->wait_done = false;
  shm_data->shutdown = false;

  // Create wait buffer thread.
  std::thread t(
      wait_buffer_state, state0, state1, state_group, timeout, shm_data);

  std::unique_lock lock(shm_data->m);
  auto done =
      shm_data->cv.wait_for(lock, timeout, [&] { return shm_data->wait_done; });
  if (!done) {
    shm_data->shutdown = true;
    t.join();
    throw ::gloo::IoException(GLOO_ERROR_MSG(
        "Timed out waiting",
        timeout.count(),
        "ms for wait buffer state operation to complete"));
  } else {
    t.join();
  }
}

void reduce_all_buffers(
    int start_elements,
    int num_elements,
    int element_size,
    int to_buffer_idx,
    int world_size,
    char* to_buffer,
    char** buffers,
    ReductionFunction fn) {
  size_t offset = start_elements * element_size;
  memcpy(to_buffer + offset, buffers[0] + offset, num_elements * element_size);
  for (int i = 1; i < world_size; i++) {
    fn(to_buffer + offset,
       to_buffer + offset,
       buffers[i] + offset,
       num_elements);
  }
}

static void parallel_memcpy(void* to, void* from, size_t n_bytes)
    __attribute__((target("avx512bw")));
static void parallel_memcpy(void* to, void* from, size_t n_bytes) {
  auto aligned_bytes = n_bytes - (n_bytes % VECTOR_LENGTH_IN_BYTES);
  // process aligned part
#pragma omp parallel for
  for (int i = 0; i < aligned_bytes; i += VECTOR_LENGTH_IN_BYTES) {
    auto val = _mm256_loadu_si256((__m256i*)((char*)from + i));
    _mm256_storeu_si256((__m256i*)((char*)to + i), val);
  }

  // process remaining part
  for (int i = aligned_bytes; i < n_bytes; i++) {
    *((char*)to + i) = *((char*)from + i);
  }
}

size_t slice_size(size_t chunk_el, int slice_idx, int world_size) {
  size_t slice_size = chunk_el / world_size;
  return slice_idx == world_size - 1 ? slice_size + (chunk_el % world_size)
                                     : slice_size;
}

char* slice_data(
    char* data_ptr,
    size_t chunk_el,
    int el_size,
    int slice_idx,
    int world_size) {
  size_t slice_size = chunk_el / world_size;
  size_t el_offset = slice_size * slice_idx;
  return data_ptr + el_offset * el_size;
}

size_t slice_el_start(size_t chunk_el, int slice_idx, int world_size) {
  size_t slice_size = chunk_el / world_size;
  return slice_size * slice_idx;
}

void symmetric_naive_all_reduce(
    char* data_ptr,
    int element_size,
    size_t chunk_size,
    size_t chunk_el,
    const detail::AllreduceOptionsImpl& opts) {
  const auto& context = opts.context;
  auto& shm_data = context->shmData;
  const int rank = shm_data->rank;
  const int world_size = shm_data->world_size;
  auto symmetric_buffer = shm_data->symmetric_buffer;
  auto workspace = shm_data->workspace;
  auto& state_idx = shm_data->state_idx;
  auto& current_buffer = shm_data->current_buffer;

  const int state_group = 0;

  CollState copy_current, copy_next;

  switch (state_idx) {
    case 0:
      copy_current = CollState::coll_allreduce_naive__copy_in_done;
      copy_next = CollState::coll_alt1_allreduce_naive__copy_in_done;
      break;
    case 1:
      copy_current = CollState::coll_alt1_allreduce_naive__copy_in_done;
      copy_next = CollState::coll_alt2_allreduce_naive__copy_in_done;
      break;
    case 2:
      copy_current = CollState::coll_alt2_allreduce_naive__copy_in_done;
      copy_next = CollState::coll_allreduce_naive__copy_in_done;
      break;
    default:
      assert(!"Should not get here.");
  }
  state_idx = (state_idx + 1) % 3;

  parallel_memcpy(symmetric_buffer[current_buffer][rank], data_ptr, chunk_size);

  std::atomic_thread_fence(std::memory_order_release);
  workspace[rank]->states[state_group] = copy_current;

  wait_buffer_state_until_2(
      copy_current, copy_next, state_group, opts.timeout, shm_data);

  // each rank reduce the buffer independently so therre is no need for
  // synchronization afterward
  reduce_all_buffers(
      0,
      chunk_el,
      element_size,
      rank,
      world_size,
      data_ptr,
      symmetric_buffer[current_buffer],
      opts.reduce);

  // switch buffer
  current_buffer = 1 - current_buffer;
}

// naive allreduce distributed, each rank do naive reduce on its slice
void distributed_naive_reduce(
    char* data_ptr,
    int element_size,
    size_t chunk_size,
    size_t chunk_el,
    const detail::AllreduceOptionsImpl& opts) {
  const auto& context = opts.context;
  auto& shm_data = context->shmData;
  const int rank = shm_data->rank;
  const int world_size = shm_data->world_size;
  auto distributed_buffer = shm_data->distributed_buffer;
  auto workspace = shm_data->workspace;
  auto& state_idx = shm_data->state_idx;
  auto& current_buffer = shm_data->current_buffer;

  const int state_group = 1;

  CollState copy_current, copy_next, reduce_current;

  // similar to symmetric_naive_allreduce, but here we only need two sets of
  // states, because distributed naive reduce has two barriers in the
  // algorithm
  switch (state_idx) {
    case 0:
      copy_current = CollState::coll_allreduce_naive__copy_in_done;
      reduce_current = CollState::coll_allreduce_naive__reduce_done;
      copy_next = CollState::coll_alt1_allreduce_naive__copy_in_done;
      break;
    case 1:
      copy_current = CollState::coll_alt1_allreduce_naive__copy_in_done;
      reduce_current = CollState::coll_alt1_allreduce_naive__reduce_done;
      copy_next = CollState::coll_allreduce_naive__copy_in_done;
      break;
    default:
      assert(!"Should not get here.");
  }
  state_idx = (state_idx + 1) % 2;

  int data_size = chunk_size / chunk_el;
  parallel_memcpy(
      distributed_buffer[current_buffer][rank], data_ptr, chunk_size);
  std::atomic_thread_fence(std::memory_order_release);
  workspace[rank]->states[state_group] = copy_current;

  wait_buffer_state_until_2(
      copy_current, reduce_current, state_group, opts.timeout, shm_data);

  // reduce scatter
  reduce_all_buffers(
      slice_el_start(chunk_el, rank, world_size),
      slice_size(chunk_el, rank, world_size),
      element_size,
      rank,
      world_size,
      distributed_buffer[current_buffer][rank],
      distributed_buffer[current_buffer],
      opts.reduce);
  std::atomic_thread_fence(std::memory_order_release);
  workspace[rank]->states[state_group] = reduce_current;

  wait_buffer_state_until_2(
      copy_current, reduce_current, state_group, opts.timeout, shm_data);

  for (int i = 0; i < world_size; i++) {
    int rank = (i + rank) % world_size;
    parallel_memcpy(
        slice_data(data_ptr, chunk_el, data_size, rank, world_size),
        slice_data(
            distributed_buffer[current_buffer][rank],
            chunk_el,
            chunk_size / chunk_el,
            rank,
            world_size),
        slice_size(chunk_el, rank, world_size) * data_size);
  }

  current_buffer = 1 - current_buffer;
}

} // namespace

void AllreduceSharedMemoryData::initialize() {
  std::string addr_string(""), port_string("");
  const auto& addr_string_env = std::getenv("MASTER_ADDR");
  if (addr_string_env != nullptr) {
    addr_string = addr_string_env;
  }
  const auto port_string_env = std::getenv("MASTER_PORT");
  if (port_string_env != NULL) {
    port_string = port_string_env;
  }

  char shm_name_prefix[Allreduceworkspace::NAME_BUF_SIZE];
  char shm_name[Allreduceworkspace::NAME_BUF_SIZE];
  snprintf(
      shm_name_prefix,
      Allreduceworkspace::NAME_BUF_SIZE,
      "%s_%d_%s_%s",
      "shm_allreduce_buffer",
      getuid(),
      addr_string.c_str(),
      port_string.c_str());
  // create shared workspace for SHM based allreduce
  // allocate workspace_buf for current rank
  AllreduceWorkspace* workspace_buf;
  AllreduceWorkspace* workspace_buf_other;
  SharedData allreduce_buffer;
  cur_workspace = (AllreduceWorkspace*)malloc(sizeof(AllreduceWorkspace));
  workspace_buf = cur_workspace;

  int written = snprintf(
      shm_name,
      AllreduceWorkspace::NAME_BUF_SIZE,
      "%s_%d",
      shm_name_prefix,
      rank);
  if (written >= AllreduceWorkspace::NAME_BUF_SIZE) {
    std::cout << "[warning]: written >= NAME_BUF_SIZE" << std::endl;
  }

  shared_create(
      &allreduce_buffer, shm_name, workspace_buf, sizeof(AllreduceWorkspace));

  workspace_buf = (AllreduceWorkspace*)allreduce_buffer.bytes;
  workspace_buf->states[0] = coll_alt2_allreduce_naive__copy_in_done;
  workspace_buf->states[1] = coll_begin;
  workspace_buf->fd = allreduce_buffer.descriptor;
  strcpy(workspace_buf->name, shm_name);

  // create the workspace pointer list
  workspace =
      (AllreduceWorkspace**)malloc(world_size * sizeof(Allreduceworkspace*));
  symmetric_buffer[0] = (char**)malloc(world_size * sizeof(char**));
  symmetric_buffer[1] = (char**)malloc(world_size * sizeof(char**));
  distributed_buffer[0] = (char**)malloc(world_size * sizeof(char**));
  distributed_buffer[1] = (char**)malloc(world_size * sizeof(char**));

  // map shm of all ranks
  for (int i = 0; i < world_size; i++) {
    if (i != rank) {
      int written = snprintf(
          shm_name,
          AllreduceWorkspace::NAME_BUF_SIZE,
          "%s_%d",
          shm_name_prefix,
          i);
      if (written >= AllreduceWorkspace::NAME_BUF_SIZE) {
        std::cout << "[warning]: written >= NAME_BUF_SIZE" << std::endl;
      }

      do {
        shared_open(&allreduce_buffer, shm_name, sizeof(AllreduceWorkspace));
      } while (allreduce_buffer.descriptor == -1 && errno == ENOENT);
      workspace_buf_other = (AllreduceWorkspace*)allreduce_buffer.bytes;
      workspace[i] = workspace_buf_other;
    } else {
      workspace[i] = workspace_buf;
    }
    symmetric_buffer[0][i] = workspace[i]->buffer + BUFFER0_OFFSET(0);
    symmetric_buffer[1][i] = workspace[i]->buffer + BUFFER0_OFFSET(1);
    distributed_buffer[0][i] = workspace[i]->buffer + BUFFER1_OFFSET(0);
    distributed_buffer[1][i] = workspace[i]->buffer + BUFFER1_OFFSET(1);
  }
  is_initialized = true;
}

AllreduceSharedMemoryData::~AllreduceSharedMemoryData() {
  if (is_initialized == true) {
    // unlink and munmap shared memory
    for (int i = 0; i < world_size; i++) {
      std::string shm_name = std::string(workspace[i]->name);
      close(workspace[i]->fd);
      munmap(workspace[i], sizeof(Allreduceworkspace));
      shm_unlink(shm_name.c_str());
    }

    free(cur_workspace);
    free(workspace);
    free(symmetric_buffer[0]);
    free(symmetric_buffer[1]);
    free(distributed_buffer[0]);
    free(distributed_buffer[1]);
  }
}

void shm(const detail::AllreduceOptionsImpl& opts) {
  const auto& context = opts.context;
  if (context->shmData == nullptr) {
    context->shmData = std::make_shared<AllreduceSharedMemoryData>(
        context->rank, context->size);
    context->shmData->initialize();
  }
  const size_t data_size = opts.elements * opts.elementSize;
  auto& in = opts.in;
  auto& out = opts.out;

  // Do local reduction
  if (in.size() > 0) {
    if (in.size() == 1) {
      memcpy(
          static_cast<uint8_t*>(out[0]->ptr),
          static_cast<uint8_t*>(in[0]->ptr),
          data_size);
    } else {
      opts.reduce(
          static_cast<uint8_t*>(out[0]->ptr),
          static_cast<const uint8_t*>(in[0]->ptr),
          static_cast<const uint8_t*>(in[1]->ptr),
          opts.elements);
      for (size_t i = 2; i < in.size(); i++) {
        opts.reduce(
            static_cast<uint8_t*>(out[0]->ptr),
            static_cast<const uint8_t*>(out[0]->ptr),
            static_cast<const uint8_t*>(in[i]->ptr),
            opts.elements);
      }
    }
  } else {
    for (size_t i = 1; i < out.size(); i++) {
      opts.reduce(
          static_cast<uint8_t*>(out[0]->ptr),
          static_cast<const uint8_t*>(out[0]->ptr),
          static_cast<const uint8_t*>(out[i]->ptr),
          opts.elements);
    }
  }

  void* data = out[0].get()->ptr;

  for (int offset = 0; offset < data_size;
       offset += Allreduceworkspace::MAX_BUF_SIZE) {
    auto data_ptr = ((char*)(data) + offset);
    size_t chunk_size = data_size - offset > Allreduceworkspace::MAX_BUF_SIZE
        ? Allreduceworkspace::MAX_BUF_SIZE
        : data_size - offset;
    size_t chunk_el = chunk_size / (data_size / opts.elements);
    if (chunk_size < Allreduceworkspace::NAIVE_ALLREDUCE_THRESHOLD) {
      symmetric_naive_all_reduce(
          data_ptr, opts.elementSize, chunk_size, chunk_el, opts);
    } else {
      distributed_naive_reduce(
          data_ptr, opts.elementSize, chunk_size, chunk_el, opts);
    }
  }

  if (out.size() > 1) {
    for (size_t i = 1; i < out.size(); i++) {
      memcpy(
          static_cast<uint8_t*>(out[i]->ptr),
          static_cast<uint8_t*>(out[0]->ptr),
          data_size);
    }
  }
}

} // namespace gloo
