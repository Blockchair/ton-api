#include "../blockchain-api-query.hpp"


BlockSearch::BlockSearch(ton::WorkchainId workchain, ton::AccountIdPrefix account,
                                           ton::BlockSeqno seqno, std::string prefix,
                                           td::Promise<MHD_Response *> promise)
    : HttpQueryCommon(std::move(prefix), std::move(promise))
    , account_prefix_{workchain, account}
    , mode_(1)
    , seqno_(seqno) {
}
BlockSearch::BlockSearch(ton::WorkchainId workchain, ton::AccountIdPrefix account,
                                           ton::LogicalTime lt, std::string prefix, td::Promise<MHD_Response *> promise)
    : HttpQueryCommon(std::move(prefix), std::move(promise)), account_prefix_{workchain, account}, mode_(2), lt_(lt) {
}
BlockSearch::BlockSearch(ton::WorkchainId workchain, ton::AccountIdPrefix account, bool dummy,
                                           ton::UnixTime utime, std::string prefix, td::Promise<MHD_Response *> promise)
    : HttpQueryCommon(std::move(prefix), std::move(promise))
    , account_prefix_{workchain, account}
    , mode_(4)
    , utime_(utime) {
}


BlockSearch::BlockSearch(std::map<std::string, std::string> opts, std::string prefix,
                                           td::Promise<MHD_Response *> promise)
    : HttpQueryCommon(std::move(prefix), std::move(promise)) {
  auto R2 = parse_account_prefix(opts, false);
  if (R2.is_ok()) {
    account_prefix_ = R2.move_as_ok();
  } else {
    error_ = R2.move_as_error();
    return;
  }
  if (opts.count("seqno") + opts.count("lt") + opts.count("utime") != 1) {
    error_ = td::Status::Error(ton::ErrorCode::protoviolation, "exactly one of seqno/lt/utime must be set");
    return;
  }
  if (opts.count("seqno") == 1) {
    try {
      seqno_ = static_cast<td::uint32>(std::stoull(opts["seqno"]));
      mode_ = 1;
    } catch (...) {
      error_ = td::Status::Error("cannot parse seqno");
      return;
    }
  }
  if (opts.count("lt") == 1) {
    try {
      lt_ = std::stoull(opts["lt"]);
      mode_ = 2;
    } catch (...) {
      error_ = td::Status::Error("cannot parse lt");
      return;
    }
  }
  if (opts.count("utime") == 1) {
    try {
      seqno_ = static_cast<td::uint32>(std::stoull(opts["utime"]));
      mode_ = 1;
    } catch (...) {
      error_ = td::Status::Error("cannot parse utime");
      return;
    }
  }
}

void BlockSearch::start_up_query() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &BlockSearch::abort_query, R.move_as_error_prefix("litequery failed: "));
    } else {
      td::actor::send_closure(SelfId, &BlockSearch::got_block_header, R.move_as_ok());
    }
  });
  auto query = ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_lookupBlock>(
                                            mode_,
                                            ton::create_tl_lite_block_id_simple(ton::BlockId{
                                                account_prefix_.workchain, account_prefix_.account_id_prefix, seqno_}),
                                            lt_, utime_),
                                        true);
  td::actor::send_closure(CoreActorInterface::instance_actor_id(), &CoreActorInterface::send_lite_query,
                          std::move(query), std::move(P));
}

