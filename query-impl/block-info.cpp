#include "../blockchain-api-query.hpp"

HttpQueryBlockInfo::HttpQueryBlockInfo(ton::BlockIdExt block_id, std::string prefix,
                                       td::Promise<MHD_Response *> promise)
    : HttpQueryCommon(std::move(prefix), std::move(promise), std::move(block_id)) {
}

HttpQueryBlockInfo::HttpQueryBlockInfo(std::map<std::string, std::string> opts, std::string prefix,
                                       td::Promise<MHD_Response *> promise)
    : HttpQueryCommon(std::move(prefix), std::move(promise)) {
  auto R = parse_block_id(opts);
  if (R.is_ok()) {
    block_id_ = R.move_as_ok();
  } else {
    error_ = R.move_as_error();
  }
}

HttpQueryBlockInfo::HttpQueryBlockInfo(std::map<std::string, std::string> opts, std::string prefix,
                                       td::Promise<MHD_Response *> promise, DatabaseConfigParams *dbConfParams)
    : HttpQueryCommon(std::move(prefix), std::move(promise)) {
  auto R = parse_block_id(opts);
  if (R.is_ok()) {
    block_id_ = R.move_as_ok();
  } else {
    error_ = R.move_as_error();
  }
  if (dbConfParams != NULL) {
    if (dbConfParams->conn != NULL) {
      this->conn = dbConfParams->conn;
    } else {
      error_ = td::Status::Error("Connection is empty");
    }
    if (dbConfParams->mtxDB != NULL) {
      this->mtx = dbConfParams->mtxDB;
    } else {
      error_ = td::Status::Error("Mutex is null");
    }
  } else {
    error_ = td::Status::Error("DB not connected");
  }
}

void HttpQueryBlockInfo::start_up_query() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &HttpQueryBlockInfo::abort_query, R.move_as_error_prefix("litequery failed: "));
    } else {
      td::actor::send_closure(SelfId, &HttpQueryBlockInfo::got_block_header, R.move_as_ok());
    }
  });
  auto query = ton::serialize_tl_object(
      ton::create_tl_object<ton::lite_api::liteServer_getBlock>(ton::create_tl_lite_block_id(block_id_)), true);
  td::actor::send_closure(CoreActorInterface::instance_actor_id(), &CoreActorInterface::send_lite_query,
                          std::move(query), std::move(P));
  pending_queries_ = 1;

  if (block_id_.is_masterchain()) {
    auto P_2 = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &HttpQueryBlockInfo::failed_to_get_shard_info,
                                R.move_as_error_prefix("litequery failed: "));
      } else {
        td::actor::send_closure(SelfId, &HttpQueryBlockInfo::got_shard_info, R.move_as_ok());
      }
    });
    auto query_2 = ton::serialize_tl_object(
        ton::create_tl_object<ton::lite_api::liteServer_getAllShardsInfo>(ton::create_tl_lite_block_id(block_id_)),
        true);
    td::actor::send_closure(CoreActorInterface::instance_actor_id(), &CoreActorInterface::send_lite_query,
                            std::move(query_2), std::move(P_2));
    pending_queries_++;
    auto P_4 = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &HttpQueryBlockInfo::abort_query, R.move_as_error());
      } else {
        td::actor::send_closure(SelfId, &HttpQueryBlockInfo::got_config, R.move_as_ok());
      }
    });
    auto query_4 = ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_getConfigParams>(
                                                0, ton::create_tl_lite_block_id(block_id_), std::vector<int>({34})),
                                            true);
    td::actor::send_closure(CoreActorInterface::instance_actor_id(), &CoreActorInterface::send_lite_query,
                            std::move(query_4), std::move(P_4));

    pending_queries_++;
  }
  auto query_3 = ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_listBlockTransactions>(
                                              ton::create_tl_lite_block_id(block_id_), 7, 1024, nullptr, false, false),
                                          true);
  auto P_3 = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &HttpQueryBlockInfo::abort_query, R.move_as_error_prefix("litequery failed: "));
    } else {
      td::actor::send_closure(SelfId, &HttpQueryBlockInfo::got_transactions, R.move_as_ok());
    }
  });
  td::actor::send_closure(CoreActorInterface::instance_actor_id(), &CoreActorInterface::send_lite_query,
                          std::move(query_3), std::move(P_3));
  pending_queries_++;
}

