#include "../blockchain-api-query.hpp"


HttpQueryConfig::HttpQueryConfig(std::string prefix, ton::BlockIdExt block_id, std::vector<td::int32> params,
                                 td::Promise<MHD_Response *> promise)
    : HttpQueryCommon(prefix, std::move(promise), std::move(block_id)), params_(std::move(params)) {
}

HttpQueryConfig::HttpQueryConfig(std::map<std::string, std::string> opts, std::string prefix,
                                 td::Promise<MHD_Response *> promise)
    : HttpQueryCommon(prefix, std::move(promise)) {
  auto R = parse_block_id(opts, true);
  if (R.is_error()) {
    error_ = R.move_as_error();
    return;
  }
  block_id_ = R.move_as_ok();

  auto it = opts.find("param");
  if (it != opts.end()) {
    auto R2 = td::to_integer_safe<int>(it->second);
    if (R2.is_error()) {
      error_ = R2.move_as_error();
      return;
    }
    params_.push_back(R2.move_as_ok());
  }
}

void HttpQueryConfig::start_up() {
  if (error_.is_error()) {
    abort_query(std::move(error_));
    return;
  }
  if (block_id_.is_valid()) {
    send_main_query();
  } else {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &HttpQueryConfig::abort_query, R.move_as_error());
      } else {
        td::actor::send_closure(SelfId, &HttpQueryConfig::got_block, R.move_as_ok());
      }
    });

    auto query = ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_getMasterchainInfo>(), true);
    td::actor::send_closure(CoreActorInterface::instance_actor_id(), &CoreActorInterface::send_lite_query,
                            std::move(query), std::move(P));
  }
}

void HttpQueryConfig::got_block(td::BufferSlice data) {
  auto F = ton::fetch_tl_object<ton::lite_api::liteServer_masterchainInfo>(std::move(data), true);
  if (F.is_error()) {
    abort_query(F.move_as_error());
    return;
  }
  auto f = F.move_as_ok();
  block_id_ = ton::create_block_id(f->last_);

  send_main_query();
}

void HttpQueryConfig::send_main_query() {
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &HttpQueryConfig::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &HttpQueryConfig::got_result, R.move_as_ok());
    }
  });
  auto query =
      params_.size() > 0
          ? ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_getConfigParams>(
                                         0, ton::create_tl_lite_block_id(block_id_), std::vector<int>(params_)),
                                     true)
          : ton::serialize_tl_object(ton::create_tl_object<ton::lite_api::liteServer_getConfigAll>(
                                         0, ton::create_tl_lite_block_id(block_id_)),
                                     true);
  td::actor::send_closure(CoreActorInterface::instance_actor_id(), &CoreActorInterface::send_lite_query,
                          std::move(query), std::move(P));
}

void HttpQueryConfig::got_result(td::BufferSlice data) {
  auto F = ton::fetch_tl_object<ton::lite_api::liteServer_configInfo>(std::move(data), true);
  if (F.is_error()) {
    abort_query(F.move_as_error());
    return;
  }
  auto f = F.move_as_ok();

  state_proof_ = std::move(f->state_proof_);
  config_proof_ = std::move(f->config_proof_);

  finish_query();
}

void HttpQueryConfig::finish_query() {
  if (promise_) {
    auto page = [&]() -> std::string {
      HttpAnswer A{"config", prefix_, code_};
      A.set_block_id(block_id_);
      auto R = block::check_extract_state_proof(block_id_, state_proof_.as_slice(), config_proof_.as_slice());
      if (R.is_error()) {
        A.abort(PSTRING() << "masterchain state proof for " << block_id_.to_str()
                          << " is invalid : " << R.move_as_error());
        return A.finish();
      }
      try {
        auto res = block::Config::extract_from_state(R.move_as_ok(), 0);
        if (res.is_error()) {
          A.abort(PSTRING() << "cannot unpack configuration: " << res.move_as_error());
          return A.finish();
        }
        auto config = res.move_as_ok();
        if (params_.size() > 0) {
          for (int i : params_) {
            auto value = config->get_config_param(i);
            if (value.not_null()) {
              A.serializeObject(HttpAnswer::ConfigParam{i, value});
            } else {
              A.abort(td::Status::Error(404, PSTRING() << "empty param " << i));
              return A.finish();
            }
          }
        } else {
          config->foreach_config_param([&](int i, td::Ref<vm::Cell> value) {
            if (value.not_null()) {
              A.serializeObject(HttpAnswer::ConfigParam{i, value});
            }
            return true;
          });
          config.reset();
        }
      } catch (vm::VmError &err) {
        A.abort(PSTRING() << "error while traversing configuration: " << err.get_msg());
      } catch (vm::VmVirtError &err) {
        A.abort(PSTRING() << "virtualization error while traversing configuration: " << err.get_msg());
      }
      return A.finish();
    }();
    auto R = MHD_create_response_from_buffer(page.length(), const_cast<char *>(page.c_str()), MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(R, "Content-Type", "application/json");
    promise_.set_value(std::move(R));
  }
  stop();
}

