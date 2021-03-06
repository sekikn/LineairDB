/*
 *   Copyright (C) 2020 Nippon Telegraph and Telephone Corporation.

 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at

 *   http://www.apache.org/licenses/LICENSE-2.0

 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include <lineairdb/config.h>
#include <lineairdb/database.h>
#include <lineairdb/transaction.h>
#include <lineairdb/tx_status.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <experimental/filesystem>
#include <memory>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "magic_enum.hpp"
#include "spdlog/spdlog.h"

typedef std::function<void(LineairDB::Transaction&)> TransactionProcedure;
class ConcurrencyControlTest
    : public ::testing::TestWithParam<LineairDB::Config::ConcurrencyControl> {
 protected:
  LineairDB::Config config_;
  std::unique_ptr<LineairDB::Database> db_;
  virtual void SetUp() {
    config_.concurrency_control_protocol = ConcurrencyControlTest::GetParam();
    config_.enable_recovery              = false;
    config_.enable_logging               = false;
    db_ = std::make_unique<LineairDB::Database>(config_);
  }
  virtual void TearDown() {
    std::experimental::filesystem::remove_all("lineairdb_logs");
  }

  void DoTransactions(const std::vector<TransactionProcedure> txns) {
    std::atomic<size_t> terminated(0);
    for (auto& tx : txns) {
      db_->ExecuteTransaction(tx,
                              [&](const LineairDB::TxStatus) { terminated++; });
      db_->Fence();
    }

    size_t msec_elapsed_for_termination = 0;
    while (terminated != txns.size()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      msec_elapsed_for_termination++;
      bool too_long_time_elapsed = (db_->GetConfig().epoch_duration_ms * 1000) <
                                   msec_elapsed_for_termination;
      EXPECT_FALSE(too_long_time_elapsed);
      if (too_long_time_elapsed) break;
    }
  }

  size_t DoTransactionsOnMultiThreads(
      const std::vector<TransactionProcedure> txns) {
    std::atomic<size_t> terminated(0);
    std::atomic<size_t> committed(0);
    std::vector<std::thread> threads;
    for (auto& tx : txns) {
      threads.push_back(std::thread([&]() {
        db_->ExecuteTransaction(tx, [&](const LineairDB::TxStatus status) {
          terminated++;
          if (status == LineairDB::TxStatus::Committed) { committed++; }
        });
      }));
    }

    size_t msec_elapsed_for_termination = 0;
    while (terminated != txns.size()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      msec_elapsed_for_termination++;
      bool too_long_time_elapsed = (db_->GetConfig().epoch_duration_ms * 1000) <
                                   msec_elapsed_for_termination;
      EXPECT_FALSE(too_long_time_elapsed);
      if (too_long_time_elapsed) break;
    }

    for (auto& thread : threads) { thread.join(); }
    return committed;
  }
};

INSTANTIATE_TEST_SUITE_P(
    ForEachProtocol, ConcurrencyControlTest,
    ::testing::Values(LineairDB::Config::ConcurrencyControl::Silo,
                      LineairDB::Config::ConcurrencyControl::SiloNWR),
    [](const testing::TestParamInfo<LineairDB::Config::ConcurrencyControl>&
           param) { return std::string(magic_enum::enum_name(param.param)); });

TEST_P(ConcurrencyControlTest, Instantiate) {}

TEST_P(ConcurrencyControlTest, IncrementOnMultiThreads) {
  int initial_value = 1;
  DoTransactions({[&](LineairDB::Transaction& tx) {
    tx.Write<int>("alice", initial_value);
  }});
  db_->Fence();

  TransactionProcedure increment([](LineairDB::Transaction& tx) {
    auto alice = tx.Read<int>("alice");
    ASSERT_TRUE(alice.has_value());
    int current_value = alice.value();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    current_value++;
    tx.Write<int>("alice", current_value);
  });

  size_t committed_count = DoTransactionsOnMultiThreads({increment, increment});
  db_->Fence();

  DoTransactions({[&](LineairDB::Transaction& tx) {
    auto alice = tx.Read<int>("alice");
    ASSERT_TRUE(alice.has_value());
    auto expected_value = initial_value + committed_count;
    auto current_value  = alice.value();
    ASSERT_EQ(expected_value, current_value);
  }});
}

TEST_P(ConcurrencyControlTest, AvodingDirtyReadAnomaly) {
  TransactionProcedure insertTenTimes([](LineairDB::Transaction& tx) {
    int value = 0xBEEF;
    for (size_t idx = 0; idx <= 10; idx++) {
      tx.Write<int>("alice" + std::to_string(idx), value);
    }
    tx.Abort();
  });
  TransactionProcedure readTenTimes([](LineairDB::Transaction& tx) {
    for (size_t idx = 0; idx <= 10; idx++) {
      auto result = tx.Read<int>("alice" + std::to_string(idx));
      ASSERT_FALSE(result.has_value());
    }
  });
  ASSERT_NO_THROW({
    DoTransactionsOnMultiThreads(
        {insertTenTimes, insertTenTimes, readTenTimes, readTenTimes});
  });
}
TEST_P(ConcurrencyControlTest, RepeatableRead) {
  TransactionProcedure updateTenTimes([](LineairDB::Transaction& tx) {
    int value = 0xBEEF;
    for (size_t idx = 0; idx <= 10; idx++) {
      tx.Write<int>("alice", value + idx);
    }
  });
  TransactionProcedure repeatableRead([](LineairDB::Transaction& tx) {
    auto first_result = tx.Read<int>("alice");
    if (first_result.has_value()) {
      auto first_value = first_result.value();
      for (size_t idx = 0; idx <= 10; idx++) {
        auto result = tx.Read<int>("alice");
        ASSERT_TRUE(result.has_value());
        ASSERT_EQ(result.value(), first_value);
      }
    }
  });
  ASSERT_NO_THROW({
    DoTransactionsOnMultiThreads(
        {updateTenTimes, updateTenTimes, repeatableRead, repeatableRead});
  });
}
TEST_P(ConcurrencyControlTest, AvoidingWriteSkewAnomaly) {
  /** initialize **/
  DoTransactions({[](LineairDB::Transaction& tx) {
    tx.Write<int>("alice", 0);
    tx.Write<int>("bob", 1);
  }});

  TransactionProcedure readAliceWriteBob([](LineairDB::Transaction& tx) {
    auto result = tx.Read<int>("alice");
    ASSERT_TRUE(result.has_value());
    tx.Write<int>("bob", result.value() += 1);
  });
  TransactionProcedure readBobWriteAlice([](LineairDB::Transaction& tx) {
    auto result = tx.Read<int>("bob");
    ASSERT_TRUE(result.has_value());
    tx.Write<int>("alice", result.value() += 1);
  });

  DoTransactionsOnMultiThreads({readAliceWriteBob, readAliceWriteBob,
                                readAliceWriteBob, readAliceWriteBob,
                                readBobWriteAlice, readBobWriteAlice,
                                readBobWriteAlice, readBobWriteAlice});

  db_->Fence();

  /** validation **/
  DoTransactions({[](LineairDB::Transaction& tx) {
    auto alice = tx.Read<int>("alice");
    auto bob   = tx.Read<int>("bob");
    ASSERT_TRUE(alice.has_value());
    ASSERT_TRUE(bob.has_value());
    ASSERT_EQ(1, std::abs(alice.value() - bob.value()));
  }});
}

