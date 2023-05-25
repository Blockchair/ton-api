#include "blockchain-api-query.hpp"
#include <typeinfo>

// namespace {

td::Ref<vm::Tuple> prepare_vm_c7(ton::UnixTime now, ton::LogicalTime lt, td::Ref<vm::CellSlice> my_addr,
                                 const block::CurrencyCollection &balance) {
  td::BitArray<256> rand_seed;
  td::RefInt256 rand_seed_int{true};
  td::Random::secure_bytes(rand_seed.as_slice());
  if (!rand_seed_int.unique_write().import_bits(rand_seed.cbits(), 256, false)) {
    return {};
  }
  auto tuple = vm::make_tuple_ref(td::make_refint(0x076ef1ea),  // [ magic:0x076ef1ea
                                  td::make_refint(0),           //   actions:Integer
                                  td::make_refint(0),           //   msgs_sent:Integer
                                  td::make_refint(now),         //   unixtime:Integer
                                  td::make_refint(lt),          //   block_lt:Integer
                                  td::make_refint(lt),          //   trans_lt:Integer
                                  std::move(rand_seed_int),     //   rand_seed:Integer
                                  balance.as_vm_tuple(),        //   balance_remaining:[Integer (Maybe Cell)]
                                  my_addr,                      //  myself:MsgAddressInt
                                  vm::StackEntry());            //  global_config:(Maybe Cell) ] = SmartContractInfo;
  LOG(DEBUG) << "SmartContractInfo initialized with " << vm::StackEntry(tuple).to_string();
  return vm::make_tuple_ref(std::move(tuple));
}

// }  // namespace

td::Result<ton::BlockIdExt> parse_block_id(std::map<std::string, std::string> &opts, bool allow_empty) {
  if (allow_empty) {
    if (opts.count("workchain") == 0 && opts.count("shard") == 0 && opts.count("seqno") == 0) {
      return ton::BlockIdExt{};
    }
  }
  try {
    ton::BlockIdExt block_id;
    auto it = opts.find("workchain");
    if (it == opts.end()) {
      return td::Status::Error(ton::ErrorCode::protoviolation, "workchain not set");
    }
    block_id.id.workchain = std::stoi(it->second);
    it = opts.find("shard");
    if (it == opts.end()) {
      return td::Status::Error(ton::ErrorCode::protoviolation, "shard not set");
    }
    block_id.id.shard = std::stoull(it->second, nullptr, 16);
    it = opts.find("seqno");
    if (it == opts.end()) {
      return td::Status::Error(ton::ErrorCode::protoviolation, "seqno not set");
    }
    auto s = std::stoull(it->second);
    auto seqno = static_cast<ton::BlockSeqno>(s);
    if (s != seqno) {
      return td::Status::Error(ton::ErrorCode::protoviolation, "seqno too big");
    }
    block_id.id.seqno = seqno;
    it = opts.find("roothash");
    if (it == opts.end()) {
      return td::Status::Error(ton::ErrorCode::protoviolation, "roothash not set");
    }
    if (it->second.length() != 64) {
      return td::Status::Error(ton::ErrorCode::protoviolation, "roothash bad length");
    }
    auto R = td::hex_decode(td::Slice(it->second));
    if (R.is_error()) {
      return td::Status::Error(ton::ErrorCode::protoviolation, "roothash bad hex");
    }
    block_id.root_hash.as_slice().copy_from(td::as_slice(R.move_as_ok()));
    it = opts.find("filehash");
    if (it == opts.end()) {
      return td::Status::Error(ton::ErrorCode::protoviolation, "filehash not set");
    }
    if (it->second.length() != 64) {
      return td::Status::Error(ton::ErrorCode::protoviolation, "filehash bad length");
    }
    R = td::hex_decode(td::Slice(it->second));
    if (R.is_error()) {
      return td::Status::Error(ton::ErrorCode::protoviolation, "filehash bad hex");
    }
    block_id.file_hash.as_slice().copy_from(td::as_slice(R.move_as_ok()));
    return block_id;
  } catch (...) {
    return td::Status::Error(ton::ErrorCode::protoviolation, "cannot parse int");
  }
}

