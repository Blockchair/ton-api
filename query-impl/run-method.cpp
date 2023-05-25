#include "../blockchain-api-query.hpp"

HttpQueryRunMethod::HttpQueryRunMethod(ton::BlockIdExt block_id, block::StdAddress addr, std::string method_name,
                                       std::vector<vm::StackEntry> params, std::string prefix,
                                       td::Promise<MHD_Response *> promise)
    : HttpQueryCommon(std::move(prefix), std::move(promise), std::move(block_id))
    , addr_(addr)
    , method_name_(std::move(method_name))
    , params_(std::move(params)) {
}

HttpQueryRunMethod::HttpQueryRunMethod(std::map<std::string, std::string> opts, std::string prefix,
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
  auto it = opts.find("method");
  if (it == opts.end()) {
    error_ = td::Status::Error("no method");
    return;
  } else {
    method_name_ = it->second;
  }
  it = opts.find("params");
  if (it != opts.end()) {
    auto R3 = vm::parse_stack_entries(it->second);
    if (R3.is_error()) {
      error_ = R3.move_as_error();
      return;
    }
    params_ = R3.move_as_ok();
  }
}

void HttpQueryRunMethod::start_up_query() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &HttpQueryRunMethod::abort_query, R.move_as_error_prefix("litequery failed: "));
    } else {
      td::actor::send_closure(SelfId, &HttpQueryRunMethod::got_account, R.move_as_ok());
    }
  });
  auto a = ton::create_tl_object<ton::lite_api::liteServer_accountId>(addr_.workchain, addr_.addr);
  auto query = ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_getAccountState>(
                                            ton::create_tl_lite_block_id(block_id_), std::move(a)),
                                        true);
  td::actor::send_closure(CoreActorInterface::instance_actor_id(), &CoreActorInterface::send_lite_query,
                          std::move(query), std::move(P));
}

void HttpQueryRunMethod::got_account(td::BufferSlice data) {
  auto F = ton::fetch_tl_object<ton::lite_api::liteServer_accountState>(std::move(data), true);
  if (F.is_error()) {
    abort_query(F.move_as_error());
    return;
  }

  auto f = F.move_as_ok();
  data_ = std::move(f->state_);
  proof_ = std::move(f->proof_);
  shard_proof_ = std::move(f->shard_proof_);
  block_id_ = ton::create_block_id(f->id_);
  res_block_id_ = ton::create_block_id(f->shardblk_);

  finish_query();
}

void HttpQueryRunMethod::finish_query() {
  if (promise_) {
    auto page = [&]() -> std::string {
      HttpAnswer A{"account", prefix_, code_};
      A.set_account_id(addr_);
      A.set_block_id(res_block_id_);

      block::AccountState account_state;
      account_state.blk = block_id_;
      account_state.shard_blk = res_block_id_;
      account_state.shard_proof = std::move(shard_proof_);
      account_state.proof = std::move(proof_);
      account_state.state = std::move(data_);
      auto r_info = account_state.validate(block_id_, addr_);
      if (r_info.is_error()) {
        A.abort(r_info.move_as_error());
        return A.finish();
      }
      auto info = r_info.move_as_ok();
      if (info.root.is_null()) {
        A.abort(PSTRING() << "account state of " << addr_ << " is empty (cannot run method `" << method_name_ << "`)");
        return A.finish();
      }
      block::gen::Account::Record_account acc;
      block::gen::AccountStorage::Record store;
      block::CurrencyCollection balance;
      if (!(tlb::unpack_cell(info.root, acc) && tlb::csr_unpack(acc.storage, store) &&
            balance.validate_unpack(store.balance))) {
        A.abort("error unpacking account state");
        return A.finish();
      }
      int tag = block::gen::t_AccountState.get_tag(*store.state);
      switch (tag) {
        case block::gen::AccountState::account_uninit:
          A.abort(PSTRING() << "account " << addr_ << " not initialized yet (cannot run any methods)");
          return A.finish();
        case block::gen::AccountState::account_frozen:
          A.abort(PSTRING() << "account " << addr_ << " frozen (cannot run any methods)");
          return A.finish();
      }

      CHECK(store.state.write().fetch_ulong(1) == 1);  // account_init$1 _:StateInit = AccountState;
      block::gen::StateInit::Record state_init;
      CHECK(tlb::csr_unpack(store.state, state_init));
      auto code = state_init.code->prefetch_ref();
      auto data = state_init.data->prefetch_ref();
      auto stack = td::make_ref<vm::Stack>(std::move(params_));
      td::int64 method_id = (td::crc16(td::Slice{method_name_}) & 0xffff) | 0x10000;
      stack.write().push_smallint(method_id);
      long long gas_limit = vm::GasLimits::infty;
      vm::GasLimits gas{gas_limit};
      LOG(DEBUG) << "creating VM";
      vm::VmState vm{code, std::move(stack), gas, 1, data, vm::VmLog()};
      vm.set_c7(prepare_vm_c7(info.gen_utime, info.gen_lt, acc.addr, balance));  // tuple with SmartContractInfo
      // vm.incr_stack_trace(1);    // enable stack dump after each step
      int exit_code = ~vm.run();
      if (exit_code != 0) {
        A.abort(PSTRING() << "VM terminated with error code " << exit_code);
        return A.finish();
      }
      stack = vm.get_stack_ref();
      std::ostringstream os;
      os << "result: ";
      stack->dump(os, 3);
      A.serializeObject(HttpAnswer::Stack{stack});

      return A.finish();
    }();
    auto R = MHD_create_response_from_buffer(page.length(), const_cast<char *>(page.c_str()), MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(R, "Content-Type", "application/json");
    promise_.set_value(std::move(R));
  }
  stop();
}


