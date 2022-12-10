/**
 * @file macros.h
 * @author sheep (ysj1173886760@gmail.com)
 * @brief
 * @version 0.1
 * @date 2022-12-10
 *
 * @copyright Copyright (c) 2022
 *
 */

#pragma once

#include "logger.h"

namespace arcanedb {
namespace common {

#define DISALLOW_COPY(cname)                                                   \
  cname(const cname &) = delete;                                               \
  cname &operator=(const cname &) = delete

#define DISALLOW_MOVE(cname)                                                   \
  cname(cname &&) = delete;                                                    \
  cname &operator=(cname &&) = delete

#define DISALLOW_COPY_AND_MOVE(cname)                                          \
  DISALLOW_COPY(cname);                                                        \
  DISALLOW_MOVE(cname)

#ifdef NDEBUG
#define DCHECK_IS_ON() 0
#else
#define DCHECK_IS_ON() 1
#endif

#define FATAL(fmt, ...)                                                        \
  do {                                                                         \
    LOG_ERROR(fmt, __VA_ARGS__);                                            \
    std::abort();                                                              \
  } while (false)

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      FATAL("check failed, condition: %s", #condition);                         \
    }                                                                          \
  } while (false)

#define DCHECK(condition)                                                      \
  do {                                                                         \
    if (DCHECK_IS_ON()) {                                                      \
      CHECK(condition);                                                        \
    }                                                                          \
  } while (false)

} // namespace common
} // namespace arcanedb