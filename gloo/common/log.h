#pragma once

/**
 * @file log.h
 * @brief This header is intended to be internal only, so as not to make spdlog
 * a dependency of gloo library consumers. For this reason, it should not be
 * included by public headers.
 */

/* The compile-time log level is set to the most sensitive (TRACE), to ensure no
 * logging statements are removed at build time. This allows logging to be fully
 * controllable at runtime, even to the TRACE level. */
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE

#include <spdlog/cfg/env.h>
#include <spdlog/spdlog.h>

/* ranges.h provides formatter for containers, e.g. std::vector */
#include <spdlog/fmt/ranges.h>

namespace gloo::log {

inline void init() {
  /* Default to WARN log level */
  spdlog::set_level(spdlog::level::warn);
  /* Override log level if the environment variable is set. */
  spdlog::cfg::load_env_levels("GLOO_LOG_LEVEL");

  /* Set custom format. This is similiar to PyTorch's format. Equivalent to:
   * [{short-log-level}{month-num}{day-of-month} {hour}:{min}:{sec}.{microsec}
   * {threadid} {source_file}:{line_no}] {MESSAGE} */
  spdlog::set_pattern("[%L%m%d %H:%M:%S.%f %t %s:%#] %v");
}

inline std::once_flag onceFlag;

inline void ensureInit() {
  std::call_once(onceFlag, [] { init(); });
}

} // namespace gloo::log

#define GLOO_TRACE(...)        \
  do {                         \
    ::gloo::log::ensureInit(); \
    SPDLOG_TRACE(__VA_ARGS__); \
  } while (0)
#define GLOO_DEBUG(...)        \
  do {                         \
    ::gloo::log::ensureInit(); \
    SPDLOG_DEBUG(__VA_ARGS__); \
  } while (0)
#define GLOO_INFO(...)         \
  do {                         \
    ::gloo::log::ensureInit(); \
    SPDLOG_INFO(__VA_ARGS__);  \
  } while (0)
#define GLOO_WARN(...)         \
  do {                         \
    ::gloo::log::ensureInit(); \
    SPDLOG_WARN(__VA_ARGS__);  \
  } while (0)
#define GLOO_ERROR(...)        \
  do {                         \
    ::gloo::log::ensureInit(); \
    SPDLOG_ERROR(__VA_ARGS__); \
  } while (0)
#define GLOO_CRITICAL(...)        \
  do {                            \
    ::gloo::log::ensureInit();    \
    SPDLOG_CRITICAL(__VA_ARGS__); \
  } while (0)
