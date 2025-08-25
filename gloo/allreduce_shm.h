#include "gloo/allreduce.h"

namespace gloo {
    
bool is_intra_node(const detail::AllreduceOptionsImpl& opts);
void shm(const detail::AllreduceOptionsImpl& opts);

} // namespace gloo
