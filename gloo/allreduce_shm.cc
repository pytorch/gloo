#include "gloo/allreduce_shm.h"

#include <errno.h>
#include <fcntl.h>
#include <immintrin.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <assert.h>


namespace gloo {

namespace {

using ReductionFunction = AllreduceOptions::Func;

#define VECTOR_LENGTH_IN_BYTES 32
// states for collectives
enum coll_state {
  coll_begin = 0,
  coll_allreduce_naive__copy_in_done,
  coll_allreduce_naive__reduce_done,
  // alternative state when allreduce is working on alternative buffer
  // of the double buffer.
  coll_alt1_allreduce_naive__copy_in_done,
  coll_alt2_allreduce_naive__copy_in_done,
  coll_alt1_allreduce_naive__reduce_done,
};

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

static int world_rank = -1;
static int world_size = -1;
static bool is_initialized = false;

// SHM based allreduce helper functions
// buffer that holds shm name
#define NAME_BUF_SIZE 1000
#define MAX_BUF_SIZE 1048576 * 32
#define NAIVE_ALLREDUCE_THRESHOLD 1048576
#define SHM_BUFFER_NAME "deepspeed_allreduce_buffer"
struct allreduce_workspace {
  enum coll_state states[2]; // idx=0 -- state for symmetric_naive_all_reduce
                             // idx=1 -- state for distributed_naive_all_reduce
  // double buffer to avoid syncing between rounds
  // offset=0 -- 2*NAIVE_ALLREDUCE_THRESHOLD : buffer for
  // symmetric_naive_all_reduce after that : buffer for
  // distributed_naive_all_reduce
  char buffer[2 * NAIVE_ALLREDUCE_THRESHOLD + 2 * MAX_BUF_SIZE];
};

#define BUFFER0_OFFSET(current_buffer) current_buffer* NAIVE_ALLREDUCE_THRESHOLD
#define BUFFER1_OFFSET(current_buffer) \
  2 * NAIVE_ALLREDUCE_THRESHOLD + current_buffer* MAX_BUF_SIZE

struct allreduce_workspace** workspace;

// buffer for small messages, double buffer
char** symmetric_buffer[2];
// buffer for large messages, double buffer
char** distributed_buffer[2];

void wait_buffer_state_until_2(
    int index,
    enum coll_state state0,
    enum coll_state state1,
    int state_group) {
  volatile enum coll_state* state_ptr =
      &(workspace[index]->states[state_group]);

  while (1) {
    volatile enum coll_state cur_state = *state_ptr;
    if (cur_state == state0 || cur_state == state1)
      break;
  }
}

void reduce_all_buffers(
    int start_elements,
    int num_elements,
    int element_size,
    int to_buffer_idx,
    char* to_buffer,
    char** buffers,
    ReductionFunction fn) {
  const int vector_length = VECTOR_LENGTH_IN_BYTES / element_size;
  int main_elements = num_elements - (num_elements % vector_length);
  int remain_elements = num_elements % vector_length;
 
#pragma omp parallel for
  for (int i = start_elements * element_size;
       i < (start_elements + main_elements) * element_size;
       i += VECTOR_LENGTH_IN_BYTES) {
        memcpy(to_buffer + i, buffers[0] + i, element_size);
        switch (world_size){
            case 16: fn(to_buffer + i,  to_buffer + i, buffers[15] + i, vector_length);
            case 15: fn(to_buffer + i,  to_buffer + i, buffers[14] + i, vector_length);
            case 14: fn(to_buffer + i,  to_buffer + i, buffers[13] + i, vector_length);
            case 13: fn(to_buffer + i,  to_buffer + i, buffers[12] + i, vector_length);
            case 12: fn(to_buffer + i,  to_buffer + i, buffers[11] + i, vector_length);
            case 11: fn(to_buffer + i,  to_buffer + i, buffers[10] + i, vector_length);
            case 10: fn(to_buffer + i,  to_buffer + i, buffers[9] + i, vector_length);
            case 9: fn(to_buffer + i,  to_buffer + i, buffers[8] + i, vector_length);
            case 8: fn(to_buffer + i,  to_buffer + i, buffers[7] + i, vector_length);
            case 7: fn(to_buffer + i,  to_buffer + i, buffers[6] + i, vector_length);
            case 6: fn(to_buffer + i,  to_buffer + i, buffers[5] + i, vector_length);
            case 5: fn(to_buffer + i,  to_buffer + i, buffers[4] + i, vector_length);
            case 4: fn(to_buffer + i,  to_buffer + i, buffers[3] + i, vector_length);
            case 3: fn(to_buffer + i,  to_buffer + i, buffers[2] + i, vector_length);
            case 2: fn(to_buffer + i,  to_buffer + i, buffers[1] + i, vector_length);
            case 1: break;
            default:
                for (int j = 1; j < world_size; j++) {
                    fn(to_buffer + i,  to_buffer + i, buffers[j] + i, vector_length);
                }
        }
        }

  size_t offset = (start_elements + main_elements) * element_size;
  while (remain_elements > 0) {
    memcpy(to_buffer + offset, buffers[0] + offset, element_size);
    for (int j = 1; j < world_size; j++) {
      
      fn(to_buffer + offset,
        to_buffer + offset,
        buffers[j] + offset,
        1);
      
    }
    remain_elements--;
    offset += element_size;
  }
    
}

void shm_initialize(int size, int rank, char* addr_string, char* port_string) {
  world_size = size;
  world_rank = rank;

  char shm_name_prefix[NAME_BUF_SIZE];
  char shm_name[NAME_BUF_SIZE];
  snprintf(
      shm_name_prefix,
      NAME_BUF_SIZE,
      "%s_%d_%s_%s",
      SHM_BUFFER_NAME,
      getuid(),
      addr_string,
      port_string);
  // create shared workspace for SHM based allreduce
  SharedData allreduce_buffer;
  // allocate workspace_buf for current rank
  struct allreduce_workspace* workspace_buf;
  struct allreduce_workspace* workspace_buf_other;
  workspace_buf =
      (struct allreduce_workspace*)malloc(sizeof(struct allreduce_workspace));
  int written = snprintf(shm_name, NAME_BUF_SIZE, "%s_%d", shm_name_prefix, rank);
  if (written >= NAME_BUF_SIZE) {
    std::cout << "[warning]: written >= NAME_BUF_SIZE" << std::endl;
  }
  shared_create(
      &allreduce_buffer,
      shm_name,
      workspace_buf,
      sizeof(struct allreduce_workspace));
  workspace_buf = (struct allreduce_workspace*)allreduce_buffer.bytes;
  workspace_buf->states[0] = coll_alt2_allreduce_naive__copy_in_done;
  workspace_buf->states[1] = coll_begin;

  // create the workspace pointer list
  workspace = (struct allreduce_workspace**)malloc(
      size * sizeof(struct allreduce_workspace*));
  symmetric_buffer[0] = (char**)malloc(size * sizeof(char**));
  symmetric_buffer[1] = (char**)malloc(size * sizeof(char**));
  distributed_buffer[0] = (char**)malloc(size * sizeof(char**));
  distributed_buffer[1] = (char**)malloc(size * sizeof(char**));

  // map shm of all ranks
  for (int i = 0; i < size; i++) {
    if (i != rank) {
        int written = snprintf(shm_name, NAME_BUF_SIZE, "%s_%d", shm_name_prefix, i);
        if (written >= NAME_BUF_SIZE) {
          std::cout << "[warning]: written >= NAME_BUF_SIZE" << std::endl;
        }
      // printf("open %s, %d\n", shm_name, rank);
      do {
        shared_open(
            &allreduce_buffer, shm_name, sizeof(struct allreduce_workspace));
      } while (allreduce_buffer.descriptor == -1 && errno == ENOENT);
      workspace_buf_other = (struct allreduce_workspace*)allreduce_buffer.bytes;
      workspace[i] = workspace_buf_other;
    } else {
      workspace[i] = workspace_buf;
    }
    symmetric_buffer[0][i] = workspace[i]->buffer + BUFFER0_OFFSET(0);
    symmetric_buffer[1][i] = workspace[i]->buffer + BUFFER0_OFFSET(1);
    distributed_buffer[0][i] = workspace[i]->buffer + BUFFER1_OFFSET(0);
    distributed_buffer[1][i] = workspace[i]->buffer + BUFFER1_OFFSET(1);
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

#define positive_mod(num, mod) ((((num) % (mod)) + (mod)) % (mod))
#define rank_mod(rank) positive_mod(rank, world_size)
size_t slice_size(size_t chunk_el, int slice_idx) {
  size_t slice_size = chunk_el / world_size;
  return slice_idx == world_size - 1 ? slice_size + (chunk_el % world_size)
                                     : slice_size;
}

char* slice_data(char* data_ptr, size_t chunk_el, int el_size, int slice_idx) {
  size_t slice_size = chunk_el / world_size;
  size_t el_offset = slice_size * slice_idx;
  return data_ptr + el_offset * el_size;
}

size_t slice_el_start(size_t chunk_el, int slice_idx) {
  size_t slice_size = chunk_el / world_size;
  return slice_size * slice_idx;
}

void symmetric_naive_all_reduce(
    char* data_ptr,
    int element_size,
    size_t chunk_size,
    size_t chunk_el,
    ReductionFunction fn) {
  const int state_group = 0;
  static int current_buffer = 0;
  static int state_idx = 0;

  enum coll_state copy_current, copy_next;

  switch (state_idx) {
    case 0:
      copy_current = coll_allreduce_naive__copy_in_done;
      copy_next = coll_alt1_allreduce_naive__copy_in_done;
      break;
    case 1:
      copy_current = coll_alt1_allreduce_naive__copy_in_done;
      copy_next = coll_alt2_allreduce_naive__copy_in_done;
      break;
    case 2:
      copy_current = coll_alt2_allreduce_naive__copy_in_done;
      copy_next = coll_allreduce_naive__copy_in_done;
      break;
    default:
      assert(!"Should not get here.");
  }
  state_idx = (state_idx + 1) % 3;

  parallel_memcpy(
      symmetric_buffer[current_buffer][world_rank], data_ptr, chunk_size);
  std::atomic_thread_fence(std::memory_order_release);
  workspace[world_rank]->states[state_group] = copy_current;

  for (int i = 0; i < world_size; i++) {
    // wait until the other rank copy the buffer
    if (i != world_rank) {
      wait_buffer_state_until_2(i, copy_current, copy_next, state_group);
    }
  }

  // each rank reduce the buffer independently so therre is no need for
  // synchronization afterward
  reduce_all_buffers(
      0,
      chunk_el,
      element_size,
      world_rank,
      data_ptr,
      symmetric_buffer[current_buffer],
      fn);

  // switch buffer
  current_buffer = 1 - current_buffer;
}

// naive allreduce distributed, each rank do naive reduce on its slice
void distributed_naive_reduce(
    char* data_ptr,
    int element_size,
    size_t chunk_size,
    size_t chunk_el,
    ReductionFunction fn) {
  const int state_group = 1;
  static int current_buffer = 0;
  static int state_idx = 0;

  enum coll_state copy_current, copy_next, reduce_current;

  // similar to symmetric_naive_allreduce, but here we only need two sets of
  // states, because distributed naive reduce has two barriers in the algorithm
  switch (state_idx) {
    case 0:
      copy_current = coll_allreduce_naive__copy_in_done;
      reduce_current = coll_allreduce_naive__reduce_done;
      copy_next = coll_alt1_allreduce_naive__copy_in_done;
      break;
    case 1:
      copy_current = coll_alt1_allreduce_naive__copy_in_done;
      reduce_current = coll_alt1_allreduce_naive__reduce_done;
      copy_next = coll_allreduce_naive__copy_in_done;
      break;
    default:
      assert(!"Should not get here.");
  }
  state_idx = (state_idx + 1) % 2;

  int data_size = chunk_size / chunk_el;
  parallel_memcpy(
      distributed_buffer[current_buffer][world_rank], data_ptr, chunk_size);
  std::atomic_thread_fence(std::memory_order_release);
  workspace[world_rank]->states[state_group] = copy_current;

  for (int i = 0; i < world_size; i++) {
    // wait until all the other ranks copy the buffer
    if (i != world_rank)
      wait_buffer_state_until_2(i, copy_current, reduce_current, state_group);
  }

  // reduce scatter
  reduce_all_buffers(
      slice_el_start(chunk_el, world_rank),
      slice_size(chunk_el, world_rank),
      element_size,
      world_rank,
      distributed_buffer[current_buffer][world_rank],
      distributed_buffer[current_buffer],
      fn);
  std::atomic_thread_fence(std::memory_order_release);
  workspace[world_rank]->states[state_group] = reduce_current;

  for (int i = 0; i < world_size; i++) {
    // wait until all the other ranks reduce the buffer
    if (i != world_rank)
      wait_buffer_state_until_2(i, reduce_current, copy_next, state_group);
  }

  for (int i = 0; i < world_size; i++) {
    int rank = (i + world_rank) % world_size;
    parallel_memcpy(
        slice_data(data_ptr, chunk_el, data_size, rank),
        slice_data(
            distributed_buffer[current_buffer][rank],
            chunk_el,
            chunk_size / chunk_el,
            rank),
        slice_size(chunk_el, rank) * data_size);
  }

  current_buffer = 1 - current_buffer;
}

} // namespace

bool is_intra_node(const int size) {
    // must launch with torchrun
  auto local_size_string = std::getenv("LOCAL_WORLD_SIZE");
  int local_size = 0;
  if (local_size_string != NULL) {
    local_size = std::stoi(local_size_string);
  }

  return size > 1 && size == local_size;   
}


void shm(const detail::AllreduceOptionsImpl& opts) {

    const auto& context = opts.context;
  if (!is_initialized) {

    //int size = context->size;
    //int rank = context->rank;

    int size = std::stoi(std::getenv("PMI_SIZE"));
    int rank = std::stoi(std::getenv("PMI_RANK"));

    world_size = size;
    world_rank = rank;
    is_initialized = true;

    auto addr_string = std::getenv("MASTER_ADDR");
    if (addr_string == NULL) {
        addr_string = "";
    }
    auto port_string = std::getenv("MASTER_PORT");
    if (port_string == NULL) {
        port_string = "";
    }
    // std::cout << "size: " << size << std::endl;
    // std::cout << "rank: " << rank << std::endl;
    // std::cout << "addr_string: " << addr_string << std::endl;
    // std::cout << "port_string: " << port_string << std::endl;
    shm_initialize(size, rank, addr_string, port_string);
    GPF_PRINT("SHM reduce has been initialized");
  }

  const size_t data_size = opts.elements * opts.elementSize;
  const std::vector<std::unique_ptr<transport::UnboundBuffer>>& out = opts.out;
  void* data = out[0].get()->ptr;

    for (int offset = 0; offset < data_size; offset += MAX_BUF_SIZE) {
        auto data_ptr = ((char*)(data) + offset);
        size_t chunk_size =
            data_size - offset > MAX_BUF_SIZE ? MAX_BUF_SIZE : data_size - offset;
        size_t chunk_el = chunk_size / (data_size / opts.elements);
        if (chunk_size < NAIVE_ALLREDUCE_THRESHOLD) {
        symmetric_naive_all_reduce(
            data_ptr, opts.elementSize, chunk_size, chunk_el, opts.reduce);
        } else {
        distributed_naive_reduce(
            data_ptr, opts.elementSize, chunk_size, chunk_el, opts.reduce);
        }
  }

}

} //namespace gloo

