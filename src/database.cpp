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
#include <lineairdb/tx_status.h>

#include <functional>
#include <memory>

#include "database_impl.h"
#include "util/logger.hpp"
namespace LineairDB {

Database::Database() noexcept : db_pimpl_(std::make_unique<Impl>()) {
  LineairDB::Util::SetUpSPDLog();
}
Database::Database(const Config& c) noexcept
    : db_pimpl_(std::make_unique<Impl>(c)) {
  LineairDB::Util::SetUpSPDLog();
}

Database::~Database() noexcept = default;

const Config Database::GetConfig() const noexcept {
  return db_pimpl_->GetConfig();
}

void Database::ExecuteTransaction(
    std::function<void(Transaction&)> transaction_procedure,
    std::function<void(TxStatus)> callback) {
  db_pimpl_->ExecuteTransaction(transaction_procedure, callback);
}
void Database::Fence() const noexcept { db_pimpl_->Fence(); }

}  // namespace LineairDB
