/**
 * @file log_segment.h
 * @author sheep (ysj1173886760@gmail.com)
 * @brief
 * @version 0.1
 * @date 2022-12-25
 *
 * @copyright Copyright (c) 2022
 *
 */

#pragma once

#include "common/logger.h"
#include "log_store/log_store.h"
#include "util/codec/buf_writer.h"
#include "util/thread_pool.h"
#include <atomic>
#include <optional>

namespace arcanedb {
namespace log_store {

class LogSegment;

class ControlGuard {
public:
  ControlGuard(LogSegment *segment) : segment_(segment) {}
  ~ControlGuard() { OnExit_(); }

private:
  /**
   * @brief
   * 1. decr the writer num.
   * 2. when writer num == 0 and log segment is sealed, schedule an io task.
   */
  void OnExit_() noexcept;

  LogSegment *segment_{nullptr};
};

class LogSegment {
public:
  LogSegment(size_t size) : size_(size), writer_(size) {}

  LogSegment(LogSegment &&rhs) noexcept
      : state_(rhs.state_.load(std::memory_order_relaxed)), size_(rhs.size_),
        start_lsn_(rhs.start_lsn_), writer_(size_),
        control_bits_(rhs.control_bits_.load(std::memory_order_relaxed)) {}

  /**
   * @brief
   * return nullopt indicates writer should wait.
   * return ControlGuard indicates writer is ok to writing log records.
   * second returned value indicates whether writer should seal current log
   * segment.
   * @param length length of this batch of log record
   * @return std::pair<std::optional<ControlGuard>, bool>
   */
  std::pair<std::optional<ControlGuard>, bool>
  AcquireControlGuard(size_t length) {
    uint64_t current_control_bits =
        control_bits_.load(std::memory_order_acquire);
    uint64_t new_control_bits;
    do {
      auto current_lsn = GetLsn_(current_control_bits);
      if (length > size_) {
        LOG_WARN("LogLength: %lld is greater than total size: %lld, resize is "
                 "needed",
                 static_cast<int64_t>(length), static_cast<int64_t>(size_));
      }
      if (current_lsn + length > size_) {
        // writer should seal current log segment and open new one.
        return {std::nullopt, true};
      }
      auto current_writers = GetWriterNum_(current_control_bits);
      if (current_writers + 1 > kMaximumWriterNum) {
        // too much writers
        return {std::nullopt, false};
      }
      // CAS the new control bits
      new_control_bits = IncrWriterNum_(current_control_bits);
      new_control_bits = BumpLsn_(new_control_bits, length);
    } while (control_bits_.compare_exchange_weak(
        current_control_bits, new_control_bits, std::memory_order_acq_rel));
    return {ControlGuard(this), false};
  }

  void OnWriterExit() noexcept {
    uint64_t current_control_bits =
        control_bits_.load(std::memory_order_acquire);
    uint64_t new_control_bits;
    bool should_schedule_io_task = false;
    do {
      bool is_sealed = IsSealed_(current_control_bits);
      new_control_bits = DecrWriterNum_(current_control_bits);
      bool is_last_writer = GetWriterNum_(new_control_bits);
      if (is_last_writer && is_sealed) {
        should_schedule_io_task = true;
      }
    } while (control_bits_.compare_exchange_weak(
        current_control_bits, new_control_bits, std::memory_order_acq_rel));
    if (should_schedule_io_task) {
      state_.store(LogSegmentState::kIo, std::memory_order_relaxed);
    }
  }

  void OpenLogSegmentAndSetLsn(LsnType start_lsn) noexcept {
    CHECK(state_.load(std::memory_order_relaxed) == LogSegmentState::kFree);
    start_lsn_ = start_lsn;
    state_.store(LogSegmentState::kOpen, std::memory_order_release);
    // writers must observe the state being kOpen before it can proceed to
    // appending log records.
  }

  std::optional<LsnType> TrySealLogSegment() noexcept {
    CHECK(state_.load(std::memory_order_relaxed) == LogSegmentState::kOpen);
    uint64_t current_control_bits =
        control_bits_.load(std::memory_order_acquire);
    uint64_t new_control_bits;
    LsnType new_lsn;
    do {
      if (IsSealed_(current_control_bits)) {
        return std::nullopt;
      }
      new_control_bits = MarkSealed_(current_control_bits);
      new_lsn = static_cast<LsnType>(GetLsn_(new_control_bits));
    } while (control_bits_.compare_exchange_weak(
        current_control_bits, new_control_bits, std::memory_order_acq_rel));
    return new_lsn;
  }

private:
  FRIEND_TEST(PosixLogStoreTest, LogSegmentControlBitTest);

  static bool IsSealed_(size_t control_bits) noexcept {
    return (control_bits >> kIsSealedOffset) & kIsSealedMaskbit;
  }

  static size_t MarkSealed_(size_t control_bits) noexcept {
    return control_bits | (1ul << kIsSealedOffset);
  }

  static size_t GetWriterNum_(size_t control_bits) noexcept {
    return (control_bits >> kWriterNumOffset) & kWriterNumMaskbit;
  }

  static size_t IncrWriterNum_(size_t control_bits) noexcept {
    return control_bits + (1ul << kWriterNumOffset);
  }

  static size_t DecrWriterNum_(size_t control_bits) noexcept {
    return control_bits - (1ul << kWriterNumOffset);
  }

  static size_t GetLsn_(size_t control_bits) noexcept {
    return control_bits & kLsnMaskbit;
  }

  static size_t BumpLsn_(size_t control_bits, size_t length) noexcept {
    return control_bits + length;
  }

  static constexpr size_t kIsSealedOffset = 63;
  static constexpr size_t kIsSealedMaskbit = 1;
  static constexpr size_t kWriterNumOffset = 48;
  static constexpr size_t kWriterNumMaskbit = 0x7FFF;
  static constexpr size_t kLsnOffset = 0;
  static constexpr size_t kLsnMaskbit = (1ul << 48) - 1;
  static constexpr size_t kMaximumWriterNum = kWriterNumMaskbit;

  /**
   * @brief
   * State change:
   * Initial state is kFree.
   * First segment is kOpen.
   * When segment is not capable of writing new logs, one of the foreground
   * threads is responsible to seal the segment. (the one who has complete the
   * CAS operation). The one who sealed the previous segment, is responsible to
   * open next log segment. note that background worker could also seal the
   * segment, preventing the latency of persisting log record being to high. The
   * last writer of sealed segment is responsible to change segment state from
   * kSeal to kIo. and schedule an io job.
   * At last, after io worker has finish it's job, it will change kIo to kFree,
   * indicating this segment is reusable again.
   */
  enum class LogSegmentState : uint8_t {
    kFree,
    kOpen,
    // kSeal, Seal state is guided by control bits.
    kIo,
  };

  std::atomic<LogSegmentState> state_{LogSegmentState::kFree};
  size_t size_{};
  LsnType start_lsn_{};
  util::BufWriter writer_;
  /**
   * @brief
   * Control bits format:
   * | IsSealed 1bit | Writer num 15bit | LsnOffset 48bit |
   */
  // TODO: there might be a more efficient way to implement lock-free WAL.
  std::atomic<uint64_t> control_bits_{0};
};

} // namespace log_store
} // namespace arcanedb