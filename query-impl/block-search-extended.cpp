#include "../blockchain-api-query.hpp"

HttpQueryBlockSearch2::HttpQueryBlockSearch2(std::map<std::string, std::string> opts, std::string prefix,
                                           td::Promise<MHD_Response *> promise, std::shared_ptr<td::actor::Scheduler> scheduler_ptr_in)
    : HttpQueryCommon(std::move(prefix), std::move(promise)) {
  auto R2 = parse_account_prefix(opts, false);
  if (R2.is_ok()) {
    account_prefix_ = R2.move_as_ok();
  } else {
    error_ = R2.move_as_error();
    return;
  }
  if (opts.count("seqnoFrom") == 1) {
    try {
      seqnoFrom = static_cast<td::uint32>(std::stoull(opts["seqnoFrom"]));
      start = seqnoFrom;
    } catch (...) {
      error_ = td::Status::Error("cannot parse seqno");
      return;
    }
  }
  if (opts.count("seqnoTo") == 1) {
    try {
      seqnoTo = static_cast<td::uint32>(std::stoull(opts["seqnoTo"]));
    } catch (...) {
      error_ = td::Status::Error("cannot parse seqno");
      return;
    }
  }
  scheduler_ptr2 = scheduler_ptr_in;
}

void HttpQueryBlockSearch2::start_up_query() {
  for (ton::BlockSeqno i = seqnoFrom; i <= seqnoTo; i++) {
    SearchBlockRunner g{[&](td::Promise<std::unique_ptr<BlockData>> promise) {
      td::actor::create_actor<HttpQueryBlockSearchNoMHD>("blocksearchN", account_prefix_.workchain,
      account_prefix_.account_id_prefix, i, "test", std::move(promise)).release();
    }, scheduler_ptr2};
    dataTo.push_back(g.wait());
  }
  finish_query();
}

void HttpQueryBlockSearch2::finish_query() {
  if (promise_) {
    auto page = [&]() -> std::string {
      HttpAnswer A{"blockinfo", prefix_};
      create_header(A);
      UniValue uvArr;
      uvArr.setArray();
      for(auto &bd : dataTo) {
        UniValue uvObjBD;
        uvObjBD.setObject();
        auto res = vm::std_boc_deserialize(bd->data_.clone());
        // check for null
        if (res.is_error()) {
          A.abort(PSTRING() << "cannot deserialize block header data: " << res.move_as_error());
          return A.finish();
        }
        uvObjBD.pushKV("header", A.serializeBlockHeaderCellLite(HttpAnswer::BlockHeaderCell{bd->block_id_, res.move_as_ok()}));

        if (bd->shard_data_.size() > 0) {
          auto R = vm::std_boc_deserialize(bd->shard_data_.clone());
          if (R.is_error()) {
            A.abort(PSTRING() << "cannot deserialize shard configuration: " << R.move_as_error());
            return A.finish();
          } else {
            uvObjBD.pushKV("shards", A.serializeBlockShardsCellLite(HttpAnswer::BlockShardsCell{bd->block_id_, R.move_as_ok()}));
          }
        }
        UniValue uvArrT;
        uvArrT.setArray();
        for (auto &tr : bd->transactionsInfo) {
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
            A.serializeObject(HttpAnswer::TransactionSmall{std::move(bd->block_id_), std::move(list[0])}, uvObj);     // here some problems by valgrind
            uvArrT.push_back(uvObj);
          }
        }
        uvObjBD.pushKV("transactions", uvArrT);
        uvArr.push_back(uvObjBD);
        bd->transactionsInfo.clear();
        bd->data_.clear();
        bd->shard_data_.clear();
        bd.reset();
      }
      A.putInJson("answer", uvArr);
      return A.finish();
    }();
    dataTo.clear();
    auto R = MHD_create_response_from_buffer(page.length(), const_cast<char *>(page.c_str()), MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(R, "Content-Type", "application/json");
    promise_.set_value(std::move(R));
  }
  stop();
}