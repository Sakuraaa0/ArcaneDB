/**
 * @file versioned_bwtree_page_test.cpp
 * @author sheep (ysj1173886760@gmail.com)
 * @brief
 * @version 0.1
 * @date 2023-01-24
 *
 * @copyright Copyright (c) 2023
 *
 */

#include "btree/page/versioned_bwtree_page.h"
#include "bvar/bvar.h"
#include "common/config.h"
#include "util/bthread_util.h"
#include "util/wait_group.h"
#include <gtest/gtest.h>

namespace arcanedb {
namespace btree {

class VersionedBwTreePageTest : public ::testing::Test {
public:
  property::Schema MakeTestSchema() noexcept {
    property::Column column1{
        .column_id = 0, .name = "int64", .type = property::ValueType::Int64};
    property::Column column2{
        .column_id = 1, .name = "int32", .type = property::ValueType::Int32};
    property::Column column3{
        .column_id = 2, .name = "string", .type = property::ValueType::String};
    property::RawSchema schema{.columns = {column1, column2, column3},
                               .schema_id = 0,
                               .sort_key_count = 2};
    return property::Schema(schema);
  }

  struct ValueStruct {
    int64_t point_id;
    int32_t point_type;
    std::string value;
  };

  std::vector<ValueStruct> GenerateValueList(size_t size) noexcept {
    std::vector<ValueStruct> value;
    for (int i = 0; i < size; i++) {
      value.push_back(ValueStruct{
          .point_id = i, .point_type = 0, .value = std::to_string(i)});
    }
    return value;
  }

  template <typename Func>
  Status WriteHelper(const ValueStruct &value, Func func) noexcept {
    property::ValueRefVec vec;
    vec.push_back(value.point_id);
    vec.push_back(value.point_type);
    vec.push_back(value.value);
    util::BufWriter writer;
    EXPECT_TRUE(property::Row::Serialize(vec, &writer, &schema_).ok());
    auto str = writer.Detach();
    property::Row row(str.data());
    return func(row);
  }

  void TestRead(const property::Row &row, const ValueStruct &value) {
    {
      property::ValueResult res;
      EXPECT_TRUE(row.GetProp(0, &res, &schema_).ok());
      EXPECT_EQ(std::get<int64_t>(res.value), value.point_id);
    }
    {
      property::ValueResult res;
      EXPECT_TRUE(row.GetProp(1, &res, &schema_).ok());
      EXPECT_EQ(std::get<int32_t>(res.value), value.point_type);
    }
    {
      property::ValueResult res;
      EXPECT_TRUE(row.GetProp(2, &res, &schema_).ok());
      EXPECT_EQ(std::get<std::string_view>(res.value), value.value);
    }
  }

  void SetUp() {
    schema_ = MakeTestSchema();
    opts_.schema = &schema_;
    page_ = std::make_unique<VersionedBwTreePage>();
  }

  void TearDown() {}

