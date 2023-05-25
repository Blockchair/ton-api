#include "../blockchain-api-query.hpp"


HttpQueryViewAccount::HttpQueryViewAccount(ton::BlockIdExt block_id, block::StdAddress addr, std::string prefix,
                                           td::Promise<MHD_Response *> promise)
    : HttpQueryCommon(std::move(prefix), std::move(promise), std::move(block_id)), addr_(addr) {
}

HttpQueryViewAccount::HttpQueryViewAccount(std::map<std::string, std::string> opts, std::string prefix,
                                           td::Promise<MHD_Response *> promise)
    : HttpQueryCommon(std::move(prefix), std::move(promise)) {
  auto R = parse_block_id(opts, true);
  if (R.is_ok()) {
    block_id_ = R.move_as_ok();
    if (!block_id_.is_valid()) {
      block_id_.id.workchain = ton::masterchainId;
      block_id_.id.shard = ton::shardIdAll;
      block_id_.id.seqno = static_cast<td::uint32>(0xffffffff);
      block_id_.root_hash.set_zero();
      block_id_.file_hash.set_zero();
    }
  } else {
    error_ = R.move_as_error();
    return;
  }
  auto R2 = parse_account_addr(opts);
  if (R2.is_ok()) {
    addr_ = R2.move_as_ok();
  } else {
    error_ = R2.move_as_error();
    return;
  }
}

void HttpQueryViewAccount::start_up_query() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &HttpQueryViewAccount::abort_query, R.move_as_error_prefix("litequery failed: "));
    } else {
      td::actor::send_closure(SelfId, &HttpQueryViewAccount::got_account, R.move_as_ok());
    }
  });
  auto a = ton::create_tl_object<ton::lite_api::liteServer_accountId>(addr_.workchain, addr_.addr);
  auto query = ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_getAccountState>(
                                            ton::create_tl_lite_block_id(block_id_), std::move(a)),
                                        true);
  td::actor::send_closure(CoreActorInterface::instance_actor_id(), &CoreActorInterface::send_lite_query,
                          std::move(query), std::move(P));
}

void HttpQueryViewAccount::got_account(td::BufferSlice data) {
  auto F = ton::fetch_tl_object<ton::lite_api::liteServer_accountState>(std::move(data), true);
  if (F.is_error()) {
    abort_query(F.move_as_error());
    return;
  }

  auto f = F.move_as_ok();
  data_ = std::move(f->state_);
  proof_ = std::move(f->proof_);
  res_block_id_ = ton::create_block_id(f->shardblk_);

  finish_query();
}

void HttpQueryViewAccount::finish_query() {
  if (promise_) {
    auto page = [&]() -> std::string {
      HttpAnswer A{"account", prefix_};
      if(error_.is_error()) {
        abort_query(error_.move_as_error());
      }
      A.set_account_id(addr_);
      A.set_block_id(res_block_id_);
      auto R = vm::std_boc_deserialize(data_.clone());
      // LOG(INFO) << (int)data_[0];
      if (R.is_error()) {
        A.abort(PSTRING() << "FATAL: cannot deserialize account state" << R.move_as_error());
        return A.finish();
      }
      auto Q = vm::std_boc_deserialize_multi(proof_.clone());
      if (Q.is_error()) {
        A.abort(PSTRING() << "FATAL: cannot deserialize account proof" << Q.move_as_error());
        return A.finish();
      }
      auto Q_roots = Q.move_as_ok();
      auto root = R.move_as_ok();
      A.serializeObject(HttpAnswer::AccountCell{addr_, res_block_id_, root, Q_roots});
      return A.finish();
    }();
    auto R = MHD_create_response_from_buffer(page.length(), const_cast<char *>(page.c_str()), MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(R, "Content-Type", "application/json");
    promise_.set_value(std::move(R));
  }
  stop();
}
