/**
 * @file options.h
 * @author sheep (ysj1173886760@gmail.com)
 * @brief
 * @version 0.1
 * @date 2023-01-19
 *
 * @copyright Copyright (c) 2023
 *
 */

#pragma once

#include "cache/buffer_pool.h"
#include "property/schema.h"

namespace arcanedb {

/**
 * @brief
 * Options for reading and writing btree.
 */
struct Options {
  const property::Schema *schema{};
  bool disable_compaction{false};
  bool ignore_lock{false};
  cache::BufferPool *buffer_pool{};
};

} // namespace arcanedb