TEST_P(ConcurrencyControlTest, AvoidingReadOnlyAnomaly) {
  // Reference: Example 1.3 in
  // https://www.cse.iitb.ac.in/infolab/Data/Courses/CS632/2009/Papers/p492-fekete.pdf

  std::atomic<bool> waits(true);

  /** T1: r1(y0) w1(y1) **/
  TransactionProcedure T1([&](LineairDB::Transaction& tx) {
    auto y = tx.Read<int>("y");
    ASSERT_TRUE(y.has_value());
    ASSERT_EQ(0, y.value());

    while (waits) { std::this_thread::yield(); }

    tx.Write<int>("y", 20);
  });
  /** T2: r2(x0) r2(y0) w2(x2) **/
  TransactionProcedure T2([&](LineairDB::Transaction& tx) {
    auto x = tx.Read<int>("x");
    auto y = tx.Read<int>("y");
    ASSERT_TRUE(x.has_value() && y.has_value());
    ASSERT_EQ(0, x.value());
    ASSERT_EQ(0, y.value());

    waits.store(false);
    std::this_thread::yield();
    tx.Write<int>("x", -11);
  });

  /** T3: r3(x0) r3(y1) **/
  std::atomic<int> x_value_read_by_t3(0);
  std::atomic<int> y_value_read_by_t3(0);
  TransactionProcedure T3([&](LineairDB::Transaction& tx) {
    while (waits) { std::this_thread::yield(); }
    std::this_thread::yield();
    auto x = tx.Read<int>("x");
    auto y = tx.Read<int>("y");
    ASSERT_TRUE(x.has_value() && y.has_value());
    if (y.value() != 20) return tx.Abort();
    x_value_read_by_t3.store(x.value());
    y_value_read_by_t3.store(y.value());
  });

  size_t committed = 0;
  while (committed != 3) {
    waits.store(true);
    /** initialize **/
    DoTransactions({[](LineairDB::Transaction& tx) {
      tx.Write<int>("x", 0);
      tx.Write<int>("y", 0);
    }});

    committed = DoTransactionsOnMultiThreads({T1, T2, T3});
    if (committed == 3) {
      auto x = x_value_read_by_t3.load();
      auto y = y_value_read_by_t3.load();
      ASSERT_EQ(x, -11);
      ASSERT_EQ(y, 20);
    } else {
      SPDLOG_DEBUG(
          "Only {0} transactions has been committed. Retrying testcase...",
          committed);
    }
  }
}