td::Result<ton::AccountIdPrefixFull> parse_account_prefix(std::map<std::string, std::string> &opts, bool allow_empty) {
  if (allow_empty) {
    if (opts.count("workchain") == 0 && opts.count("shard") == 0 && opts.count("account") == 0) {
      return ton::AccountIdPrefixFull{ton::masterchainId, 0};
    }
  }
  try {
    ton::AccountIdPrefixFull account_id;
    auto it = opts.find("workchain");
    if (it == opts.end()) {
      return td::Status::Error(ton::ErrorCode::protoviolation, "workchain not set");
    }
    account_id.workchain = std::stoi(it->second);
    it = opts.find("shard");
    if (it == opts.end()) {
      it = opts.find("account");
      if (it == opts.end()) {
        return td::Status::Error(ton::ErrorCode::protoviolation, "shard/account not set");
      }
    }
    account_id.account_id_prefix = std::stoull(it->second, nullptr, 16);
    return account_id;
  } catch (...) {
    return td::Status::Error(ton::ErrorCode::protoviolation, "cannot parse int");
  }
}

td::Result<block::StdAddress> parse_account_addr(std::map<std::string, std::string> &opts) {
  auto it = opts.find("account");
  if (it == opts.end()) {
    return td::Status::Error(ton::ErrorCode::error, "no account id");
  }
  std::string acc_string = it->second;
  block::StdAddress a;
  if (a.parse_addr(td::Slice(acc_string))) {
    return a;
  }
  ton::WorkchainId workchain_id;
  it = opts.find("accountworkchain");
  if (it == opts.end()) {
    it = opts.find("workchain");
    if (it == opts.end()) {
      return td::Status::Error(ton::ErrorCode::error, "no account workchain id");
    }
  }
  try {
    workchain_id = std::stoi(it->second);
  } catch (...) {
    return td::Status::Error(ton::ErrorCode::error, "bad account workchain id");
  }
  if (acc_string.size() == 64) {
    TRY_RESULT(R, td::hex_decode(acc_string));
    a.addr.as_slice().copy_from(td::Slice(R));
    a.workchain = workchain_id;
    return a;
  }
  return td::Status::Error(ton::ErrorCode::error, "bad account id");
}

void HttpQueryCommon::abort_query(td::Status error) {
  if (promise_) {
    HttpAnswer A{"error", prefix_};
    A.abort(std::move(error));
    auto page = A.finish();
    auto R = MHD_create_response_from_buffer(page.length(), const_cast<char *>(page.c_str()), MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(R, "Content-Type", "application/json");
    promise_.set_value(std::move(R));
  }
  stop();
}

void HttpQueryCommon::got_shard_info(td::BufferSlice result) {
  auto F = ton::fetch_tl_object<ton::lite_api::liteServer_allShardsInfo>(std::move(result), true);
  if (F.is_error()) {
    abort_query(F.move_as_error());
    return;
  }
  shard_data_ = std::move(F.move_as_ok()->data_);

  if (!--pending_queries_) {
    finish_query();
  }
}

#define CHECKIFERROR(F, ELSE)       \
  if (F.is_error()) {               \
    abort_query(F.move_as_error()); \
  } else                            \
    ELSE

void HttpQueryCommon::got_block_header(td::BufferSlice data) {
  if (std::string(typeid(*this).name()).find("HttpQueryBlockSearchHash") != std::string::npos) {
    auto F = ton::fetch_tl_object<ton::lite_api::liteServer_blockHeader>(std::move(data), true);
    CHECKIFERROR(F, {
      auto f = F.move_as_ok();
      data_ = std::move(f->header_proof_);
      block_id_ = ton::create_block_id(f->id_);
    })
  }
  if (std::string(typeid(*this).name()).find("HttpQueryBlockInfo") != std::string::npos ) {
    auto F = ton::fetch_tl_object<ton::lite_api::liteServer_blockData>(std::move(data), true);
    CHECKIFERROR(F, {
      auto f = F.move_as_ok();
      data_ = std::move(f->data_);
    })
  }

  if (!--pending_queries_) {
    finish_query();
  }
}

void HttpQueryCommon::next(td::Status error) {
  pending_queries_ -= 1;
  if (pending_queries_ == 0) {
    finish_query();
  }
}

void HttpQueryCommon::failed_to_get_shard_info(td::Status error) {
  error_ = std::move(error);
  if (!--pending_queries_) {
    finish_query();
  }
}

