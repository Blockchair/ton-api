#include "../blockchain-api-query.hpp"


HttpQueryViewTransaction2::HttpQueryViewTransaction2(ton::BlockIdExt block_id, block::StdAddress addr,
                                                     ton::LogicalTime lt, std::string prefix,
                                                     td::Promise<MHD_Response *> promise)
    : HttpQueryCommon(std::move(prefix), std::move(promise), std::move(block_id)), addr_(addr), lt_(lt) {
}

HttpQueryViewTransaction2::HttpQueryViewTransaction2(std::map<std::string, std::string> opts, std::string prefix,
                                                     td::Promise<MHD_Response *> promise, bool fromDB, DatabaseConfigParams *dbConfParams)
    : HttpQueryCommon(std::move(prefix), std::move(promise)), fromDB(fromDB) {
  auto R2 = parse_account_addr(opts);
  if (R2.is_ok()) {
    addr_ = R2.move_as_ok();
  } else {
    error_ = R2.move_as_error();
    return;
  }
  try {
    lt_ = std::stoull(opts["lt"]);
  } catch (...) {
    error_ = td::Status::Error("cannot trans parse lt");
    return;
  }
  try {
    auto h = opts["hash"];
    if (h.length() != 64) {
      error_ = td::Status::Error("cannot trans parse hash");
      return;
    }
    auto R = td::hex_decode(td::Slice(h));
    if (R.is_error()) {
      error_ = td::Status::Error("cannot trans parse hash");
      return;
    }
    hash_.as_slice().copy_from(R.move_as_ok());
  } catch (...) {
    error_ = td::Status::Error("cannot trans parse hash");
    return;
  }
  if(fromDB && !dbConfParams) {
    error_ = td::Status::Error("DB connection error");
    return;
  }
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
  }
}

#define SELECTTRBD(account, hs, lt) \
  std::string(std::string("select encode(blob, 'hex') from ton_transaction where \
account = decode('") + account +            \
              std::string("', 'hex') and hash = decode('") + hs + \
              std::string("', 'hex') and logical_time =      ") + std::to_string(lt)).c_str()

void HttpQueryViewTransaction2::readFromFile(td::BufferSlice &data) {
  
  if (conn != nullptr) {
    mtx->lock();
    std::unique_ptr<PGresult, decltype(&PQclear)> resSQL(
        PQexec(conn.get(), SELECTTRBD(addr_.addr.to_hex(), hash_.to_hex(), lt_)), &PQclear);
    mtx->unlock();
    // LOG(INFO) << SELECTTRBD(addr_.addr.to_hex(), hash_.to_hex(), lt_);
    if (PQresultStatus(resSQL.get()) != PGRES_TUPLES_OK) {
      error_ = td::Status::Error("Select failed: " + std::string(PQresultErrorMessage(resSQL.get())));
      // std::cout << "service error: " + std::string(PQresultErrorMessage(resSQL.get())) << '\n';
      resSQL.reset();
      finish_query();
    } else {
      std::string dt = td::hex_decode(td::Slice(std::string(PQgetvalue(resSQL.get(), 0, 0)))).move_as_ok();
      data = td::BufferSlice(dt.c_str(), dt.size());
    }
  } else {
    error_ = td::Status::Error(404, PSTRING() << "unnable to connect to db");
    finish_query();
  }
  
}

void HttpQueryViewTransaction2::start_up_query() {
  if (fromDB) {
    readFromFile(data_);
    finish_query();
  } else {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &HttpQueryViewTransaction2::abort_query,
                                R.move_as_error_prefix("litequery failed: "));
      } else {
        td::actor::send_closure(SelfId, &HttpQueryViewTransaction2::got_transaction, R.move_as_ok());
      }
    });
    auto a = ton::create_tl_object<ton::lite_api::liteServer_accountId>(addr_.workchain, addr_.addr);
    auto query = ton::serialize_tl_object(
        ton::create_tl_object<ton::lite_api::liteServer_getTransactions>(1, std::move(a), lt_, hash_), true);
    td::actor::send_closure(CoreActorInterface::instance_actor_id(), &CoreActorInterface::send_lite_query,
                            std::move(query), std::move(P));
  }
}

void HttpQueryViewTransaction2::got_transaction(td::BufferSlice data) {
  auto F = ton::fetch_tl_object<ton::lite_api::liteServer_transactionList>(std::move(data), true);
  if (F.is_error()) {
    abort_query(F.move_as_error());
    return;
  }

  auto f = F.move_as_ok();
  data_ = std::move(f->transactions_);
  if (f->ids_.size() == 0) {
    abort_query(td::Status::Error("no transactions found"));
    return;
  }
  block_id_ = ton::create_block_id(f->ids_[0]);

  finish_query();
}

void HttpQueryViewTransaction2::finish_query() {
  if (promise_) {
    auto page = [&]() -> std::string {
      HttpAnswer A{"transaction", prefix_};
      if (error_.is_error()) {
        A.abort(error_.move_as_error());
        return A.finish();
      }
      A.set_block_id(block_id_);
      A.set_account_id(addr_);
      auto R = vm::std_boc_deserialize_multi(std::move(data_));
      if (R.is_error()) {
        A.abort(PSTRING() << "FATAL: cannot deserialize transactions BoC");
        return A.finish();
      }
      auto list = R.move_as_ok();
      A.serializeObject(HttpAnswer::TransactionCell{addr_, block_id_, list[0]});
      return A.finish();
    }();
    auto R = MHD_create_response_from_buffer(page.length(), const_cast<char *>(page.c_str()), MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(R, "Content-Type", "application/json");
    promise_.set_value(std::move(R));
  }
  stop();
}