void HttpQueryBlockInfo::got_transactions(td::BufferSlice data) {
  auto F = ton::fetch_tl_object<ton::lite_api::liteServer_blockTransactions>(std::move(data), true);
  if (F.is_error()) {
    abort_query(F.move_as_error());
    return;
  }
  auto f = F.move_as_ok();
  trans_req_count_ = f->req_count_;

  for (std::size_t i = 0; i < f->ids_.size(); i++) {
    transactions_.emplace_back(block::StdAddress{block_id_.id.workchain, f->ids_[i]->account_},
                               static_cast<ton::LogicalTime>(f->ids_[i]->lt_), f->ids_[i]->hash_);
    start_transaction_query(block::StdAddress{block_id_.id.workchain, f->ids_[i]->account_},
                            static_cast<ton::LogicalTime>(f->ids_[i]->lt_), f->ids_[i]->hash_);
  }

  if (f->incomplete_ && transactions_.size() > 0) {
    const auto &T = *transactions_.rbegin();
    auto query_3 = ton::serialize_tl_object(
        ton::create_tl_object<ton::lite_api::liteServer_listBlockTransactions>(
            ton::create_tl_lite_block_id(block_id_), 7 + 128, 1024,
            ton::create_tl_object<ton::lite_api::liteServer_transactionId3>(T.addr.addr, T.lt), false, false),
        true);
    auto P_3 = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &HttpQueryBlockInfo::abort_query, R.move_as_error_prefix("litequery failed: "));
      } else {
        td::actor::send_closure(SelfId, &HttpQueryBlockInfo::got_transactions, R.move_as_ok());
      }
    });
    td::actor::send_closure(CoreActorInterface::instance_actor_id(), &CoreActorInterface::send_lite_query,
                            std::move(query_3), std::move(P_3));
  } else {
    if (!--pending_queries_) {
      finish_query();
    }
  }
}

void HttpQueryBlockInfo::got_full_transaction(td::BufferSlice result) {
  auto F = ton::fetch_tl_object<ton::lite_api::liteServer_transactionList>(std::move(result), true);
  if (F.is_error()) {
    abort_query(F.move_as_error());
    return;
  }

  auto f = F.move_as_ok();
  transactionsInfo.push_back(std::move(f->transactions_));
  if (f->ids_.size() == 0) {
    abort_query(td::Status::Error("no transactions found"));
    return;
  }
  if (!--pending_queries_) {
    finish_query();
  }
}

void HttpQueryBlockInfo::got_config(td::BufferSlice data) {
  auto F = ton::fetch_tl_object<ton::lite_api::liteServer_configInfo>(std::move(data), true);
  if (F.is_error()) {
    abort_query(F.move_as_error());
    return;
  }
  auto f = F.move_as_ok();

  state_proof_ = std::move(f->state_proof_);
  config_proof_ = std::move(f->config_proof_);

  if (!--pending_queries_) {
    finish_query();
  }
}

void HttpQueryBlockInfo::start_transaction_query(block::StdAddress addr, ton::LogicalTime lt, ton::Bits256 hash) {
  auto P =
      td::PromiseCreator::lambda([SelfId = actor_id(this), addr = addr, lt = lt, hash = hash, this](td::Result<td::BufferSlice> R) {
        if (R.is_error()) {
          mtx->lock();
          // LOG(INFO) << "transaction failed for block: " << block_id_.to_str() << " addr: " << addr.rserialize(true) << '\n';
          transactionsNotPassed.push_back(TransactionDescr{addr, lt, hash});
          mtx->unlock();
          td::actor::send_closure(SelfId, &HttpQueryBlockInfo::next, R.move_as_error_prefix("litequery failed: "));
        } else {
          td::actor::send_closure(SelfId, &HttpQueryBlockInfo::got_full_transaction, R.move_as_ok());
        }
      });
  auto a = ton::create_tl_object<ton::lite_api::liteServer_accountId>(addr.workchain, addr.addr);
  auto query = ton::serialize_tl_object(
      ton::create_tl_object<ton::lite_api::liteServer_getTransactions>(1, std::move(a), lt, hash), true);
  td::actor::send_closure(CoreActorInterface::instance_actor_id(), &CoreActorInterface::send_lite_query,
                          std::move(query), std::move(P));
  pending_queries_++;
}

// it will return only one column

#define VALIDATORSql(x, y)                                                                                         \
  std::string(                                                                                                     \
      std::string("select distinct encode(wallet, 'escape') from validators v where \"time\" >= to_timestamp(") +  \
      std::to_string(x) + std::string(") and  \"time\" <= to_timestamp(") + std::to_string(y) + std::string(");")) \
      .c_str()

