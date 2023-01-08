/**
 * @file schema.h
 * @author sheep (ysj1173886760@gmail.com)
 * @brief
 * @version 0.1
 * @date 2023-01-07
 *
 * @copyright Copyright (c) 2023
 *
 */

#pragma once

#include <string>

#include "absl/container/flat_hash_map.h"
#include "absl/container/inlined_vector.h"
#include "property/property_type.h"

namespace arcanedb {
namespace property {

class Schema {
public:
  Schema(const RawSchema &raw_schema) noexcept;

  Column *GetColumnRefById(ColumnId column_id) noexcept;

  Column *GetColumnRefByIndex(size_t index) noexcept;

  SchemaId GetSchemaId() noexcept { return schema_id_; }

  size_t GetColumnNum() noexcept { return columns_.size(); }

private:
  void BuildColumnIndex_() noexcept;

  absl::InlinedVector<Column, kDefaultColumnNum> columns_;
  SchemaId schema_id_;
  // mapping from column id to column index
  absl::flat_hash_map<ColumnId, size_t> column_index_;
};

} // namespace property
} // namespace arcanedb