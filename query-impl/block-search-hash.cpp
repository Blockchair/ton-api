#include "../blockchain-api-query.hpp"

HttpQueryBlockSearchHash::HttpQueryBlockSearchHash(std::map<std::string, std::string> opts, std::string prefix,
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
}

void HttpQueryBlockSearchHash::start_up_query() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &HttpQueryBlockSearchHash::abort_query, R.move_as_error_prefix("litequery failed: "));
    } else {
      td::actor::send_closure(SelfId, &HttpQueryBlockSearchHash::got_block_header, R.move_as_ok());
    }
  });
  auto query = ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_lookupBlock>(
                                            1,
                                            ton::create_tl_lite_block_id_simple(ton::BlockId{
                                                account_prefix_.workchain, account_prefix_.account_id_prefix, seqno_}),
                                            0, 0),
                                        true);
  td::actor::send_closure(CoreActorInterface::instance_actor_id(), &CoreActorInterface::send_lite_query,
                          std::move(query), std::move(P));
  pending_queries_++;
}

void HttpQueryBlockSearchHash::finish_query() {
  if (promise_) {
    auto page = [&]() -> std::string {
      HttpAnswer A{"lastBlock", prefix_};
      if(error_.is_error()) {
        A.abort(error_.move_as_error());
        return A.finish();
      }
      A.set_block_id(block_id_);
      A.putInJson("block", A.serializeObject(HttpAnswer::BlockLink{block_id_}));
      return A.finish();
    }();
    auto R = MHD_create_response_from_buffer(page.length(), const_cast<char *>(page.c_str()), MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(R, "Content-Type", "application/json");
    promise_.set_value(std::move(R));
  }
  stop();
}