bool HttpQueryBlockInfo::unpackValidators(HttpAnswer &A, std::shared_ptr<PGconn> conn) {
  UniValue uvArr;
  uvArr.setArray();
  auto R = block::check_extract_state_proof(block_id_, state_proof_.as_slice(), config_proof_.as_slice());
  if (R.is_error()) {
    A.abort(PSTRING() << "masterchain state proof for " << block_id_.to_str() << " is invalid : " << R.move_as_error());\
    return false;
  }
  try {
    auto res = block::Config::extract_from_state(R.move_as_ok(), 0);
    if (res.is_error()) {
      A.abort(PSTRING() << "cannot unpack configuration: " << res.move_as_error());
      return false;
    }
    std::unique_ptr<block::Config> config = res.move_as_ok();
    td::Ref<vm::Cell> value = config->get_config_param(34);
    if (value.not_null()) {
      std::int64_t from = 0;
      std::int64_t to = 0;
      A.unpackValidatorsTime(HttpAnswer::ValidatorSet{34, value}, from, to);
      if (conn != nullptr) {
        std::unique_ptr<PGresult, decltype(&PQclear)> resSQL(PQexec(conn.get(), VALIDATORSql(from, to)), &PQclear);
        if (PQresultStatus(resSQL.get()) != PGRES_TUPLES_OK) {
          A.abort(PSTRING() << "Select failed: " << PQresultErrorMessage(resSQL.get()));
        } else {
          for (int i = 0; i < PQntuples(resSQL.get()); i++) {
            for (int j = 0; j < PQnfields(resSQL.get()); j++) {
              uvArr.push_back(std::string(PQgetvalue(resSQL.get(), i, j)));
            }
          }
        }
        resSQL.reset();
      } else {
        abort_query(td::Status::Error(404, PSTRING() << "unnable to connect to db"));
        A.abort(td::Status::Error(404, PSTRING() << "unnable to connect to db"));
        return false;
      }
    } else {
      abort_query(td::Status::Error(404, PSTRING() << "empty param " << 34));
      A.abort(td::Status::Error(404, PSTRING() << "empty param " << 34));
      return false;
    }
  } catch (vm::VmError &err) {
    A.abort(PSTRING() << "error while traversing configuration: " << err.get_msg());
    return false;
  } catch (vm::VmVirtError &err) {
    A.abort(PSTRING() << "virtualization error while traversing configuration: " << err.get_msg());
    return false;
  }
  A.putInJson("validators", uvArr);
  return true;
}

void HttpQueryBlockInfo::finish_query() {
  if (promise_) {
    auto page = [&](std::shared_ptr<PGconn> conn) -> std::string { 
      HttpAnswer A{"blockinfo", prefix_}; 
      A.set_block_id(block_id_);
      create_header(A);
      auto res = vm::std_boc_deserialize(data_.clone());
      if (res.is_error()) {
        A.abort(PSTRING() << "cannot deserialize block header data: " << res.move_as_error());
        return A.finish();
      }
      auto data = HttpAnswer::RawData<block::gen::Block>{res.move_as_ok()};
      A.serializeBlockData(data.root, block_id_);

      UniValue uvArr;
      uvArr.setArray();
      for(auto &tr : transactionsInfo) {
        auto R = vm::std_boc_deserialize_multi(std::move(tr));
        if (R.is_error()) {
          A.abort(PSTRING() << "FATAL: cannot deserialize transactions BoC");
          return A.finish();
        }
        auto list = R.move_as_ok();
        auto n = list.size();
        if (n != 1) {
          A.abort(PSTRING() << "obtained " << n << " transaction, but only 1 have been requested");
          return A.finish();
        } else {
          UniValue uvObj;
          uvObj.setObject();
          A.serializeObject(HttpAnswer::TransactionSmall{block_id_, list[0]}, uvObj);
          uvArr.push_back(uvObj);
        }
      }
      for (auto &tr : transactionsNotPassed) {
        UniValue uvObj;
        uvObj.setObject();
        A.serializeObject(tr.addr, tr.lt, tr.hash, uvObj);
        uvArr.push_back(uvObj);
      }
      HttpAnswer::TransactionList I;
      I.block_id = block_id_;
      I.req_count_ = trans_req_count_;
      for (auto &T : transactions_) {
        I.vec.emplace_back(T.addr, T.lt, T.hash);
      }
      A.serializeObject(I);
      I.vec.clear();
      transactions_.clear();
      transactionsNotPassed.clear();
      transactionsInfo.clear();
      A.putInJson("transactions", uvArr);
      if (block_id_.id.workchain == -1) {
        unpackValidators(A, conn);
      }

      return A.finish();
    }(this->conn);
    auto R = MHD_create_response_from_buffer(page.length(), const_cast<char *>(page.c_str()), MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(R, "Content-Type", "application/json");
    promise_.set_value(std::move(R));
  }
  stop();
}
