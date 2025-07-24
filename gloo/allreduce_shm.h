#include "gloo/allreduce.h"

namespace gloo {
    
bool is_intra_node(const int size);
void shm(const detail::AllreduceOptionsImpl& opts);

} // namespace gloo
