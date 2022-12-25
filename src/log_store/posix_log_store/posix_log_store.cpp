/**
 * @file posix_log_store.cpp
 * @author sheep (ysj1173886760@gmail.com)
 * @brief
 * @version 0.1
 * @date 2022-12-24
 *
 * @copyright Copyright (c) 2022
 *
 */

#include "log_store/posix_log_store/posix_log_store.h"
#include "common/config.h"
#include "log_store/log_store.h"
#include "log_store/posix_log_store/log_segment.h"
#include "util/bthread_util.h"
#include <atomic>
#include <string>

namespace arcanedb {
namespace log_store {

Status PosixLogStore::Open(const std::string &name, const Options &options,
                           std::shared_ptr<LogStore> *log_store) noexcept {
  auto store = std::make_shared<PosixLogStore>();
  store->env_ = leveldb::Env::Default();
  store->name_ = name;
  // create directory
  auto s = store->env_->CreateDir(name);
  if (!s.ok()) {
    LOG_WARN("Failed to create dir, error: %s", s.ToString().c_str());
    return Status::Err();
  }

  // create log file
  s = store->env_->NewWritableFile(MakeLogFileName_(name), &store->log_file_);
  if (!s.ok()) {
    LOG_WARN("Failed to create writable file, error: %s", s.ToString().c_str());
    return Status::Err();
  }

  // initialize log segment
  store->segment_num_ = options.segment_num;
  store->segments_ = std::make_unique<LogSegment[]>(options.segment_num);

  // set first log segment as open
  store->GetCurrentLogSegment_()->OpenLogSegment(0);

  // generate mfence here.
  std::atomic_thread_fence(std::memory_order_seq_cst);

  // start background thread
  store->StartBackgroundThread_();

  *log_store = store;
  return Status::Ok();
}

Status PosixLogStore::AppendLogRecord(std::vector<std::string> log_records,
                                      std::vector<LsnRange> *result) noexcept {
  
  return Status::Ok();
}

LsnType GetPersistentLsn() noexcept { return kInvalidLsn; }

void ControlGuard::OnExit_() noexcept { segment_->OnWriterExit(); }

void PosixLogStore::ThreadJob_() noexcept {
  size_t current_io_segment = 0;
  while (!stopped_.load(std::memory_order_relaxed)) {
    auto *log_segment = GetLogSegment_(current_io_segment);
    if (log_segment->state_.load(std::memory_order_relaxed) ==
        LogSegment::LogSegmentState::kIo) {
      auto data = log_segment->buffer_;
      auto s = log_file_->Append(data);
      if (!s.ok()) {
        FATAL("io failed, status: %s", s.ToString().c_str());
      }
      s = log_file_->Sync();
      if (!s.ok()) {
        FATAL("sync failed, status: %s", s.ToString().c_str());
      }
      log_segment->FreeSegment();
      // increment index
      current_io_segment = (current_io_segment + 1) % segment_num_;
      continue;
    }
    // otherwise, we wait
    log_segment->waiter_.Wait(common::Config::kLogStoreFlushInterval);
    // recheck state
    if (log_segment->state_.load(std::memory_order_acquire) !=
        LogSegment::LogSegmentState::kIo) {
      SealAndOpen(log_segment);
    }
  }
}

bool PosixLogStore::SealAndOpen(LogSegment *log_segment) noexcept {
  // try to seal the segment.
  auto lsn = log_segment->TrySealLogSegment();
  if (!lsn.has_value()) {
    return false;
  }
  // open a new segment
  OpenNewLogSegment_(lsn.value());
  return true;
}

} // namespace log_store
} // namespace arcanedb