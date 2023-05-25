#include "../blockchain-api-query.hpp"


HttpQuerySearchByHeight::HttpQuerySearchByHeight(std::map<std::string, std::string> opts, std::string prefix,
                                           td::Promise<MHD_Response *> promise, DatabaseConfigParams *dbConfParams)
    : HttpQueryCommon(std::move(prefix), std::move(promise)) {
  if (opts.count("workchain") == 1) {
    try {
      account_prefix_.workchain = static_cast<td::uint32>(std::stoull(opts["workchain"]));
      mode_ = 1;
    } catch (...) {
      error_ = td::Status::Error("cannot parse workchain");
      return;
    }
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

#define SELECTShards(height, wc)                                                                               \
  std::string(                                                                                                 \
      "select shard_id, encode(filehash, 'hex') as filehash, encode(roothash, 'hex') as roothash from ton_block tb where " \
      "tb.workchain = " +                                                                                      \
      std::to_string(wc) + " and tb.seqno = " + std::to_string(height))                                            \
      .c_str()

void HttpQuerySearchByHeight::find_shards() {
  std::string incomeShard;
  if (conn != nullptr) {
    mtx->lock();
    std::unique_ptr<PGresult, decltype(&PQclear)> resSQL(
        PQexec(conn.get(), SELECTShards(seqno_, account_prefix_.workchain)), &PQclear);
    mtx->unlock();
    if (PQresultStatus(resSQL.get()) != PGRES_TUPLES_OK) {
      error_ = td::Status::Error("Select failed: " + std::string(PQresultErrorMessage(resSQL.get())));
      resSQL.reset();
      finish_query();
    } else {
      for (int i = 0; i < PQntuples(resSQL.get()); i++) {
        auto x = ton::FileHash();
        auto y = ton::RootHash();
        std::string dt;
        ton::BlockIdExt res_block_id_;
        for (int j = 0; j < PQnfields(resSQL.get()); j++) {
          switch (j) {
            case 0:
              res_block_id_.id.shard = std::stoll(PQgetvalue(resSQL.get(), i, j));
              break;
            case 1:
              x.from_hex(td::Slice(PQgetvalue(resSQL.get(), i, j)));
              res_block_id_.file_hash = x;
              break;
            case 2:
              y.from_hex(td::Slice(PQgetvalue(resSQL.get(), i, j)));
              res_block_id_.root_hash = y;
              break;
            default:
              break;
          }
        }
        res_block_id_.id.seqno = seqno_;
        res_block_id_.id.workchain = account_prefix_.workchain;
        block_id_.push_back(res_block_id_);
      }
    }
  } else {
    error_ = td::Status::Error(404, PSTRING() << "unnable to connect to db");
  }
}

void HttpQuerySearchByHeight::start_up_query() {
  if (error_.is_error()) {
    finish_query();
  }
  find_shards();
  finish_query();
}

void HttpQuerySearchByHeight::finish_query() {
  if (promise_) {
    auto page = [&]() -> std::string {
      HttpAnswer A{"lastBlock", prefix_};
      if (error_.is_error()) {
        A.abort(error_.move_as_error());
        return A.finish();
      }
      A.unpackBlockVectorForOneChain(block_id_);
      data_.clear();
      block_id_.clear();
      shards.clear();
      return A.finish();
    }();
    auto R = MHD_create_response_from_buffer(page.length(), const_cast<char *>(page.c_str()), MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(R, "Content-Type", "application/json");
    promise_.set_value(std::move(R));
  }
  stop();
}
