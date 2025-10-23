#include "gloo/allreduce_bcube.h"

#include <string_view>

#include "gloo/common/log.h"

namespace gloo {

template <typename T>
void AllreduceBcube<T>::printElems(T* p, int count, int start) {
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

template <typename T>
void AllreduceBcube<T>::printStageBuffer(const std::string& msg) {
  if (printCheck(myRank_)) {
    GLOO_TRACE("rank ({}) {}:", myRank_, msg);
    printElems(&ptrs_[0][0], totalNumElems_);
  }
}

template <typename T>
void AllreduceBcube<T>::printStepBuffer(
    const std::string& stage,
    int step,
    int srcRank,
    int destRank,
    T* p,
    int count,
    int start) {
  if (printCheck(myRank_)) {
    GLOO_TRACE(
        "{}: step ({}) srcRank ({}) -> destRank ({})",
        stage,
        step,
        srcRank,
        destRank);
    printElems(p, count, start);
  }
}

} // namespace gloo
