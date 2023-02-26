/**
 * @file txn_context_occ.cpp
 * @author sheep (ysj1173886760@gmail.com)
 * @brief
 * @version 0.1
 * @date 2023-01-26
 *
 * @copyright Copyright (c) 2023
 *
 */

#include "txn/txn_context_occ.h"
#include "absl/cleanup/cleanup.h"
#include "btree/write_info.h"
#include "txn/txn_manager_occ.h"
#include "wal/occ_log.h"
#include <optional>

namespace arcanedb {
namespace txn {

Status TxnContextOCC::SetRow(const std::string &sub_table_key,
                             const property::Row &row,
                             const Options &opts) noexcept {
  auto s = AcquireLock_(sub_table_key, row.GetSortKeys().as_slice(), opts);
  if (unlikely(!s.ok())) {
    return s;
  }
  auto owner = std::make_unique<std::string>(row.as_slice());
  property::Row new_row(owner->data());
  // new write will overwrite old writes
  write_set_[{sub_table_key, new_row.GetSortKeys()}] = new_row;
  row_owners_.insert(std::move(owner));
  return Status::Ok();
}

Status TxnContextOCC::DeleteRow(const std::string &sub_table_key,
                                property::SortKeysRef sort_key,
                                const Options &opts) noexcept {
  auto s = AcquireLock_(sub_table_key, sort_key.as_slice(), opts);
  if (unlikely(!s.ok())) {
    return s;
  }
  auto owner = std::make_unique<std::string>(sort_key.as_slice());
  write_set_[{sub_table_key, property::SortKeysRef(*owner)}] = std::nullopt;
  row_owners_.insert(std::move(owner));
  return Status::Ok();
}

Status TxnContextOCC::GetRow(const std::string &sub_table_key,
                             property::SortKeysRef sort_key,
                             const Options &opts,
                             btree::RowView *view) noexcept {
  // try read
  auto sub_table = GetSubTable_(sub_table_key, opts);
  if (txn_type_ == TxnType::ReadOnlyTxn) {
    return sub_table->GetRow(sort_key, read_ts_, opts, view);
  }
  // sheep: hope this won't incur memory allocation
  // first check the write_set_
  auto it = write_set_.find({sub_table_key, sort_key});
  if (it != write_set_.end()) {
    if (!it->second.has_value()) {
      return Status::NotFound();
    }
    // TODO(sheep): amend interface here.
    // row is passed out without ownership
    view->PushBackRef(it->second.value());
    return Status::Ok();
  }
  // perform read
  // read set will only record the ts we read on real table
  // instead of write cache.
  auto s = sub_table->GetRow(sort_key, read_ts_, opts, view);
  if (s.IsNotFound()) {
    // TODO(sheep): check consistency here
    // should check the consistency with previous read.
    // and abort early.
    read_set_[{sub_table_key, sort_key.deref()}] = std::nullopt;
    return s;
  }
  // remember the read ts
  read_set_[{sub_table_key, sort_key.deref()}] = view->at(0).GetTs();
  return s;
}

Status TxnContextOCC::CommitOrAbort(const Options &opts) noexcept {
  if (txn_type_ == TxnType::ReadOnlyTxn) {
    return Status::Commit();
  }
  auto defer = absl::MakeCleanup([&]() { ReleaseLock_(opts); });
  // commit protocol:
  // 1. write all intents
  // 2. acquire commit ts
  // 3. validate read set
  // 4. commit the intents
  Status s;
  {
    Options write_intent_opts = opts;
    write_intent_opts.check_intent_locked =
        lock_manager_type_ == LockManagerType::kInlined;
    s = WriteIntents_(write_intent_opts);
  }
  if (!s.ok()) {
    ARCANEDB_INFO("Txn id: {} read ts: {}, Failed to commit {}", txn_id_,
                  read_ts_, s.ToString());
    return Status::Abort();
  }
  commit_ts_ = txn_manager_->RequestTs();
  if (!ValidateRead_(opts)) {
    ARCANEDB_INFO(
        "Txn id: {} read ts: {}, commit ts: {}, Read validation failed.",
        txn_id_, read_ts_, commit_ts_);
    AbortIntents_(opts);
    return Status::Abort();
  }
  CommitIntents_(opts);

  // commit ts
  txn_manager_->Commit(this);
  return Status::Commit();
}

Status TxnContextOCC::WriteIntents_(const Options &opts) noexcept {
  // iterate write sets
  btree::WriteInfo info;
  std::vector<std::pair<std::string_view, property::SortKeysRef>> undo_list;
  for (const auto &[k, v] : write_set_) {
    auto sub_table = GetSubTable_(k.first, opts);
    Status s;
    if (v.has_value()) {
      s = sub_table->SetRow(v.value(), MarkLocked(read_ts_), opts, &info);
    } else {
      s = sub_table->DeleteRow(k.second, MarkLocked(read_ts_), opts, &info);
    }
    if (!s.ok()) {
      // abort the intents.
      for (const auto &entry : undo_list) {
        auto sub_table = GetSubTable_(entry.first, opts);
        sub_table->SetTs(entry.second, kAbortedTxnTs, opts, &info);
      }
      return s;
    }
    undo_list.push_back({k.first, k.second});
  }
  return Status::Ok();
}

bool TxnContextOCC::ValidateRead_(const Options &opts) noexcept {
  Options tmp_opt = opts;
  tmp_opt.owner_ts = read_ts_;
  for (const auto &[k, v] : read_set_) {
    // since read set will only record the ts we read on real table
    // instead of write cache.
    // so we can only skip the intent that is written by ourself.
    auto sub_table = GetSubTable_(k.first, opts);
    btree::RowView view;
    auto s = sub_table->GetRow(k.second.as_ref(), commit_ts_, tmp_opt, &view);
    if (v.has_value()) {
      if (!s.ok()) {
        ARCANEDB_INFO("Expect value, get {}", s.ok());
        return false;
      }
      if (view.at(0).GetTs() != v.value()) {
        ARCANEDB_INFO("Expect ts {}, get {}", v.value(), view.at(0).GetTs());
        return false;
      }
    } else {
      if (!s.IsNotFound()) {
        ARCANEDB_INFO("Expect not found, get {}", s.ok());
        return false;
      }
    }
  }
  return true;
}

void TxnContextOCC::CommitIntents_(const Options &opts) noexcept {
  btree::WriteInfo info;
  for (const auto &[k, v] : write_set_) {
    auto sub_table = GetSubTable_(k.first, opts);
    sub_table->SetTs(k.second, commit_ts_, opts, &info);
  }
}

void TxnContextOCC::AbortIntents_(const Options &opts) noexcept {
  btree::WriteInfo info;
  for (const auto &[k, v] : write_set_) {
    auto sub_table = GetSubTable_(k.first, opts);
    sub_table->SetTs(k.second, kAbortedTxnTs, opts, &info);
  }
}

std::string_view ExtraceSubTableKey(const std::string_view &lock_key) noexcept {
  return lock_key.substr(0, lock_key.find("#"));
}

void TxnContextOCC::ReleaseLock_(const Options &opts) noexcept {
  switch (lock_manager_type_) {
  case LockManagerType::kCentralized: {
    for (const auto &lock : lock_set_) {
      lock_table_->Unlock(lock, read_ts_);
    }
    break;
  }
  case LockManagerType::kDecentralized: {
    for (const auto &lock : lock_set_) {
      auto sub_table = GetSubTable_(ExtraceSubTableKey(lock), opts);
      sub_table->GetLockTable().Unlock(lock, read_ts_);
    }
    break;
  }
  case LockManagerType::kInlined: {
    break;
  }
  default:
    UNREACHABLE();
  }
}

Status TxnContextOCC::AcquireLock_(const std::string &sub_table_key,
                                   std::string_view sort_key,
                                   const Options &opts) noexcept {
  if (lock_manager_type_ == LockManagerType::kInlined) {
    return Status::Ok();
  }

  // concat the subtable key and sortkey here.
  // user's subtable key and sortkey couldn't contains #
  // since it is used as delimiter here.
  std::string lock_key = sub_table_key;
  lock_key.append("#");
  lock_key.append(sort_key);
  if (!lock_set_.count(lock_key)) {
    Status s;
    switch (lock_manager_type_) {
    case LockManagerType::kCentralized: {
      s = lock_table_->Lock(lock_key, read_ts_);
      lock_set_.insert(std::move(lock_key));
      break;
    }
    case LockManagerType::kDecentralized: {
      auto sub_table = GetSubTable_(sub_table_key, opts);
      s = sub_table->GetLockTable().Lock(lock_key, read_ts_);
      lock_set_.insert(std::move(lock_key));
      break;
    }
    case LockManagerType::kInlined: {
      break;
    }
    default:
      UNREACHABLE();
    }
    return s;
  }
  return Status::Ok();
}

btree::SubTable *
TxnContextOCC::GetSubTable_(const std::string_view &sub_table_key,
                            const Options &opts) noexcept {
  auto it = tables_.find(sub_table_key);
  if (it != tables_.end()) {
    return it->second.get();
  }
  std::unique_ptr<btree::SubTable> table;
  auto s = btree::SubTable::OpenSubTable(sub_table_key, opts, &table);
  CHECK(s.ok());
  auto [new_it, succeed] =
      tables_.emplace(table->GetTableKey(), std::move(table));
  CHECK(succeed);
  return new_it->second.get();
}

template <typename Func>
void WriteLogHelper_(log_store::LogStore *log_store, log_store::LsnType *lsn,
                     Func &&func) noexcept {
  if (log_store == nullptr) {
    return;
  }
  wal::OccLogWriter log_writer;
  func(log_writer);
  log_store::LogStore::LogResultContainer result;
  log_store->AppendLogRecord(log_writer.GetLogRecords(), &result);
  *lsn = std::max(*lsn, result[0].end_lsn);
}

void TxnContextOCC::Begin_(log_store::LogStore *log_store) noexcept {
  WriteLogHelper_(log_store, &lsn_,
                  [read_ts = read_ts_](wal::OccLogWriter &log_writer) {
                    log_writer.Begin(read_ts);
                  });
}

void TxnContextOCC::Commit_(log_store::LogStore *log_store) noexcept {
  WriteLogHelper_(log_store, &lsn_,
                  [read_ts = read_ts_,
                   commit_ts = commit_ts_](wal::OccLogWriter &log_writer) {
                    log_writer.Commit(read_ts, commit_ts);
                  });
}

void TxnContextOCC::Abort_(log_store::LogStore *log_store) noexcept {
  WriteLogHelper_(log_store, &lsn_,
                  [read_ts = read_ts_](wal::OccLogWriter &log_writer) {
                    log_writer.Abort(read_ts);
                  });
}

} // namespace txn
} // namespace arcanedb