void BlockSearch::got_block_header(td::BufferSlice data) {
  auto F = ton::fetch_tl_object<ton::lite_api::liteServer_blockHeader>(std::move(data), true);
  if (F.is_error()) {
    abort_query(F.move_as_error());
    return;
  }
  auto f = F.move_as_ok();
  data_ = std::move(f->header_proof_);
  block_id_ = ton::create_block_id(f->id_);

  if (block_id_.is_masterchain()) {
    auto P_2 = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &BlockSearch::failed_to_get_shard_info,
                                R.move_as_error_prefix("litequery failed: "));
      } else {
        td::actor::send_closure(SelfId, &BlockSearch::got_shard_info, R.move_as_ok());
      }
    });
    auto query_2 = ton::serialize_tl_object(
        ton::create_tl_object<ton::lite_api::liteServer_getAllShardsInfo>(ton::create_tl_lite_block_id(block_id_)),
        true);
    td::actor::send_closure(CoreActorInterface::instance_actor_id(), &CoreActorInterface::send_lite_query,
                            std::move(query_2), std::move(P_2));
    pending_queries_++;
  }

  auto query_3 = ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_listBlockTransactions>(
                                              ton::create_tl_lite_block_id(block_id_), 7, 1024, nullptr, false, false),
                                          true);
  auto P_3 = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &BlockSearch::abort_query, R.move_as_error_prefix("litequery failed: "));
    } else {
      td::actor::send_closure(SelfId, &BlockSearch::got_transactions, R.move_as_ok());
    }
  });
  td::actor::send_closure(CoreActorInterface::instance_actor_id(), &CoreActorInterface::send_lite_query,
                          std::move(query_3), std::move(P_3));
  pending_queries_++;
}

void BlockSearch::got_full_transaction(td::BufferSlice result) {
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

void BlockSearch::start_transaction_query(block::StdAddress addr, ton::LogicalTime lt, ton::Bits256 hash) {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &BlockSearch::abort_query,
                              R.move_as_error_prefix("litequery failed: "));
    } else {
      td::actor::send_closure(SelfId, &BlockSearch::got_full_transaction, R.move_as_ok());
    }
  });
  auto a = ton::create_tl_object<ton::lite_api::liteServer_accountId>(addr.workchain, addr.addr);
  auto query = ton::serialize_tl_object(
      ton::create_tl_object<ton::lite_api::liteServer_getTransactions>(1, std::move(a), lt, hash), true);
  td::actor::send_closure(CoreActorInterface::instance_actor_id(), &CoreActorInterface::send_lite_query,
                          std::move(query), std::move(P));
  pending_queries_++;
}

void BlockSearch::got_transactions(td::BufferSlice data) {
  auto F = ton::fetch_tl_object<ton::lite_api::liteServer_blockTransactions>(std::move(data), true);
  if (F.is_error()) {
    abort_query(F.move_as_error());
    return;
  }
  auto f = F.move_as_ok();
  trans_req_count_ = f->req_count_;

  for(std::size_t i = 0; i < f->ids_.size(); i++) {
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
        td::actor::send_closure(SelfId, &BlockSearch::abort_query,
                                R.move_as_error_prefix("litequery failed: "));
      } else {
        td::actor::send_closure(SelfId, &BlockSearch::got_transactions, R.move_as_ok());
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

void HttpQueryBlockSearch::finish_query() {
  if (promise_) {
    auto page = [&]() -> std::string {
      HttpAnswer A{"blockinfo", prefix_};
      A.set_block_id(block_id_);
      create_header(A);
      if (error_.is_error()) {
        A.abort(error_.move_as_error());
        return A.finish();
      }
      auto res = vm::std_boc_deserialize(data_.clone());
      if (res.is_error()) {
        A.abort(res.move_as_error());
        return A.finish();
      }

      A.serializeObject(HttpAnswer::BlockHeaderCell{block_id_, res.move_as_ok()});

      if (shard_data_.size() > 0) {
        auto R = vm::std_boc_deserialize(shard_data_.clone());
        if (R.is_error()) {
          A.abort(R.move_as_error());
          return A.finish();
        } else {
          A.serializeObject(HttpAnswer::BlockShardsCell{block_id_, R.move_as_ok()});
        }
      }

      UniValue uvArr;
      uvArr.setArray();
      for (auto &tr : transactionsInfo) {
        auto R = vm::std_boc_deserialize_multi(std::move(tr));
        if (R.is_error()) {
          A.abort(R.move_as_error());
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
      A.putInJson("transactions", uvArr);
      transactions_.clear();
      return A.finish();
    }();
    auto R = MHD_create_response_from_buffer(page.length(), const_cast<char *>(page.c_str()), MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(R, "Content-Type", "application/json");
    promise_.set_value(std::move(R));
  }
  stop();
}