  Options opts_;
  const int32_t type_ = 0;
  property::Schema schema_;
  std::unique_ptr<VersionedBwTreePage> page_;
};

TEST_F(VersionedBwTreePageTest, BasicTest) {
  // insert
  {
    ValueStruct value{.point_id = 0, .point_type = 0, .value = "hello"};
    auto s = WriteHelper(value, [&](const property::Row &row) {
      return page_->SetRow(row, 0, opts_);
    });
    EXPECT_TRUE(s.ok());
    RowView view;
    auto sk = property::SortKeys({value.point_id, value.point_type});
    EXPECT_TRUE(page_->GetRow(sk.as_ref(), 1, opts_, &view).ok());
    TestRead(view.at(0), value);
  }
  // update
  {
    ValueStruct value{.point_id = 0, .point_type = 0, .value = "world"};
    auto s = WriteHelper(value, [&](const property::Row &row) {
      return page_->SetRow(row, 1, opts_);
    });
    EXPECT_TRUE(s.ok());
    RowView view;
    auto sk = property::SortKeys({value.point_id, value.point_type});
    EXPECT_TRUE(page_->GetRow(sk.as_ref(), 1, opts_, &view).ok());
    TestRead(view.at(0), value);
  }
  // delete
  {
    ValueStruct value{.point_id = 0, .point_type = 0, .value = ""};
    auto sk = property::SortKeys({value.point_id, value.point_type});
    auto s = page_->DeleteRow(sk.as_ref(), 2, opts_);
    EXPECT_TRUE(s.ok());
    RowView view;
    EXPECT_TRUE(page_->GetRow(sk.as_ref(), 2, opts_, &view).IsNotFound());
  }
}

TEST_F(VersionedBwTreePageTest, CompactionTest) {
  auto value_list = GenerateValueList(1000);
  Options opts;
  opts.disable_compaction = false;
  for (const auto &value : value_list) {
    auto s = WriteHelper(value, [&](const property::Row &row) {
      return page_->SetRow(row, 0, opts);
    });
    EXPECT_TRUE(s.ok());
  }
  EXPECT_LE(page_->TEST_GetDeltaLength(),
            common::Config::kBwTreeDeltaChainLength);
  // test read
  for (const auto &value : value_list) {
    auto sk = property::SortKeys({value.point_id, value.point_type});
    RowView view;
    auto s = page_->GetRow(sk.as_ref(), 0, opts_, &view);
    EXPECT_TRUE(s.ok());
    TestRead(view.at(0), value);
  }
}

TEST_F(VersionedBwTreePageTest, ConcurrentCompactionTest) {
  int worker_count = 100;
  int epoch = 10;
  util::WaitGroup wg(worker_count);
  Options opts;
  opts.disable_compaction = false;
  bvar::LatencyRecorder write_latency;
  bvar::LatencyRecorder read_latency;
  bvar::LatencyRecorder read_null_latency;
  bvar::LatencyRecorder epoch_latency;
  for (int i = 0; i < worker_count; i++) {
    util::LaunchAsync([&, index = i]() {
      for (int j = 0; j < epoch; j++) {
        util::Timer epoch_timer;
        // insert
        TxnTs ts = j * 3;
        {
          ValueStruct value{
              .point_id = index, .point_type = 0, .value = "hello"};
          auto s = WriteHelper(value, [&](const property::Row &row) {
            util::Timer timer;
            auto s = page_->SetRow(row, ts, opts);
            write_latency << timer.GetElapsed();
            return s;
          });
          EXPECT_TRUE(s.ok());
          RowView view;
          auto sk = property::SortKeys({value.point_id, value.point_type});
          {
            util::Timer timer;
            EXPECT_TRUE(page_->GetRow(sk.as_ref(), ts, opts, &view).ok());
            read_latency << timer.GetElapsed();
          }
          TestRead(view.at(0), value);
        }
        // update
        {
          ValueStruct value{
              .point_id = index, .point_type = 0, .value = "world"};
          auto s = WriteHelper(value, [&](const property::Row &row) {
            util::Timer timer;
            auto s = page_->SetRow(row, ts + 1, opts);
            write_latency << timer.GetElapsed();
            return s;
          });
          EXPECT_TRUE(s.ok());
          RowView view;
          auto sk = property::SortKeys({value.point_id, value.point_type});
          {
            util::Timer timer;
            EXPECT_TRUE(page_->GetRow(sk.as_ref(), ts + 1, opts, &view).ok());
            read_latency << timer.GetElapsed();
          }
          TestRead(view.at(0), value);
        }
        // delete
        {
          ValueStruct value{.point_id = index, .point_type = 0, .value = ""};
          auto sk = property::SortKeys({value.point_id, value.point_type});
          {
            util::Timer timer;
            auto s = page_->DeleteRow(sk.as_ref(), ts + 2, opts);
            write_latency << timer.GetElapsed();
            EXPECT_TRUE(s.ok());
          }
          {
            RowView view;
            util::Timer timer;
            EXPECT_TRUE(
                page_->GetRow(sk.as_ref(), ts + 2, opts, &view).IsNotFound());
            read_null_latency << timer.GetElapsed();
          }
        }
        epoch_latency << epoch_timer.GetElapsed();
      }
      wg.Done();
    });
  }
  wg.Wait();
  EXPECT_LE(page_->TEST_GetDeltaLength(),
            common::Config::kBwTreeDeltaChainLength);
  ARCANEDB_INFO("read avg latency: {}, max latency: {}", read_latency.latency(),
                read_latency.max_latency());
  ARCANEDB_INFO("read null avg latency: {}, max latency: {}",
                read_null_latency.latency(), read_null_latency.max_latency());
  ARCANEDB_INFO("write latency avg: {}, max latency: {}",
                write_latency.latency(), write_latency.max_latency());
  ARCANEDB_INFO("epoch latency avg: {}, max latency: {}",
                epoch_latency.latency(), epoch_latency.max_latency());
  ARCANEDB_INFO("{}", page_->TEST_GetDeltaLength());
}

TEST_F(VersionedBwTreePageTest, PerformanceTest) {
  int epoch = 1000;
  util::WaitGroup wg(1);
  Options opts;
  opts.disable_compaction = false;
  bvar::LatencyRecorder write_latency;
  bvar::LatencyRecorder read_latency;
  bvar::LatencyRecorder read_null_latency;
  bvar::LatencyRecorder epoch_latency;
  for (int j = 0; j < epoch; j++) {
    util::Timer epoch_timer;
    // insert
    TxnTs ts = 3 * j;
    {
      ValueStruct value{.point_id = 0, .point_type = 0, .value = "hello"};
      auto s = WriteHelper(value, [&](const property::Row &row) {
        util::Timer timer;
        auto s = page_->SetRow(row, ts, opts);
        write_latency << timer.GetElapsed();
        return s;
      });
      EXPECT_TRUE(s.ok());
      RowView view;
      auto sk = property::SortKeys({value.point_id, value.point_type});
      {
        util::Timer timer;
        EXPECT_TRUE(page_->GetRow(sk.as_ref(), ts, opts, &view).ok());
        read_latency << timer.GetElapsed();
      }
      TestRead(view.at(0), value);
    }
    // update
    {
      ValueStruct value{.point_id = 0, .point_type = 0, .value = "world"};
      auto s = WriteHelper(value, [&](const property::Row &row) {
        util::Timer timer;
        auto s = page_->SetRow(row, ts + 1, opts);
        write_latency << timer.GetElapsed();
        return s;
      });
      EXPECT_TRUE(s.ok());
      RowView view;
      auto sk = property::SortKeys({value.point_id, value.point_type});
      {
        util::Timer timer;
        EXPECT_TRUE(page_->GetRow(sk.as_ref(), ts + 1, opts, &view).ok());
        read_latency << timer.GetElapsed();
      }
      TestRead(view.at(0), value);
    }
    // delete
    {
      ValueStruct value{.point_id = 0, .point_type = 0, .value = ""};
      auto sk = property::SortKeys({value.point_id, value.point_type});
      {
        util::Timer timer;
        auto s = page_->DeleteRow(sk.as_ref(), ts + 2, opts);
        write_latency << timer.GetElapsed();
        EXPECT_TRUE(s.ok());
      }
      {
        RowView view;
        util::Timer timer;
        auto s = page_->GetRow(sk.as_ref(), ts + 2, opts, &view);
        EXPECT_TRUE(s.IsNotFound()) << s.ToString();
        read_null_latency << timer.GetElapsed();
      }
    }
    epoch_latency << epoch_timer.GetElapsed();
  }
  ARCANEDB_INFO("read avg latency: {}, max latency: {}", read_latency.latency(),
                read_latency.max_latency());
  ARCANEDB_INFO("read null avg latency: {}, max latency: {}",
                read_null_latency.latency(), read_null_latency.max_latency());
  ARCANEDB_INFO("write latency avg: {}, max latency: {}",
                write_latency.latency(), write_latency.max_latency());
  ARCANEDB_INFO("epoch latency avg: {}, max latency: {}",
                epoch_latency.latency(), epoch_latency.max_latency());
}

} // namespace btree
} // namespace arcanedb