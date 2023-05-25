#include "../blockchain-api-query.hpp"


HttpQueryViewTransaction::HttpQueryViewTransaction(block::StdAddress addr, ton::LogicalTime lt, ton::Bits256 hash,
                                                   std::string prefix, td::Promise<MHD_Response *> promise)
    : HttpQueryCommon(std::move(prefix), std::move(promise)), addr_(addr), lt_(lt), hash_(hash) {
      uvArr.setArray();
}

HttpQueryViewTransaction::HttpQueryViewTransaction(std::map<std::string, std::string> opts, std::string prefix,
                                                   td::Promise<MHD_Response *> promise, bool fromDB, DatabaseConfigParams *dbConfParams)
    : HttpQueryCommon(std::move(prefix), std::move(promise)), fromDB(fromDB) {
  auto R2 = parse_account_addr(opts);
  if (R2.is_ok()) {
    addr_ = R2.move_as_ok();
  } else {
    error_ = R2.move_as_error();
    return;
  }
  auto it = opts.find("count");
  if (it != opts.end()) {
    try {
      count = std::stoi(it->second);
    } catch (...) {
      error_ = td::Status::Error(ton::ErrorCode::error, "bad count");
      return;
    }
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
  try {
    auto codeIt = opts.find("code");
    if(codeIt != opts.end()) {
      code_ = true;
    }
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
  uvArr.setArray();
}

void HttpQueryViewTransaction::start_up_query() {
  if (fromDB) {
    readTransactionsFromDB();
    finish_query();
  } else {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &HttpQueryViewTransaction::abort_query,
                                R.move_as_error_prefix("litequery failed: "));
      } else {
        td::actor::send_closure(SelfId, &HttpQueryViewTransaction::got_transaction, R.move_as_ok());
      }
    });
    auto a = ton::create_tl_object<ton::lite_api::liteServer_accountId>(addr_.workchain, addr_.addr);
    auto query = ton::serialize_tl_object(
        ton::create_tl_object<ton::lite_api::liteServer_getTransactions>(count, std::move(a), lt_, hash_), true);
    td::actor::send_closure(CoreActorInterface::instance_actor_id(), &CoreActorInterface::send_lite_query,
                            std::move(query), std::move(P));
  }
}

void HttpQueryViewTransaction::got_transaction(td::BufferSlice data) {
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
  res_block_id_ = ton::create_block_id(f->ids_[0]);

  unpackRestart();
}

#define SELECTTRBD(account, lt, count)                                                                                        \
  std::string(                                                                                                                \
      "select blob, workchain, shard_id, seqno, encode(filehash, 'hex') as filehash, encode(roothash, 'hex') as   \
roothash from (select encode(blob, 'hex') as blob, block, logical_time from ton_transaction tt where tt.account = decode('" + \
      account +                                                                                                               \
      "', 'hex')  \
and tt.logical_time <=" +                                                                                                     \
      std::to_string(lt) +                                                                                                    \
      " ) as tt \
join ton_block tb on  \
tt.block = tb.id  \
order by tt.logical_time desc limit " +                                                                                       \
      std::to_string(count) + ";")                                                                                            \
      .c_str()

void HttpQueryViewTransaction::readTransactionsFromDB() {
  if (conn != nullptr) {
    mtx->lock();
    std::unique_ptr<PGresult, decltype(&PQclear)> resSQL(
        PQexec(conn.get(), SELECTTRBD(addr_.addr.to_hex(), lt_, count)), &PQclear);
    mtx->unlock();
    // LOG(INFO) << SELECTTRBD(addr_.addr.to_hex(), lt_, count);
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
              dt = td::hex_decode(td::Slice(std::string(PQgetvalue(resSQL.get(), i, j)))).move_as_ok();
              break;
            case 1:
              res_block_id_.id.workchain = std::atoi(PQgetvalue(resSQL.get(), i, j));
              break;
            case 2:
              res_block_id_.id.shard = std::stoll(PQgetvalue(resSQL.get(), i, j));
              break;
            case 3:
              res_block_id_.id.seqno = static_cast<unsigned int>(std::stoul(PQgetvalue(resSQL.get(), i, j)));
              break;
            case 4:
              x.from_hex(td::Slice(PQgetvalue(resSQL.get(), i, j)));
              res_block_id_.file_hash = x;
              break;
            case 5:
              y.from_hex(td::Slice(PQgetvalue(resSQL.get(), i, j)));
              res_block_id_.root_hash = y;
              break;
            default:
              break;
          }
        }
        unpackFromDB(dt, res_block_id_);
      }
      
    }
  } else {
    error_ = td::Status::Error(404, PSTRING() << "unnable to connect to db");
    finish_query();
  }
}

void HttpQueryViewTransaction::unpackFromDB(std::string data, ton::BlockIdExt res_block_id_) {
  HttpAnswer A{"transaction", prefix_, code_};
  auto R = vm::std_boc_deserialize_multi(std::move(td::BufferSlice(data.c_str(), data.size())));
  if (R.is_error()) {
    error_ = td::Status::Error(404, PSTRING() << "error of unpacking query");
    finish_query();
  }
  auto list = R.move_as_ok();
  UniValue uvObj;
  uvObj.setObject();
  A.serializeObject(HttpAnswer::TransactionSmall{res_block_id_, list[0]}, uvObj);
  uvArr.push_back(uvObj);
}

void HttpQueryViewTransaction::unpackRestart() {
  bool goStart = false;
  if (promise_) {
    HttpAnswer A{"transaction", prefix_, code_};
    HttpAnswer::TransactionDescr transDescr;
    A.set_block_id(res_block_id_);
    A.set_account_id(addr_);
    auto R = vm::std_boc_deserialize_multi(std::move(data_));
    if (R.is_error()) {
      error_ = td::Status::Error(ton::ErrorCode::error, "bad count");
    }
    auto list = R.move_as_ok();
    if (!this->firstTryToGetTransactions) {
      list.erase(list.begin());
    }
    auto n = list.size();
    if(n == 0) {
      finish_query();
    }

    for (std::size_t i = 0; i < n && this->count > 0; i++, this->count--) {
      UniValue uvObj;
      uvObj.setObject();
      transDescr = A.serializeTransactionSmall(HttpAnswer::TransactionSmall{res_block_id_, list[i]}, uvObj);
      uvArr.push_back(uvObj);
    }

    if (this->count == 1 && lastCountTry) {
      finish_query();
    } else {
      lastCountTry = true;
      this->count++;
    }

    if (this->count != 0 && n != 1) {
      lt_ = transDescr.lt;
      hash_ = transDescr.hash;
      addr_ = transDescr.addr;
      this->firstTryToGetTransactions = false;
      goStart = true;
    } else {
      finish_query();
    }
    if (goStart) {
      start_up_query();
    }
  } else {
    finish_query();
  }
}

void HttpQueryViewTransaction::finish_query() {
  auto page = [&]() -> std::string {
    HttpAnswer A{"transaction", prefix_};
    if (error_.is_error()) {
      A.abort(error_.move_as_error());
      return A.finish();
    }
    A.set_block_id(block_id_);
    A.putInJson("answer", uvArr);
    return A.finish();
  }();
  auto R = MHD_create_response_from_buffer(page.length(), const_cast<char *>(page.c_str()), MHD_RESPMEM_MUST_COPY);
  MHD_add_response_header(R, "Content-Type", "application/json");
  promise_.set_value(std::move(R));
  stop();
}
