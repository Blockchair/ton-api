#include "../blockchain-api-query.hpp"


HttpQueryViewLastBlock::HttpQueryViewLastBlock(std::string prefix, td::Promise<MHD_Response *> promise)
    : HttpQueryCommon(std::move(prefix), std::move(promise)) {
}

HttpQueryViewLastBlock::HttpQueryViewLastBlock(std::map<std::string, std::string> opts, std::string prefix,
                                               td::Promise<MHD_Response *> promise)
    : HttpQueryCommon(std::move(prefix), std::move(promise)) {
}

void HttpQueryViewLastBlock::start_up() {
  if (error_.is_error()) {
    abort_query(std::move(error_));
    return;
  }
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &HttpQueryViewLastBlock::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &HttpQueryViewLastBlock::got_result, R.move_as_ok());
    }
  });

  auto query = ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_getMasterchainInfo>(), true);
  td::actor::send_closure(CoreActorInterface::instance_actor_id(), &CoreActorInterface::send_lite_query,
                          std::move(query), std::move(P));
}

void HttpQueryViewLastBlock::got_result(td::BufferSlice data) {
  auto F = ton::fetch_tl_object<ton::lite_api::liteServer_masterchainInfo>(std::move(data), true);
  if (F.is_error()) {
    abort_query(F.move_as_error());
    return;
  }
  auto f = F.move_as_ok();
  res_block_id_ = ton::create_block_id(f->last_);

  finish_query();
}

void HttpQueryViewLastBlock::finish_query() {
  if (promise_) {
    td::actor::create_actor<HttpQueryBlockInfo>("blockinfo", res_block_id_, prefix_, std::move(promise_)).release();
  }
  stop();
}

HttpQueryViewLastBlockNumber::HttpQueryViewLastBlockNumber(std::string prefix, td::Promise<MHD_Response *> promise)
    : HttpQueryCommon(std::move(prefix), std::move(promise)) {
}

HttpQueryViewLastBlockNumber::HttpQueryViewLastBlockNumber(std::map<std::string, std::string> opts, std::string prefix,
                                                           td::Promise<MHD_Response *> promise, bool fromDB,
                                                           DatabaseConfigParams *dbConfParams)
    : HttpQueryCommon(std::move(prefix), std::move(promise)) {
  if (fromDB && dbConfParams) {
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
    this->fromDb = true;
  }
}

void HttpQueryViewLastBlockNumber::start_up() {
  if (error_.is_error()) {
    abort_query(std::move(error_));
    return;
  }
  if (fromDb) {
    getFromDB();
    get_shards();
  } else {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &HttpQueryViewLastBlockNumber::abort_query, R.move_as_error());
      } else {
        td::actor::send_closure(SelfId, &HttpQueryViewLastBlockNumber::got_result, R.move_as_ok());
      }
    });

    auto query = ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_getMasterchainInfo>(), true);
    td::actor::send_closure(CoreActorInterface::instance_actor_id(), &CoreActorInterface::send_lite_query,
                            std::move(query), std::move(P));

    pending_queries_++;
  }
}

void HttpQueryViewLastBlockNumber::get_shards() {
auto P_2 = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &HttpQueryViewLastBlockNumber::failed_to_get_shard_info,
                                R.move_as_error_prefix("litequery failed: "));
      } else {
        td::actor::send_closure(SelfId, &HttpQueryViewLastBlockNumber::got_shard_info, R.move_as_ok());
      }
    });
    auto query_2 = ton::serialize_tl_object(
        ton::create_tl_object<ton::lite_api::liteServer_getAllShardsInfo>(ton::create_tl_lite_block_id(res_block_id_)),
        true);
    td::actor::send_closure(CoreActorInterface::instance_actor_id(), &CoreActorInterface::send_lite_query,
                            std::move(query_2), std::move(P_2));
  pending_queries_++;
}


void HttpQueryViewLastBlockNumber::got_result(td::BufferSlice data) {
  auto F = ton::fetch_tl_object<ton::lite_api::liteServer_masterchainInfo>(std::move(data), true);
  if (F.is_error()) {
    abort_query(F.move_as_error());
    return;
  }
  auto f = F.move_as_ok();
  res_block_id_ = ton::create_block_id(f->last_);
  pending_queries_--;
  get_shards();
}

void HttpQueryViewLastBlockNumber::finish_query() {
  if (promise_) {
    auto page = [&]() -> std::string {
      HttpAnswer A{"lastBlock", prefix_};
      A.set_block_id(res_block_id_);
      if (error_.is_error()) {
        A.abort(error_.move_as_error());
        return A.finish();
      }
      auto R = vm::std_boc_deserialize(shard_data_.clone());
      if (R.is_error()) {
        A.abort(PSTRING() << "cannot deserialize shard configuration: " << R.move_as_error());
        return A.finish();
      } else {
        A.serializeObject(HttpAnswer::BlockShardsCellSmall{res_block_id_, R.move_as_ok()});
      }
      return A.finish();
    }();
    auto R = MHD_create_response_from_buffer(page.length(), const_cast<char *>(page.c_str()), MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(R, "Content-Type", "application/json");
    promise_.set_value(std::move(R));
  }
  stop();
}
