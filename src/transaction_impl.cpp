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

#include "transaction_impl.h"

#include <lineairdb/config.h>
#include <lineairdb/database.h>
#include <lineairdb/transaction.h>

#include <algorithm>
#include <memory>
#include <type_traits>
#include <utility>

#include "concurrency_control/concurrency_control_base.h"
#include "concurrency_control/impl/silo_nwr.hpp"
#include "database_impl.h"
#include "types.h"
namespace LineairDB {

Transaction::Impl::Impl(Database::Impl* db_pimpl) noexcept
    : user_aborted_(false),
      db_pimpl_(db_pimpl),
      config_ref_(db_pimpl_->GetConfig()) {
  TransactionReferences&& tx = {db_pimpl_->GetPointIndex(), read_set_,
                                write_set_, db_pimpl_->GetMyThreadLocalEpoch()};

  // WANTFIX for performance
  // Here we allocate one (derived) concurrency control instance per
  // transactions. It may be worse on performance because of heap
  // memory allocation. Need to re-implement with composition or templates.
  switch (config_ref_.concurrency_control_protocol) {
    case Config::ConcurrencyControl::SiloNWR:
      concurrency_control_ = std::make_unique<ConcurrencyControl::SiloNWR>(
          std::forward<TransactionReferences>(tx));
      break;
    case Config::ConcurrencyControl::Silo:
      concurrency_control_ = std::make_unique<ConcurrencyControl::Silo>(
          std::forward<TransactionReferences>(tx));
      break;
    default:
      concurrency_control_ = std::make_unique<ConcurrencyControl::SiloNWR>(
          std::forward<TransactionReferences>(tx));

      break;
  }
}

Transaction::Impl::~Impl() noexcept = default;

const std::pair<const std::byte* const, const size_t> Transaction::Impl::Read(
    const std::string_view key) {
  if (user_aborted_) return {nullptr, 0};

  for (auto& snapshot : write_set_) {
    if (snapshot.key == key) {
      return std::make_pair(snapshot.value_copy, snapshot.size);
    }
  }

  for (auto& snapshot : read_set_) {
    if (snapshot.key == key) {
      return std::make_pair(snapshot.value_copy, snapshot.size);
    }
  }
  const auto result = concurrency_control_->Read(key);
  read_set_.emplace_back(std::move(result));
  return {result.value_copy, result.size};
}  // namespace LineairDB

void Transaction::Impl::Write(const std::string_view key,
                              const std::byte value[], const size_t size) {
  if (user_aborted_) return;

  bool is_rmf = false;
  for (auto& snapshot : read_set_) {
    if (snapshot.key == key) {
      is_rmf                        = true;
      snapshot.is_read_modify_write = true;
    }
  }

  for (auto& snapshot : write_set_) {
    if (snapshot.key != key) continue;
    snapshot.Reset(value, size);
    if (is_rmf) snapshot.is_read_modify_write = true;
    return;
  }

  concurrency_control_->Write(key, value, size);
  Snapshot sp(key, value, size, nullptr);
  write_set_.emplace_back(std::move(sp));
}

void Transaction::Impl::Abort() { user_aborted_ = true; }
bool Transaction::Impl::Precommit() {
  if (user_aborted_) {
    concurrency_control_->PostProcessing(TxStatus::Aborted);
    return false;
  }

  bool committed = concurrency_control_->Precommit();
  if (committed) {
    concurrency_control_->PostProcessing(TxStatus::Committed);
  } else {
    concurrency_control_->PostProcessing(TxStatus::Aborted);
  }
  return committed;
}

const std::pair<const std::byte* const, const size_t> Transaction::Read(
    const std::string_view key) {
  return tx_pimpl_->Read(key);
}
void Transaction::Write(const std::string_view key, const std::byte value[],
                        const size_t size) {
  tx_pimpl_->Write(key, value, size);
}
void Transaction::Abort() { tx_pimpl_->Abort(); }
bool Transaction::Precommit() { return tx_pimpl_->Precommit(); }

Transaction::Transaction(void* db_pimpl) noexcept
    : tx_pimpl_(
          std::make_unique<Impl>(reinterpret_cast<Database::Impl*>(db_pimpl))) {
}
Transaction::~Transaction() noexcept = default;

}  // namespace LineairDB
