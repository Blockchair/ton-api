#include "blockchain-api-http.hpp"
#include "block/block-db.h"
#include "block/block.h"
#include "block/block-parse.h"
#include "vm/boc.h"
#include "vm/cellops.h"
#include "vm/cells/MerkleProof.h"
#include "block/mc-config.h"
#include "ton/ton-shard.h"

#include "univalue.h"
#include "blockchain-api-query.hpp"

bool local_scripts{false};

std::map<HttpAnswer::TokenType, std::string> HttpAnswer::tokenTypeString = {
   {TokenType::Jetton, "jetton"}, {TokenType::NFT, "nft"}, {TokenType::Comment, "comment"}
};

UniValue HttpAnswer::serializeObject(AddressCell addr_c) {
  ton::WorkchainId wc;
  ton::StdSmcAddress addr;
  UniValue uvObj;
  uvObj.setObject();
  if (!block::tlb::t_MsgAddressInt.extract_std_address(addr_c.root, wc, addr)) {
    abort("<cannot unpack addr>");
    return uvObj;
  }
  block::StdAddress caddr{wc, addr};
  uvObj.pushKV("address_cell_info", serializeObject(AccountLink{caddr, ton::BlockIdExt{}}));
  return uvObj;
}

unpackRC HttpAnswer::serializeObject(AddressCell addr_c, std::string &address) {
  ton::WorkchainId wc;
  ton::StdSmcAddress addr;
  UniValue uvObj;
  uvObj.setObject();
  if (!block::tlb::t_MsgAddressInt.extract_std_address(addr_c.root, wc, addr)) {
    abort("<cannot unpack addr>");
    return uERR;
  }
  block::StdAddress caddr{wc, addr};
  address = caddr.rserialize(true);
  return uOK;
}

#define messageInType 1
#define messageOutType 2

#define PDO(__op) \
  if (!(__op)) {  \
    ok = false;   \
  }


unpackRC HttpAnswer::buildContractDataStruct(vm::CellSlice in_msg_state, StateInit &dataStateInit) {
  block::gen::StateInit::Record state;
  signed char split_depth{-1};
  bool tick;
  bool tock;
  td::Ref<vm::Cell> code, data, library;
  int special = 0;
  if (!tlb::unpack(in_msg_state, state)) {
    LOG(ERROR) << "cannot unpack StateInit from an inbound message";
    return uERR;
  }
  if (state.split_depth->size() == 6) {
    split_depth = (signed char)(state.split_depth->prefetch_ulong(6) - 32);
    dataStateInit.split_depth = split_depth;
  } else {
    split_depth = 0;
    dataStateInit.split_depth = 0;
  }
  if (state.special->size() > 1) {
    int z = (int)state.special->prefetch_ulong(3);
    if (z < 0) {
      return uERR;
    }
    tick = z & 2;
    tock = z & 1;
    LOG(DEBUG) << "tick=" << tick << ", tock=" << tock;
  }
  code = state.code->prefetch_ref();
  data = state.data->prefetch_ref();
  library = state.library->prefetch_ref();

  dataStateInit.code = state.code->prefetch_ref();
  dataStateInit.data = state.data->prefetch_ref();
  dataStateInit.library = state.library->prefetch_ref();

  vm::CellBuilder cb;
  if (!split_depth) {
    if (!cb.store_long_bool(0, 1)) {
      LOG(INFO) << "invalid split_depth for a smart contract";
      return uERR;
    }
  } else {
    if (!(cb.store_long_bool(1, 1) && cb.store_ulong_rchk_bool(split_depth, 5))) {
      LOG(INFO) << "invalid split_depth for a smart contract";
      return uERR;
    }
  }
  if (!special) {
    if (!(cb.store_long_bool(0, 1))) {
      LOG(INFO) << "invalid special TickTock argument for a smart contract";
      return uERR;
    }
  } else {
    if (!(cb.store_long_bool(1, 1) && cb.store_ulong_rchk_bool(special, 2))) {
      LOG(INFO) << "invalid special TickTock argument for a smart contract";
      return uERR;
    }
  }
  if (!(cb.store_maybe_ref(std::move(code)) && cb.store_maybe_ref(std::move(data)) && cb.store_maybe_ref(library))) {
    LOG(INFO) << "cannot store smart-contract code, data or library";
    return uERR;
  }
  td::Ref<vm::DataCell> state_init = cb.finalize();
  td::RefInt256 smc_addr;
  td::BitArray<256U> addr;
  if (smc_addr.is_null()) {
    addr = state_init->get_hash().as_array();
    smc_addr = td::RefInt256{true};
    if (!(smc_addr.write().import_bits(addr.data(), 0, 256, false))) {
      LOG(INFO) << "invalid special TickTock argument for a smart contract";
      return uERR;
    }
  } else {
    if (!(smc_addr->export_bits(addr.data(), 0, 256, false))) {
      LOG(INFO) << "invalid special TickTock argument for a smart contract";
      return uERR;
    }
  }
  std::string addr3 = "0:";
  block::StdAddress a;
  addr3.append(smc_addr->to_hex_string());
  if (a.parse_addr(td::Slice(addr3))) {
    dataStateInit.address = a.rserialize(true);
  }
  return uOK;
}

// unpacking without full fields (only counting fees and src, dst)
unpackRC HttpAnswer::unpackMessage(MessageCell msg, UniValue &uvObj, int messageType, bool small) {
  std::string addrSmcAddr = "";
  TransferData transferData;
  if (msg.root.is_null()) {
    abort("<message not found");
    return uERR;
  }

  auto cs = vm::load_cell_slice(msg.root);
  block::gen::CommonMsgInfo info;
  td::Ref<vm::CellSlice> src, dest;

  block::gen::Message::Record message;
  if (!tlb::type_unpack_cell(msg.root, block::gen::t_Message_Any, message)) {
    return uERR;
  }
  td::Ref<vm::CellSlice> body;

  if (message.body->prefetch_long(1) == 0) {
    body = std::move(message.body);
    body.write().advance(1);
  } else {
    body = vm::load_cell_slice_ref(message.body->prefetch_ref());
  }

  if (!body.is_null()) {
    auto nm = body->prefetch_int256(32, false);
    if (!nm.is_null()) {
      if (*nm == td::BigInt256((int)0x178d4519) || *nm == td::BigInt256((int)0x7362d09c)) {
        transferData.init();
        if (*nm == td::BigInt256((int)0x7362d09c))
          transferData.transferType = transferTypeString.find(TransferType::TransferNotify)->second;
        else
          transferData.transferType = transferTypeString.find(TransferType::InterWallet)->second;
        transferData.tokenType = TokenType::Jetton;
      }
      if (*nm == td::BigInt256((int)0x05138d91)) {
        transferData.init();
        transferData.tokenType = TokenType::NFT;
        transferData.transferType = transferTypeString.find(TransferType::OwnershipAssigned)->second;
      }
      if (*nm == td::BigInt256((int)0x00)) {
        transferData.init();
        transferData.tokenType = TokenType::Comment;
      }
    }
  }

  std::string source = "";
  std::string destination = "";
  uvObj.pushKV("message_hash", msg.root->get_hash().to_hex());
  switch (block::gen::t_CommonMsgInfo.get_tag(cs)) {
    case block::gen::CommonMsgInfo::ext_in_msg_info: {
      block::gen::CommonMsgInfo::Record_ext_in_msg_info info;
      if (!tlb::unpack(cs, info)) {
        abort("<cannot unpack inbound external message>");
        return uNULL_ptr;
      }
      int64_t gramsCurColl = 0;
      td::BigInt256 importFee;
      if (!small) {
        if (serializeObject(AddressCell{info.dest}, destination) == uERR) {
          return uERR;
        }
        uvObj.pushKV("destination", destination);
      }
      switch (unpackBalance(info.import_fee, gramsCurColl, importFee)) {
        case uOK:
          if (!small) {
            uvObj.pushKV("import_fee", gramsCurColl);
          }
          this->fees += importFee;
          break;
        case uNULL_ptr:
        case uERR:
        default:
          if (!small) {
            uvObj.pushKV("import_fee", UniValue::VNULL);
          }
          break;
      }
      return uOK;
    }

    case block::gen::CommonMsgInfo::ext_out_msg_info: {
      block::gen::CommonMsgInfo::Record_ext_out_msg_info info;
      if (!tlb::unpack(cs, info)) {
        abort("<cannot unpack outbound external message>");
        return uNULL_ptr;
      }
      if (!small) {
        uvObj.pushKV("lt", (uint64_t)info.created_lt);
        uvObj.pushKV("time", (uint64_t)info.created_at);
        uvObj.pushKV("source", serializeObject(AddressCell{info.src}));
        uvObj.pushKV("destination", UniValue::VNULL);
      }
      return uOK;
    }

    case block::gen::CommonMsgInfo::int_msg_info: {
      block::gen::CommonMsgInfo::Record_int_msg_info info;
      if (!tlb::unpack(cs, info)) {
        abort("cannot unpack internal message");
        return uERR;
      }
      td::RefInt256 value;
      td::Ref<vm::Cell> extra;
      if (!block::unpack_CurrencyCollection(info.value, value, extra)) {
        abort("cannot unpack message value");
        return uERR;
      }
      std::string source;
      std::string destination;
      std::string money;
      if (serializeObject(AddressCell{info.src}, source) == uERR) {
        return uERR;
      }
      if (serializeObject(AddressCell{info.dest}, destination) == uERR) {
        return uERR;
      }
      int64_t ihr_feeCurColl;
      int64_t fwd_feeCurColl;
      td::BigInt256 ihr_fee;
      td::BigInt256 fwd_fee;
      switch (unpackBalance(info.ihr_fee, ihr_feeCurColl, ihr_fee)) {
        case uOK:
          if (!small)
            uvObj.pushKV("ihr_fee", ihr_feeCurColl);
          this->fees += (messageType != messageInType) ? ihr_fee : td::BigInt256(0);
          break;
        case uNULL_ptr:
        case uERR:
        default:
          if (!small)
            uvObj.pushKV("ihr_fee", UniValue::VNULL);
          break;
      }
      switch (unpackBalance(info.fwd_fee, fwd_feeCurColl, fwd_fee)) {
        case uOK:
          if (!small)
            uvObj.pushKV("fwd_fee", fwd_feeCurColl);
          this->fees += (messageType != messageInType) ? fwd_fee : td::BigInt256(0);
          break;
        case uNULL_ptr:
        case uERR:
        default:
          if (!small)
            uvObj.pushKV("fwd_fee", UniValue::VNULL);
          break;
      }
      uvObj.pushKV("lt", (uint64_t)info.created_lt);
      uvObj.pushKV("time", (uint64_t)info.created_at);
      uvObj.pushKV("value", value->to_dec_string());
      uvObj.pushKV("source", source);
      uvObj.pushKV("destination", destination);

      UniValue uvFake;
      uvFake.setObject();
      unpackStateInit(cs, destination, info.dest, uvFake, transferData);
      if (transferData.tokenType == TokenType::Jetton) {
        unpackMessageBodyJetton(*(body.get()), transferData);
      }
      if (transferData.tokenType == TokenType::NFT) {
        unpackMessageBodyNFT(*(body.get()), transferData);
        transferData.token = source;
        transferData.to = destination;
      }
      if (transferData.tokenType == TokenType::Comment) {
        unpackMessageBodyComment(*(body.get()), transferData);
      }
      uvObj.pushKV("transfer", transferData.serialize());

      if (messageType == messageInType) {
        this->sender = source;
        this->recipient = destination;
        this->gram_amount_in = value.write();
      }
      if (messageType == messageOutType) {
        this->gram_amount_out = value.write();
      }
      break;
    }
    default:
      abort("cannot unpack message");
      return uERR;
  }
  return uOK;
}

unpackRC HttpAnswer::unpackStateInit(vm::CellSlice cs, std::string destination, td::Ref<vm::CellSlice> destination_cs,
                                     UniValue &uvObj, TransferData &transferData) {
  td::Ref<vm::Cell> in_msg_state;
  td::Ref<vm::CellSlice> in_msg_body;
  StateInit dataStateInit;

  switch ((int)cs.prefetch_ulong(2)) {
    case 2: {  // (just$1 (left$0 _:StateInit ))
      td::Ref<vm::CellSlice> state_init;
      vm::CellBuilder cb;
      if (!(cs.advance(2) && block::gen::t_StateInit.fetch_to(cs, state_init) &&
            cb.append_cellslice_bool(std::move(state_init)) && cb.finalize_to(in_msg_state) &&
            block::gen::t_StateInit.validate_ref(in_msg_state))) {
        LOG(INFO) << "cannot parse StateInit in inbound message";
      } else {
        buildContractDataStruct(vm::load_cell_slice(in_msg_state), dataStateInit);
      }
      break;
    }
    case 3: {  // (just$1 (right$1 _:^StateInit ))
      if (!(cs.advance(2) && cs.fetch_ref_to(in_msg_state) && block::gen::t_StateInit.validate_ref(in_msg_state))) {
        LOG(INFO) << "cannot parse ^StateInit in inbound message";
      } else {
        buildContractDataStruct(vm::load_cell_slice(in_msg_state), dataStateInit);
      }
      break;
    }
    default:  // nothing$0
      if (!cs.advance(1)) {
        LOG(INFO) << "invalid init field in an inbound message";
      }
      break;
  }
  if (dataStateInit.address == destination && dataStateInit.address.size() > 0) {
    uvObj.pushKV("deployed", true);  // TODO - new name for the field
    serializeStateInit(dataStateInit, uvObj);
  } else {
    uvObj.pushKV("deployed", false);
  }

  getContractData(dataStateInit, destination_cs, uvObj, transferData);

  return uOK;
}

void HttpAnswer::serializeStateInit(StateInit dataStateInit, UniValue &uvObj) {
  UniValue uvState;
  uvState.setObject();
  if (dataStateInit.code.not_null()) {
    td::BufferSlice code = std_boc_serialize(dataStateInit.code, 2).move_as_ok();
    std::string code_hex = printInString(code.data(), code.length());
    UniValue uvCode;
    uvCode.setObject();
    if (dataStateInit.walletType.size() == 0)
      uvState.pushKV("wallet_type", walletType(code_hex));
    else
      uvState.pushKV("wallet_type", dataStateInit.walletType);
    uvCode.pushKV("code_base64", td::base64_encode(code));
    uvCode.pushKV("code_hex", code_hex);
    uvState.pushKV("code", uvCode);
  } else {
    uvState.pushKV("code", UniValue::VNULL);
  }
  if (dataStateInit.data.not_null()) {
    UniValue uvData;
    uvData.setObject();
    td::BufferSlice data = std_boc_serialize(dataStateInit.data).move_as_ok();
    uvData.pushKV("data_base64", td::base64_encode(data));
    uvData.pushKV("data_hex", printInString(data.data(), data.length()));
    uvState.pushKV("data", uvData);
  } else {
    uvState.pushKV("data", UniValue::VNULL);
  }
  if (dataStateInit.library.not_null()) {
    UniValue uvLib;
    uvLib.setObject();
    td::BufferSlice library = std_boc_serialize(dataStateInit.library).move_as_ok();
    uvLib.pushKV("library_base64", td::base64_encode(library));
    uvLib.pushKV("library_hex", printInString(library.data(), library.length()));
    uvState.pushKV("library", uvLib);
  } else {
    uvState.pushKV("library", UniValue::VNULL);
  }
  uvObj.pushKV("state_init", uvState);
}

unpackRC HttpAnswer::getContractData(StateInit &dataStateInit, td::Ref<vm::CellSlice> address, UniValue &uvObj,
                                     TransferData &transferData) {
  td::Ref<vm::Stack> stack;
  UniValue uvStack;
  uvStack.setObject();
  for (auto mt : methods) {
    if (!processContractMethod(dataStateInit, address, mt.first, stack)) {
      unpackContractData(stack.write(), uvStack, transferData, mt.second);
      dataStateInit.walletType = mt.second.first;
      break;
    }
  }
  uvObj.pushKV("contract_data", uvStack);
  return uOK;
}

unpackRC HttpAnswer::unpackContractData(vm::Stack stack, UniValue &uvObj, TransferData &transferData, std::pair<std::string, std::vector<std::string>> args) {
  std::size_t stackSize = stack.depth();
  for (std::size_t i = 0, y = args.second.size() - 1; i < stackSize; i++, y--) {
    vm::StackEntry se = stack.pop();
    if (se.type() == vm::StackEntry::t_cell) {
      td::Ref<vm::Cell> jetton_wallet_code = se.as_cell();
      if (args.second[y] == "jetton_content" || 
          args.second[y] == "individual_content" || 
          args.second[y] == "collection_content") {
        UniValue uvMeta;
        uvMeta.setObject();
        auto content = vm::load_cell_slice(jetton_wallet_code);
        unpackContentData(content, uvMeta);
        uvObj.pushKV(args.second[y], uvMeta);
      } else {
        td::BufferSlice code = std_boc_serialize(jetton_wallet_code).move_as_ok();
        uvObj.pushKV(args.second[y], printInString(code.data(), code.length()));
      }
      continue;
    }
    if (se.type() == vm::StackEntry::t_slice) {
      td::Ref<vm::CellSlice> slice = se.as_slice();
      std::string sliceStr = unpackMsgAddress(slice.write());
      uvObj.pushKV(args.second[y], sliceStr);
      if (args.second[y] == "owner") {
        transferData.to = sliceStr;
      }
      if(args.second[y] == "jetton") {
        transferData.token = sliceStr;
      }
      continue;
    }
    if (se.type() == vm::StackEntry::t_int) {
      td::RefInt256 refInt256 = se.as_int();
      uvObj.pushKV(args.second[y], refInt256.write().to_dec_string());
      continue;
    }
  }
  return uOK;
}

unpackRC HttpAnswer::unpackContentData(vm::CellSlice content, UniValue &uvObj) {
  UniValue uvData;
  uvData.setObject();
  Metadata metadata{};
  int fb = content.prefetch_octet();
  if(fb == On_Chain_Content) {
    content.fetch_bits(8);
    block::gen::HashmapE::Record_hme_root record;
    td::Ref<vm::Cell> rc;
    if (content.have_refs() > 0) {
      vm::Dictionary dict{std::move(content), 256};
      dict.check_for_each([&](td::Ref<vm::CellSlice> cs, td::ConstBitPtr key, int n) -> bool {
        std::string outData;
        // LOG(INFO) << td::hex_decode(key.to_hex(256)).move_as_ok();
        unpackLongHashMap(cs.write(), outData);
        metadata.data.push_back(outData);
        return true;
      });
    }
    uvObj.pushKV("metadata", metadata.serialize());
    return uOK;
  }
  if(fb == Off_Chain_Content) {
    content.fetch_bits(8);
    uvData.pushKV("url", td::hex_decode(content.as_bitslice().to_hex()).move_as_ok());
  }
  uvObj.pushKV("metadata", uvData);
  return uOK;
}

unpackRC HttpAnswer::unpackLongHashMap(vm::CellSlice cs, std::string &outInfo, bool firstTime) {
  if (cs.have_refs()) {
    td::Ref<vm::Cell> c2 = cs.prefetch_ref();
    vm::CellSlice cs2 = vm::load_cell_slice(c2);
    if (!firstTime) {
      outInfo += td::hex_decode(cs2.as_bitslice().to_hex()).move_as_ok();
      if (cs2.have_refs()) {
        return unpackLongHashMap(cs2, outInfo, false);
      }
      return uOK;
    } else {
      if (cs2.prefetch_octet() == Snake_Format) {
        cs2.fetch_bits(8);
        outInfo += td::hex_decode(cs2.as_bitslice().to_hex()).move_as_ok();
        if (cs2.have_refs()) {
          return unpackLongHashMap(cs2, outInfo, false);
        }
        return uOK;
      }
      if (cs2.prefetch_octet() == Chunked_Format) {
        return uOK;
      }
    }
  }
  return uERR;
}

unpackRC HttpAnswer::processContractMethod(StateInit dataStateInit, td::Ref<vm::CellSlice> address, std::string method,
                                           td::Ref<vm::Stack> &stack) {
  block::CurrencyCollection balance;
  std::vector<vm::StackEntry> params_;

  auto code = dataStateInit.code;
  auto data = dataStateInit.data;
  stack = td::make_ref<vm::Stack>(std::move(params_));
  td::int64 method_id = (td::crc16(td::Slice{method}) & 0xffff) | 0x10000;
  stack.write().push_smallint(method_id);
  long long gas_limit = vm::GasLimits::infty;
  vm::GasLimits gas{gas_limit};
  LOG(DEBUG) << "creating VM";
  vm::VmState vm{code, std::move(stack), gas, 1, data, vm::VmLog()};
  vm.set_c7(prepare_vm_c7(this->now, this->lt, address, balance));
  int exit_code = ~vm.run();
  if (exit_code != 0) {
    return uERR;
  }
  stack = vm.get_stack_ref();
  return uOK;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

unpackRC HttpAnswer::unpackMessageBody(vm::CellSlice in_msg_raw, const int out_msg_count, vm::CellSlice out_msg_map,
                                       UniValue &uvObj) {
  block::gen::Maybe t_Ref_Message_Maybe{block::gen::t_Ref_Message_Any};
  block::gen::Maybe::Record_just ref_msg;

  if (t_Ref_Message_Maybe.unpack(in_msg_raw, ref_msg)) {
    block::gen::RefT ref{block::gen::t_Message_Any};
    td::Ref<vm::CellSlice> ref_msg_body_raw;
    if (ref.fetch_to(ref_msg.value.write(), ref_msg_body_raw)) {
      vm::CellSlice in_body, out_body;
      if (tryExtractMsgBody(ref_msg_body_raw->prefetch_ref(), in_body) != uOK) {
        return uERR;
      }
      if (this->recipient == ELECTOR_CONST && out_msg_count == 1) {  //
        if (out_msg_count != 1) {
          return uERR;
        }
        vm::Dictionary omsg_dict{out_msg_map, 15};
        td::Ref<vm::Cell> omsg = omsg_dict.lookup_ref(td::BitArray<15>{(int)0});
        if (tryExtractMsgBody(omsg, out_body) != uOK) {
          return uERR;
        }
        if (unpackMsgBody_Elector(in_body, out_body, uvObj) == uERR) {
          return uERR;
        }
      }
    } else {
      return uERR;
    }
    return uOK;
  } else {
    return uERR;
  }
  return uERR;
}

unpackRC HttpAnswer::unpackMsgBody_Elector(vm::CellSlice &in_msg, vm::CellSlice &out_msg, UniValue &uvObj) {
  if (!in_msg.have(32 + 64) || !out_msg.have(32 + 64)) {
    return uERR;
  }
  td::RefInt256 op = in_msg.fetch_int256(32, false);
  td::RefInt256 query_id_in = in_msg.fetch_int256(64, false);

  td::RefInt256 ans_tag = out_msg.fetch_int256(32, false);
  td::RefInt256 query_id_out = out_msg.fetch_int256(64, false);

  if (query_id_in->to_dec_string() !=
      query_id_out
          ->to_dec_string()) { 
    return uERR;                
  }

  if (*op == td::BigInt256((int)0x4e73744b)) {
    return elector_processNewStake(in_msg, *ans_tag, uvObj);
  }
  if (*op == td::BigInt256((int)0x47657424)) {
    return elector_recoverStake(*ans_tag, uvObj);
  }
  return uOK;
}

unpackRC HttpAnswer::tryExtractMsgBody(td::Ref<vm::Cell> msg_ref, vm::CellSlice &ret) {
  block::gen::Message::Record tmp;
  block::gen::Message t_Message{block::gen::t_Message_Any};

  if (!t_Message.cell_unpack(msg_ref, tmp)) {
    return uERR;
  }
  if (tmp.body.is_null()) {  // maybe redundant
    return uERR;
  }

  vm::CellSlice _body = *(tmp.body);
  if (!_body.have(1)) {
    return uERR;
  }

  if (_body.fetch_long(1) == 0) {  // either->LEFT ie msg as is
    ret = *(_body.fetch_subslice(_body.size(), 0));
    if (!ret.is_valid()) {
      return uERR;
    }
  } else {  // either->RIGHT ie msg as a reference
    td::Ref<vm::Cell> right = _body.fetch_ref();
    vm::CellSlice rbody{vm::NoVmOrd(), right};
    ret = std::move(rbody);
    if (!ret.is_valid()) {
      return uERR;
    }
  }
  return uOK;
}

// https://github.com/ton-blockchain/ton/blob/master/crypto/smartcont/elector-code.fc#L198
unpackRC HttpAnswer::elector_processNewStake(vm::CellSlice &in_msg, td::BigInt256 ans_tag, UniValue &uvObj) {
  if (ans_tag == td::BigInt256(0xee6f454c)) {         // incorrect payload - stake returned
    return uOK;                                       // not interested in those
  } else if (ans_tag != td::BigInt256(0xf374484c)) {  // something funny going on - manual intervention please
    return uERR;                                      // might need some VERY special error cuz this is dangerous
  }

  // if contract was happy with payload - so should we
  td::BitSlice validator_pubkey = in_msg.fetch_bits(256);
  in_msg.fetch_bits(32 + 32);  // dont need next 2 fields
  td::BitSlice validator_adnl_addr = in_msg.fetch_bits(256);

  UniValue uv_new_stake;
  uv_new_stake.setObject();

  uv_new_stake.pushKV("pubkey", validator_pubkey.to_hex());
  uv_new_stake.pushKV("adnl_addr", validator_adnl_addr.to_hex());
  uv_new_stake.pushKV("wallet", this->sender);
  this->gram_amount_in -= td::BigInt256(1000000000);
  uv_new_stake.pushKV("stake", this->gram_amount_in.to_dec_string());

  uvObj.pushKV("staker_info", uv_new_stake);
  return uOK;
}

// https://github.com/ton-blockchain/ton/blob/master/crypto/smartcont/elector-code.fc#L403
unpackRC HttpAnswer::elector_recoverStake(td::BigInt256 ans_tag, UniValue &uvObj) {
  if (ans_tag == td::BigInt256(0xfffffffe)) {         // incorrect payload -- reverting
    return uOK;                                       // not interested in those
  } else if (ans_tag != td::BigInt256(0xf96f7324)) {  // something funny going on - manual intervention please
    return uERR;
  }

  // guy successfully took everything (in out_msg.grams)
  UniValue uv_stake_recovered;
  uv_stake_recovered.setObject();
  uv_stake_recovered.pushKV("wallet", this->sender);
  uv_stake_recovered.pushKV("stake", this->gram_amount_out.to_dec_string());

  uvObj.pushKV("stake_retrieved", uv_stake_recovered);

  return uOK;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void printInFile(const unsigned char *data, int size) {
  for (int i = 0; i < size; i++) {
    char xbin[3] = {0};
    sprintf(xbin, "%02x", (unsigned char)data[i]);
    std::cout << xbin;
  }
  std::cout << '\n';
}

std::string HttpAnswer::printInString(char *data, size_t size) {
  std::ostringstream str;
  for (size_t i = 0; i < size; i++) {
    char xbin[3] = {0};
    sprintf(xbin, "%02x", (unsigned char)data[i]);
    str << xbin;
  }
  return str.str();
}

unpackRC HttpAnswer::unpackMsgAddressExt(vm::CellSlice &body, std::string &addr) {
  block::gen::MsgAddressExt::Record_addr_extern data;
  switch (block::gen::t_MsgAddressExt.get_tag(body)) {
    case block::gen::MsgAddressExt::addr_none:
          break;
    case block::gen::MsgAddressExt::addr_extern:
      if (block::gen::t_MsgAddressExt.unpack(body, data)) {
        addr = block::StdAddress(0, data.external_address->bits()).rserialize(true);
      }
      break;
    default:
      break;
  }
  return uOK;
}

unpackRC HttpAnswer::unpackMsgAddressExt(vm::CellSlice &body, UniValue &uvObj) {
  std::string addr = "";
  unpackMsgAddressExt(body, addr);
  LOG(INFO) << "strange address" << addr;
  uvObj.pushKV("strange address", addr);
  return uOK;
}

unpackRC HttpAnswer::unpackMsgAddressInt(vm::CellSlice &body, std::string &addr) {
  block::gen::MsgAddressInt::Record_addr_std record_addr_std;
  block::gen::MsgAddressInt::Record_addr_var record_addr_var;
  switch (block::gen::t_MsgAddressInt.get_tag(body)) {
    case block::gen::MsgAddressInt::addr_std:
      if (block::gen::t_MsgAddressInt.unpack(body, record_addr_std)) {
        addr = block::StdAddress(record_addr_std.workchain_id, record_addr_std.address, true, false).rserialize(true);
      }
      break;
    case block::gen::MsgAddressInt::addr_var:
      if (block::gen::t_MsgAddressInt.unpack(body, record_addr_var)) {
        addr = block::StdAddress(record_addr_var.workchain_id, record_addr_var.address.write().cbits(), true, false)
                   .rserialize(true);
      }
      break;
    default:
      break;
  }

  return uOK;
}

unpackRC HttpAnswer::unpackMsgAddressInt(vm::CellSlice &body, UniValue &uvObj) {
  std::string addr = "";
  unpackMsgAddressInt(body, addr);
  LOG(INFO) << "addr_addr" << addr;
  uvObj.pushKV("addr_addr", addr);
  return uOK;
}

std::string HttpAnswer::unpackMsgAddress(vm::CellSlice &body) {
  std::string address = "";
  block::gen::MsgAddress::Record_cons1 refMsgAddressCons1;
  block::gen::MsgAddress::Record_cons2 refMsgAddressCons2;
  switch (block::gen::t_MsgAddress.get_tag(body)) {
    case block::gen::MsgAddress::cons1:
      if (block::gen::t_MsgAddress.unpack(body, refMsgAddressCons1)) {
        unpackMsgAddressInt(refMsgAddressCons1.x.write(), address);
      }
      break;
    case block::gen::MsgAddress::cons2:
      if (tlb::unpack(body, refMsgAddressCons2)) {
        unpackMsgAddressExt(refMsgAddressCons2.x.write(), address);
      }
      break;
    default:
      break;
  }
  return address;
}

unpackRC HttpAnswer::unpackMessageBodyJetton(vm::CellSlice body, TransferData &transferData) {
  std::string from;
  std::string to;
  auto query = body.fetch_bits(32);
  transferData.query_id = body.fetch_int256(64, false);
  // body.fetch_bits(64);
  block::gen::VarUInteger::Record varUInteger;
  if (block::gen::t_VarUInteger_16.unpack(body, varUInteger)) {
    transferData.amount = varUInteger.value;
  };
  from = unpackMsgAddress(body);
  to = unpackMsgAddress(body);
  if (from == to) {
    transferData.from = from;
  } else {
    transferData.from = from;
    transferData.to = to;
  }
  return uOK;
}

unpackRC HttpAnswer::unpackMessageBodyNFT(vm::CellSlice body, TransferData &transferData) {
  std::string from;
  auto query = body.fetch_bits(32);
  transferData.query_id = body.fetch_int256(64, false);
  transferData.from = unpackMsgAddress(body);
  return uOK;
}

unpackRC HttpAnswer::unpackMessageBodyComment(vm::CellSlice body, TransferData &transferData) {
  auto query = body.fetch_bits(32);
  td::Result<td::string> comment = td::hex_decode(body.as_bitslice().to_hex());
  if(comment.is_ok()) {
    transferData.comment = comment.move_as_ok();
  }
  return uOK;
}

UniValue HttpAnswer::serializeCellSlice(vm::CellSlice cs) {
  UniValue uvObj;
  UniValue uvObjBits;
  UniValue uvObjRefs;
  uvObj.setObject();
  uvObj.pushKV("hash", (cs.get_base_cell())->get_hash().to_hex());
  uvObjBits.setObject();
  uvObjBits.pushKV("bits_st", (int64_t)cs.cur_pos());
  uvObjBits.pushKV("bits_en", (int64_t)cs.cur_pos() + (int64_t)cs.size());

  uvObjRefs.setObject();
  uvObjRefs.pushKV("bits_st", (int64_t)cs.cur_ref());
  uvObjRefs.pushKV("bits_en", (int64_t)cs.cur_ref() + (int64_t)cs.size_refs());

  uvObj.pushKV("bits", uvObjBits);
  uvObj.pushKV("refs", uvObjRefs);
  return uvObj;
}

UniValue HttpAnswer::serializeStackEntry(vm::StackEntry se) {
  UniValue uvObj;
  uvObj.setObject();
  switch (se.type()) {
    case vm::StackEntry::t_null:
      uvObj.pushKV("data", UniValue::VNULL);
      uvObj.pushKV("type", "null");
      break;
    case vm::StackEntry::t_int:
      try {
        // TODO check why I need '-1'
        std::string addr = "-1:";
        block::StdAddress a;
        addr.append((se.as_int())->to_hex_string());
        if (a.parse_addr(td::Slice(addr))) {
          uvObj.pushKV("data", a.rserialize(true));
          uvObj.pushKV("type", "int");
        } else {
          uvObj.pushKV("data", (se.as_int()).write().to_dec_string());
          uvObj.pushKV("type", "int");
        }
      } catch (...) {
        uvObj.pushKV("data", (se.as_int()).write().to_dec_string());
        uvObj.pushKV("type", "int");
      }
      break;
    case vm::StackEntry::t_cell:
      uvObj.pushKV("data", static_cast<vm::Ref<vm::Cell>>(se.as_cell())->get_hash().to_hex());
      uvObj.pushKV("type", "cell");
      break;
    case vm::StackEntry::t_builder:
      uvObj.pushKV("data", static_cast<vm::Ref<vm::CellBuilder>>(se.as_builder())->to_hex());
      uvObj.pushKV("type", "cell_builder");
      break;
    case vm::StackEntry::t_slice: {
      uvObj.pushKV("data", serializeCellSlice(static_cast<vm::Ref<vm::CellSlice>>(se.as_slice()).write()));
      uvObj.pushKV("type", "cell_slice");
      break;
    }
    case vm::StackEntry::t_string:
      uvObj.pushKV("data", se.as_string());
      uvObj.pushKV("type", "string");
      break;
    case vm::StackEntry::t_bytes:
      uvObj.pushKV("data", vm::str_to_hex(se.as_bytes()));
      uvObj.pushKV("type", "bytes");
      break;
    case vm::StackEntry::t_box: {
      uvObj.pushKV("data", "unsupported type");
      uvObj.pushKV("type", "box");
      break;
    }
    case vm::StackEntry::t_atom: {
      uvObj.pushKV("data", "unsupported type");
      uvObj.pushKV("type", "atom");
      break;
    }
    case vm::StackEntry::t_tuple: {
      UniValue uvArr;
      uvArr.setArray();
      const auto &tuple = *static_cast<vm::Ref<vm::Tuple>>(se.as_tuple());
      for (const auto &entry : tuple) {
        uvArr.push_back(serializeStackEntry(entry));
      }
      uvObj.pushKV("data", uvArr);
      uvObj.pushKV("type", "tuple");
      break;
    }
    case vm::StackEntry::t_object: {
      uvObj.pushKV("data", "unsupported type");
      uvObj.pushKV("type", "object");
      break;
    }
    default:
      uvObj.pushKV("data", UniValue::VNULL);
      uvObj.pushKV("type", "unknown");
  }
  return uvObj;
}

UniValue HttpAnswer::serializeObject(Stack stack) {
  auto stackNotRef = stack.stack.write();
  UniValue uvArr;
  uvArr.setArray();
  int stackSize = stackNotRef.depth();
  for (int i = 0; i < stackSize; i++) {
    vm::StackEntry se = stackNotRef.pop();
    uvArr.push_back(serializeStackEntry(se));
  }
  jsonObject.pushKV("answer", uvArr);
  return uvArr;
}

bool HttpAnswer::checkNeededType(vm::StackEntry se, std::string expectedType) {
  switch (se.type()) {
    case vm::StackEntry::t_null:
      if (expectedType == "t_null")
        return true;
      break;
    case vm::StackEntry::t_int:
      if (expectedType == "t_int")
        return true;
      break;
    case vm::StackEntry::t_cell:
      if (expectedType == "t_cell")
        return true;
      break;
    case vm::StackEntry::t_builder:
      if (expectedType == "t_builder")
        return true;
      break;
    case vm::StackEntry::t_slice: {
      if (expectedType == "t_slice")
        return true;
      break;
    }
    case vm::StackEntry::t_string:
      if (expectedType == "t_string")
        return true;
      break;
    case vm::StackEntry::t_bytes:
      if (expectedType == "t_bytes")
        return true;
      break;
    case vm::StackEntry::t_box: {
      if (expectedType == "t_box")
        return true;
      break;
    }
    case vm::StackEntry::t_atom: {
      if (expectedType == "t_atom")
        return true;
      break;
    }
    case vm::StackEntry::t_tuple: {
      if (expectedType == "t_tuple")
        return true;
      break;
    }
    case vm::StackEntry::t_object: {
      if (expectedType == "t_object")
        return true;
      break;
    }
    default:
      return false;
  }
  return false;
}

inline UniValue HttpAnswer::serializeBlockIdExt(ton::BlockIdExt block_id) {
  return serializeObject(BlockLink{block_id});
}

UniValue HttpAnswer::serializeBlockId(ton::BlockId block_id) {
  UniValue uvObj;
  uvObj.setObject();
  uvObj.pushKV("workchain", block_id.workchain);
  uvObj.pushKV("shard", ton::shard_to_str(block_id.shard));
  uvObj.pushKV("seqno", (uint64_t)block_id.seqno);
  return uvObj;
}

unpackRC HttpAnswer::unpackVarUInteger16(vm::CellSlice varUInteger16, td::RefInt256 &out) {
  block::gen::VarUInteger::Record varUInteger;
  if (block::gen::VarUInteger(16).unpack(varUInteger16, varUInteger)) {
    out = varUInteger.value;
    return uOK;
  }
  return uERR;
}

unpackRC HttpAnswer::unpackVarUInteger7(vm::CellSlice varUInteger7, td::RefInt256 &out) {
  block::gen::VarUInteger::Record varUInteger;
  if (block::gen::VarUInteger(7).unpack(varUInteger7, varUInteger)) {
    out = varUInteger.value;
    return uOK;
  }
  return uERR;
}

unpackRC HttpAnswer::unpackVarUInteger3(vm::CellSlice varUInteger3, td::RefInt256 &out) {
  block::gen::VarUInteger::Record varUInteger;
  if (block::gen::VarUInteger(3).unpack(varUInteger3, varUInteger)) {
    out = varUInteger.value;
    return uOK;
  }
  return uERR;
}

unpackRC HttpAnswer::unpackMaybeVarUInteger3(vm::CellSlice varUInteger3, td::RefInt256 &out) {
  block::gen::VarUInteger t_VarUInteger_3{3};
  block::gen::Maybe t_Maybe_VarUInteger_3{t_VarUInteger_3};
  block::gen::Maybe::Record_just rjst;

  if (t_Maybe_VarUInteger_3.unpack(varUInteger3, rjst)) {
    return unpackVarUInteger3(rjst.value.write(), out);
  } else {
    return uERR;
  }

  return uERR;
}

unpackRC HttpAnswer::unpackInt32(vm::CellSlice int_32Val, td::RefInt256 &out) {
  block::gen::Int t_int32{32};
  out = t_int32.as_integer(int_32Val);
  return uOK;
}

unpackRC HttpAnswer::unpackMaybeInt32(vm::CellSlice int32, td::RefInt256 &out) {
  block::gen::Int t_int32{32};
  block::gen::Maybe t_Maybe_t_int32{t_int32};
  block::gen::Maybe::Record_just rjst;

  if (t_Maybe_t_int32.unpack(int32, rjst)) {
    unpackInt32(rjst.value.write(), out);
  } else {
    return uNULL_ptr;
  }
  return uERR;
}

unpackRC HttpAnswer::unpackGrams(vm::CellSlice grams, block::CurrencyCollection &gramsCurColl) {
  block::gen::Grams::Record gramsR;
  td::RefInt256 gramsInteger;
  if (tlb::unpack(grams, gramsR)) {
    if (unpackVarUInteger16(gramsR.amount.write(), gramsInteger) == uOK) {
      gramsCurColl = block::CurrencyCollection{std::move(gramsInteger), {}};
      return uOK;
    } else {
      return uNULL_ptr;
    }
  } else {
    return uNULL_ptr;
  }
}

unpackRC HttpAnswer::unpackMaybeGrams(vm::CellSlice &grams, block::CurrencyCollection &gramsCurCol) {
  block::gen::Grams t_Grams;
  block::gen::Maybe maybeGrams{t_Grams};
  block::gen::Maybe::Record_just rjst;

  if (maybeGrams.unpack(grams, rjst)) {
    switch (unpackGrams(rjst.value.write(), gramsCurCol)) {
      case uOK:
        return uOK;
      case uERR:
      case uNULL_ptr:
      default:
        return uNULL_ptr;
    }
  } else {
    return uNULL_ptr;
  }
}

unpackRC HttpAnswer::unpackMaybeBalance(vm::CellSlice &grams, int64_t &gramsCurCol) {
  block::gen::Grams t_Grams;
  block::gen::Maybe maybeGrams{t_Grams};
  block::gen::Maybe::Record_just rjst;

  if (maybeGrams.unpack(grams, rjst)) {
    switch (unpackBalance(rjst.value, gramsCurCol)) {
      case uOK:
        return uOK;
      case uERR:
      case uNULL_ptr:
      default:
        return uNULL_ptr;
    }
  } else {
    return uNULL_ptr;
  }
}

unpackRC HttpAnswer::unpackTrStoragePhase(vm::CellSlice storage_ph, UniValue &uvObj) {
  int64_t storage_fees_collected;
  int64_t storage_fees_due;

  block::gen::TrStoragePhase::Record trStoragePhaseRecord;

  if (tlb::unpack(storage_ph, trStoragePhaseRecord)) {
    switch (unpackBalance(trStoragePhaseRecord.storage_fees_collected, storage_fees_collected)) {
      case uOK:
        uvObj.pushKV("storage_fees_collected", storage_fees_collected);
        break;
      case uNULL_ptr:
      case uERR:
      default:
        uvObj.pushKV("storage_fees_collected", NULL);
        break;
    }

    switch (unpackMaybeBalance(trStoragePhaseRecord.storage_fees_due.write(), storage_fees_due)) {
      case uOK:
        uvObj.pushKV("storage_fees_due", storage_fees_due);
        break;
      case uNULL_ptr:
      case uERR:
      default:
        uvObj.pushKV("storage_fees_due", UniValue::VNULL);
        break;
    }
    uvObj.pushKV("acc_status_change", transactionStatus(trStoragePhaseRecord.status_change));
    return uOK;
  } else {
    uvObj.pushKV("error", "Can't unpack maybe TrStoragePhase");
    return uOK;
  }
}

unpackRC HttpAnswer::unpackMaybeTrStoragePhase(vm::CellSlice storage_ph, UniValue &uvObj) {
  block::gen::TrStoragePhase t_TrStoragePhase;
  block::gen::Maybe t_Maybe_TrStoragePhase{t_TrStoragePhase};
  block::gen::Maybe::Record_just rjst;

  if (t_Maybe_TrStoragePhase.unpack(storage_ph, rjst)) {
    switch (unpackTrStoragePhase(rjst.value.write(), uvObj)) {
      case uOK:
        return uOK;
      case uNULL_ptr:
        return uNULL_ptr;
      case uERR:
        uvObj.pushKV("error", "Can't unpack maybe TrStoragePhase");
        return uOK;
      default:
        return uNULL_ptr;
    }
  } else {
    uvObj.pushKV("error", "Can't unpack maybe TrStoragePhase");
    return uOK;
  }
}

unpackRC HttpAnswer::unpackTrCreditPhase(vm::CellSlice credit_ph, UniValue &uvObj) {
  int64_t due_fees_collected;
  block::CurrencyCollection credit;
  std::string creditInt = "";

  block::gen::TrCreditPhase::Record trCreditPhaseRecord;

  if (tlb::unpack(credit_ph, trCreditPhaseRecord)) {
    if (credit.validate_unpack(trCreditPhaseRecord.credit)) {
      switch (unpack_CurrencyCollection(credit, creditInt)) {
        case uOK:
          uvObj.pushKV("credit", creditInt);
          break;
        case uNULL_ptr:
        case uERR:
        default:
          uvObj.pushKV("credit", UniValue::VNULL);
      }
    } else {
      uvObj.pushKV("credit", UniValue::VNULL);
    }

    switch (unpackMaybeBalance(trCreditPhaseRecord.due_fees_collected.write(), due_fees_collected)) {
      case uOK:
        uvObj.pushKV("due_fees_collected", due_fees_collected);
        break;
      case uERR:
      case uNULL_ptr:
      default:
        uvObj.pushKV("due_fees_collected", UniValue::VNULL);
        break;
    }
    return uOK;
  } else {
    return uERR;
  }
}

unpackRC HttpAnswer::unpackMaybeTrCreditPhase(vm::CellSlice credit_ph, UniValue &uvObj) {
  block::gen::TrCreditPhase trCreditPhase;
  block::gen::Maybe t_Maybe_TrComputePhase{trCreditPhase};
  block::gen::Maybe::Record_just rjst;

  switch (t_Maybe_TrComputePhase.get_tag(credit_ph)) {
    case block::gen::Maybe::just:
      if (t_Maybe_TrComputePhase.unpack(credit_ph, rjst)) {
        if (rjst.value.is_null()) {
          return uNULL_ptr;
        }
        switch (unpackTrCreditPhase(*(rjst.value), uvObj)) {
          case uOK:
            return uOK;
          case uNULL_ptr:
            return uNULL_ptr;
          case uERR:
            uvObj.pushKV("error", "Can't unpack maybe_TrCreditPhase");
            return uOK;
          default:
            return uNULL_ptr;
        }
      } else {
        uvObj.pushKV("error", "Can't unpack maybe_TrCreditPhase");
        return uOK;
      }
      break;
    case block::gen::Maybe::nothing:
    default:
      uvObj.pushKV("error", "Can't unpack maybe_TrCreditPhase");
      return uOK;
  }
}

bool HttpAnswer::unpackComputeSkipped(
    block::gen::TrComputePhase::Record_tr_phase_compute_skipped record_tr_phase_compute_skipped, UniValue &uvObj) {
  // no point to realise it
  return uOK;
}

bool HttpAnswer::parseTrComputePhase_aux(block::gen::TrComputePhase_aux::Record record_TrComputePhase_aux,
                                         UniValue &uvObj) {
  td::RefInt256 gas_used;
  td::RefInt256 gas_limit;
  td::RefInt256 gas_credit;
  td::RefInt256 exit_arg;

  switch (unpackVarUInteger7(record_TrComputePhase_aux.gas_used.write(), gas_used)) {
    case uOK:
      uvObj.pushKV("gas_used", (int64_t)(gas_used).write().to_long());
      break;
    case uNULL_ptr:
    case uERR:
    default:
      uvObj.pushKV("gas_used", UniValue::VNULL);
      break;
  }
  switch (unpackVarUInteger7(record_TrComputePhase_aux.gas_limit.write(), gas_limit)) {
    case uOK:
      uvObj.pushKV("gas_limit", (int64_t)(gas_limit).write().to_long());
      break;
    case uNULL_ptr:
    case uERR:
    default:
      uvObj.pushKV("gas_limit", UniValue::VNULL);
      break;
  }
  switch (unpackMaybeVarUInteger3(record_TrComputePhase_aux.gas_credit.write(), gas_credit)) {
    case uOK:
      uvObj.pushKV("gas_credit", (int64_t)(gas_credit).write().to_long());
      break;
    case uNULL_ptr:
    case uERR:
    default:
      uvObj.pushKV("gas_credit", UniValue::VNULL);
      break;
  }

  uvObj.pushKV("mode", record_TrComputePhase_aux.mode);
  uvObj.pushKV("exit_code", record_TrComputePhase_aux.exit_code);

  switch (unpackMaybeInt32(record_TrComputePhase_aux.exit_arg.write(), exit_arg)) {
    case uOK:
      uvObj.pushKV("exit_arg", (int64_t)(exit_arg).write().to_long());
      break;
    case uERR:
    case uNULL_ptr:
    default:
      uvObj.pushKV("exit_arg", UniValue::VNULL);
  }

  uvObj.pushKV("vm_steps", (uint64_t)record_TrComputePhase_aux.vm_steps);
  uvObj.pushKV("vm_init_state_hash", record_TrComputePhase_aux.vm_init_state_hash.to_hex());
  uvObj.pushKV("vm_final_state_hash", record_TrComputePhase_aux.vm_init_state_hash.to_hex());

  return true;
}

unpackRC HttpAnswer::unpackComputeVM(block::gen::TrComputePhase::Record_tr_phase_compute_vm record_tr_phase_compute_vm,
                                     UniValue &uvObj) {
  int64_t gas_fees;
  uvObj.pushKV("success", record_tr_phase_compute_vm.success);
  uvObj.pushKV("msg_state_used", record_tr_phase_compute_vm.msg_state_used);
  uvObj.pushKV("account_activated", record_tr_phase_compute_vm.account_activated);

  if (unpackBalance(record_tr_phase_compute_vm.gas_fees, gas_fees)) {
    uvObj.pushKV("gas_fees", gas_fees);
  } else {
    uvObj.pushKV("gas_fees", UniValue::VNULL);
  }

  if (parseTrComputePhase_aux(record_tr_phase_compute_vm.r1, uvObj)) {
    return uOK;
  } else {
    uvObj.pushKV("error", "Can't unpack TrComputePhase_aux");
    return uERR;
  }
}

unpackRC HttpAnswer::MonteCarloTrComputePhase(vm::CellSlice compute_ph, UniValue &uvObj) {
  block::gen::TrComputePhase::Record_tr_phase_compute_skipped record_tr_phase_compute_skipped;
  block::gen::TrComputePhase::Record_tr_phase_compute_vm record_tr_phase_compute_vm;

  auto cs1 = compute_ph.clone();
  auto cs2 = compute_ph.clone();

  if (tlb::unpack(cs1, record_tr_phase_compute_skipped)) {
    uvObj.pushKV("type", "compute_skipped");
    uvObj.pushKV("reason", record_tr_phase_compute_skipped.reason);
    return uOK;
  }
  if (tlb::unpack(cs2, record_tr_phase_compute_vm)) {
    uvObj.pushKV("type", "compute_vm");
    switch (unpackComputeVM(record_tr_phase_compute_vm, uvObj)) {
      case uOK:
        return uOK;
      case uNULL_ptr:
        return uNULL_ptr;
      case uERR:
        return uERR;
      default:
        return uNULL_ptr;
    }
  }
  return uERR;
}

unpackRC HttpAnswer::unpackTrComputePhase(vm::CellSlice compute_ph, UniValue &uvObj) {
  switch (MonteCarloTrComputePhase(compute_ph, uvObj)) {
    case uOK:
      return uOK;
    case uNULL_ptr:
      return uNULL_ptr;
    case uERR:
      uvObj.pushKV("error", "Can't unpack TrComputePhase");
      return uOK;
    default:
      return uNULL_ptr;
  }
}

unpackRC HttpAnswer::unpackMaybeRefTrActionPhase(vm::CellSlice action_ph, UniValue &uvObj) {
  block::gen::TrActionPhase trActionPhase;
  block::gen::RefT t_Ref_TrActionPhase{trActionPhase};
  block::gen::Maybe t_Maybe_Ref_TrActionPhase{t_Ref_TrActionPhase};
  block::gen::Maybe::Record_just rjst;

  if (t_Maybe_Ref_TrActionPhase.unpack(action_ph, rjst)) {
    switch (unpackTrActionPhase(vm::load_cell_slice(rjst.value->prefetch_ref()), uvObj)) {
      case uOK:
        return uOK;
      case uNULL_ptr:
        return uNULL_ptr;
      case uERR:
        uvObj.pushKV("error", "Can't unpack ^TrActionPhase");
        return uOK;
      default:
        return uNULL_ptr;
    }
  } else {
    uvObj.pushKV("error", "Can't unpach maybe_TrActionPhase");
    return uOK;
  }
  return uERR;
}

unpackRC HttpAnswer::unpackStorageUsedShort(vm::CellSlice storageUsedSHortCS, UniValue &uvObj) {
  block::gen::StorageUsedShort::Record storageUsedSHortRecord;
  td::RefInt256 cells;
  td::RefInt256 bits;

  if (tlb::unpack(storageUsedSHortCS, storageUsedSHortRecord)) {
    switch (unpackVarUInteger7(storageUsedSHortRecord.cells.write(), cells)) {
      case uOK:
        uvObj.pushKV("cells", (int64_t)cells->to_long());
        break;
      case uNULL_ptr:
      case uERR:
      default:
        uvObj.pushKV("cells", UniValue::VNULL);
    }
    switch (unpackVarUInteger7(storageUsedSHortRecord.bits.write(), bits)) {
      case uOK:
        uvObj.pushKV("bits", (int64_t)bits->to_long());
        break;
      case uNULL_ptr:
      case uERR:
      default:
        uvObj.pushKV("bits", UniValue::VNULL);
    }
    return uOK;
  }
  return uERR;
}

std::string HttpAnswer::transactionStatus(char status) {
  switch (status) {
    case 0:
      return "unchanged";
    case 1:
      return "frozen";
    case 2:
      return "deleted";
    default:
      return "unknown";
  }
}

unpackRC HttpAnswer::unpackTrActionPhase(vm::CellSlice action_ph, UniValue &uvObj) {
  block::gen::TrActionPhase::Record trActionPhaseRecord;
  int64_t total_fwd_fees;
  int64_t total_action_fees;
  td::RefInt256 result_arg;

  if (tlb::unpack(action_ph, trActionPhaseRecord)) {
    uvObj.pushKV("succses", trActionPhaseRecord.success);
    uvObj.pushKV("valid", trActionPhaseRecord.valid);
    uvObj.pushKV("no_funds", trActionPhaseRecord.no_funds);
    uvObj.pushKV("status_change", transactionStatus(trActionPhaseRecord.status_change));
    uvObj.pushKV("no_funds", trActionPhaseRecord.no_funds);

    switch (unpackMaybeBalance(trActionPhaseRecord.total_fwd_fees.write(), total_fwd_fees)) {
      case uOK:
        uvObj.pushKV("total_fwd_fees", total_fwd_fees);
        break;
      case uNULL_ptr:
      case uERR:
      default:
        uvObj.pushKV("total_fwd_fees", UniValue::VNULL);
        break;
    }
    switch (unpackMaybeBalance(trActionPhaseRecord.total_action_fees.write(), total_action_fees)) {
      case uOK:
        uvObj.pushKV("total_action_fees", total_action_fees);
        break;
      case uNULL_ptr:
      case uERR:
      default:
        uvObj.pushKV("total_action_fees", UniValue::VNULL);
        break;
    }

    uvObj.pushKV("result_code", trActionPhaseRecord.result_code);

    switch (unpackMaybeInt32(trActionPhaseRecord.result_arg.write(), result_arg)) {
      case uOK:
        uvObj.pushKV("result_arg", (int64_t)result_arg->to_long());
        break;
      case uNULL_ptr:
      case uERR:
      default:
        uvObj.pushKV("result_arg", UniValue::VNULL);
        break;
    }

    uvObj.pushKV("tot_actions", trActionPhaseRecord.tot_actions);
    uvObj.pushKV("spec_actions", trActionPhaseRecord.spec_actions);
    uvObj.pushKV("skipped_actions", trActionPhaseRecord.skipped_actions);
    uvObj.pushKV("msgs_created", trActionPhaseRecord.msgs_created);
    uvObj.pushKV("action_list_hash", trActionPhaseRecord.action_list_hash.to_hex());

    uvObj.pushKV("tot_msg_size", [this](vm::CellSlice cs) -> const UniValue {
      UniValue uvObjMsg;
      uvObjMsg.setObject();
      switch (unpackStorageUsedShort(cs, uvObjMsg)) {
        case uOK:
          break;
        case uNULL_ptr:
        case uERR:
        default:
          uvObjMsg.pushKV("error", "Can't unpack StorageUsedShort");
          break;
      }
      return uvObjMsg;
    }(trActionPhaseRecord.tot_msg_size.write()));
    return uOK;
  }
  return uNULL_ptr;
}

bool HttpAnswer::unpackOrdinaryTrans(block::gen::TransactionDescr::Record_trans_ord ordinaryTrans, UniValue &uvObj) {
  UniValue uvObjDescription;
  uvObjDescription.setObject();

  uvObjDescription.pushKV("credit_first", ordinaryTrans.credit_first);
  uvObjDescription.pushKV("aborted", ordinaryTrans.aborted);
  uvObjDescription.pushKV("destroyed", ordinaryTrans.destroyed);

  UniValue uvObjStorage;
  uvObjStorage.setObject();
  if (unpackMaybeTrStoragePhase(ordinaryTrans.storage_ph.write(), uvObjStorage) != uNULL_ptr) {
    uvObjDescription.pushKV("storage", uvObjStorage);
  } else {
    uvObjDescription.pushKV("storage", UniValue::VNULL);
  }

  UniValue uvObjCredit;
  uvObjCredit.setObject();
  if (unpackMaybeTrCreditPhase(ordinaryTrans.credit_ph.write(), uvObjCredit) != uNULL_ptr) {
    uvObjDescription.pushKV("credit", uvObjCredit);
  } else {
    uvObjDescription.pushKV("credit", UniValue::VNULL);
  }

  UniValue uvObjCompute;
  uvObjCompute.setObject();
  if (unpackTrComputePhase(ordinaryTrans.compute_ph.write(), uvObjCompute) != uNULL_ptr) {
    uvObjDescription.pushKV("compute", uvObjCompute);
  } else {
    uvObjDescription.pushKV("compute", UniValue::VNULL);
  }

  UniValue uvObjAction;
  uvObjAction.setObject();
  if (unpackMaybeRefTrActionPhase(ordinaryTrans.action.write(), uvObjAction) != uNULL_ptr) {
    uvObjDescription.pushKV("action", uvObjAction);
  } else {
    uvObjDescription.pushKV("action", UniValue::VNULL);
  }

  uvObj.pushKV("record", uvObjDescription);
  return true;
}

bool HttpAnswer::unpackTickTockTrans(block::gen::TransactionDescr::Record_trans_tick_tock description,
                                     UniValue &uvObj) {
  UniValue uvObjDescription;
  uvObjDescription.setObject();

  uvObjDescription.pushKV("is_tock", description.is_tock);

  UniValue uvObjStorage;
  uvObjStorage.setObject();
  if (unpackTrStoragePhase(description.storage_ph.write(), uvObjStorage) != uNULL_ptr) {
    uvObjDescription.pushKV("storage", uvObjStorage);
  } else {
    uvObjDescription.pushKV("storage", UniValue::VNULL);
  }

  UniValue uvObjCompute;
  uvObjCompute.setObject();
  if (unpackTrComputePhase(description.compute_ph.write(), uvObjCompute) != uNULL_ptr) {
    uvObjDescription.pushKV("compute", uvObjCompute);
  } else {
    uvObjDescription.pushKV("compute", UniValue::VNULL);
  }

  UniValue uvObjAction;
  uvObjAction.setObject();
  if (unpackMaybeRefTrActionPhase(description.action.write(), uvObjAction) != uNULL_ptr) {
    uvObjDescription.pushKV("action", uvObjAction);
  } else {
    uvObjDescription.pushKV("action", UniValue::VNULL);
  }

  uvObjDescription.pushKV("aborted", description.aborted);
  uvObjDescription.pushKV("destroyed", description.destroyed);

  uvObj.pushKV("record", uvObjDescription);
  return true;
}

unpackRC HttpAnswer::unpackSplitMergeInfo(vm::CellSlice split_info, UniValue &uvObj) {
  block::gen::SplitMergeInfo::Record storageMergeInfo;

  if (tlb::unpack(split_info, storageMergeInfo)) {
    uvObj.pushKV("cur_shard_pfx_len", storageMergeInfo.cur_shard_pfx_len);
    uvObj.pushKV("acc_split_depth", storageMergeInfo.acc_split_depth);
    uvObj.pushKV("this_addr", storageMergeInfo.this_addr.to_hex());
    uvObj.pushKV("sibling_addr", storageMergeInfo.sibling_addr.to_hex());
    return uOK;
  } else {
    return uERR;
  }
}

//Just believe, untested
bool HttpAnswer::unpackMergePrepareTrans(block::gen::TransactionDescr::Record_trans_merge_prepare description,
                                         UniValue &uvObj) {
  UniValue uvObjDescription;
  uvObjDescription.setObject();

  UniValue uvObjSplit;
  uvObjSplit.setObject();
  if (unpackSplitMergeInfo(description.split_info.write(), uvObjSplit) != uNULL_ptr) {
    uvObjDescription.pushKV("split_info", uvObjSplit);
  } else {
    uvObjDescription.pushKV("split_info", UniValue::VNULL);
  }

  UniValue uvObjStorage;
  uvObjStorage.setObject();
  if (unpackTrStoragePhase(description.storage_ph.write(), uvObjStorage) != uNULL_ptr) {
    uvObjDescription.pushKV("storage", uvObjStorage);
  } else {
    uvObjDescription.pushKV("storage", UniValue::VNULL);
  }

  uvObjDescription.pushKV("aborted", description.aborted);
  return true;
}

bool HttpAnswer::unpackMergeInstallTrans(block::gen::TransactionDescr::Record_trans_merge_install description,
                                         UniValue &uvObj) {
  UniValue uvObjDescription;
  uvObjDescription.setObject();

  UniValue uvObjSplit;
  uvObjSplit.setObject();
  if (unpackSplitMergeInfo(description.split_info.write(), uvObjSplit) != uNULL_ptr) {
    uvObjDescription.pushKV("split_info", uvObjSplit);
  } else {
    uvObjDescription.pushKV("split_info", UniValue::VNULL);
  }

  UniValue uvObjTrans;
  uvObjTrans.setObject();
  if (unpackTransaction(vm::load_cell_slice(description.prepare_transaction), uvObjTrans)) {
    uvObjDescription.pushKV("prepare_transaction", uvObjTrans);
  } else {
    uvObjDescription.pushKV("prepare_transaction", UniValue::VNULL);
  }

  UniValue uvObjStorage;
  uvObjStorage.setObject();
  if (unpackMaybeTrStoragePhase(description.storage_ph.write(), uvObjStorage) != uNULL_ptr) {
    uvObjDescription.pushKV("storage", uvObjStorage);
  } else {
    uvObjDescription.pushKV("storage", UniValue::VNULL);
  }

  UniValue uvObjCredit;
  uvObjCredit.setObject();
  if (unpackMaybeTrCreditPhase(description.credit_ph.write(), uvObjCredit) != uNULL_ptr) {
    uvObjDescription.pushKV("credit", uvObjCredit);
  } else {
    uvObjDescription.pushKV("credit", UniValue::VNULL);
  }

  UniValue uvObjCompute;
  uvObjCompute.setObject();
  if (unpackTrComputePhase(description.compute_ph.write(), uvObjCompute) != uNULL_ptr) {
    uvObjDescription.pushKV("compute", uvObjCompute);
  } else {
    uvObjDescription.pushKV("compute", UniValue::VNULL);
  }

  UniValue uvObjAction;
  uvObjAction.setObject();
  if (unpackMaybeRefTrActionPhase(description.action.write(), uvObjAction) != uNULL_ptr) {
    uvObjDescription.pushKV("action", uvObjAction);
  } else {
    uvObjDescription.pushKV("action", UniValue::VNULL);
  }

  uvObjDescription.pushKV("aborted", description.aborted);
  uvObjDescription.pushKV("destroyed", description.destroyed);

  return true;
}

bool HttpAnswer::unpackSplitInstallTrans(block::gen::TransactionDescr::Record_trans_split_install description,
                                         UniValue &uvObj) {
  UniValue uvObjDescription;
  uvObjDescription.setObject();
  block::gen::Transaction::Record transactionRecord;

  UniValue uvObjSplit;
  uvObjSplit.setObject();
  if (unpackSplitMergeInfo(description.split_info.write(), uvObjSplit) != uNULL_ptr) {
    uvObjDescription.pushKV("split_info", uvObjSplit);
  } else {
    uvObjDescription.pushKV("split_info", UniValue::VNULL);
  }

  UniValue uvObjTrans;
  uvObjTrans.setObject();
  if (unpackTransaction(vm::load_cell_slice(description.prepare_transaction), uvObjTrans)) {
    uvObjDescription.pushKV("prepare_transaction", uvObjTrans);
  } else {
    uvObjDescription.pushKV("prepare_transaction", UniValue::VNULL);
  }
  return true;
}

bool HttpAnswer::unpackSplitPrepareTrans(block::gen::TransactionDescr::Record_trans_split_prepare description,
                                         UniValue &uvObj) {
  UniValue uvObjDescription;
  uvObjDescription.setObject();

  UniValue uvObjSplit;
  uvObjSplit.setObject();
  if (unpackSplitMergeInfo(description.split_info.write(), uvObjSplit) != uNULL_ptr) {
    uvObjDescription.pushKV("split_info", uvObjSplit);
  } else {
    uvObjDescription.pushKV("split_info", UniValue::VNULL);
  }

  UniValue uvObjStorage;
  uvObjStorage.setObject();
  if (unpackMaybeTrStoragePhase(description.storage_ph.write(), uvObjStorage) != uNULL_ptr) {
    uvObjDescription.pushKV("storage", uvObjStorage);
  } else {
    uvObjDescription.pushKV("storage", UniValue::VNULL);
  }

  UniValue uvObjCompute;
  uvObjCompute.setObject();
  if (unpackTrComputePhase(description.compute_ph.write(), uvObjCompute) != uNULL_ptr) {
    uvObjDescription.pushKV("compute", uvObjCompute);
  } else {
    uvObjDescription.pushKV("compute", UniValue::VNULL);
  }

  UniValue uvObjAction;
  uvObjAction.setObject();
  if (unpackMaybeRefTrActionPhase(description.action.write(), uvObjAction) != uNULL_ptr) {
    uvObjDescription.pushKV("action", uvObjAction);
  } else {
    uvObjDescription.pushKV("action", UniValue::VNULL);
  }

  uvObjDescription.pushKV("aborted", description.aborted);
  uvObjDescription.pushKV("destroyed", description.destroyed);

  return true;
}

bool HttpAnswer::unpackStorageTrans(block::gen::TransactionDescr::Record_trans_storage description, UniValue &uvObj) {
  UniValue uvObjDescription;
  uvObjDescription.setObject();

  UniValue uvObjStorage;
  uvObjStorage.setObject();
  if (unpackTrStoragePhase(description.storage_ph.write(), uvObjStorage) != uNULL_ptr) {
    uvObjDescription.pushKV("storage", uvObjStorage);
  } else {
    uvObjDescription.pushKV("storage", UniValue::VNULL);
  }

  return true;
}

UniValue HttpAnswer::MonteCarloDescription(vm::Ref<vm::Cell> description) {
  UniValue uvObjTransDescription;
  uvObjTransDescription.setObject();

  vm::CellSlice slice = vm::load_cell_slice(description);

  block::gen::TransactionDescr transactionDescr;
  block::gen::TransactionDescr::Record_trans_ord record_trans_ord;
  block::gen::TransactionDescr::Record_trans_merge_prepare record_trans_merge_prepare;
  block::gen::TransactionDescr::Record_trans_merge_install record_trans_merge_install;
  block::gen::TransactionDescr::Record_trans_split_install record_trans_split_install;
  block::gen::TransactionDescr::Record_trans_split_prepare record_trans_split_prepare;
  block::gen::TransactionDescr::Record_trans_storage record_trans_storage;
  block::gen::TransactionDescr::Record_trans_tick_tock record_trans_tick_tock;

  switch (transactionDescr.get_tag(slice)) {
    case block::gen::TransactionDescr::trans_ord:
      if (transactionDescr.cell_unpack(description, record_trans_ord)) {
        uvObjTransDescription.pushKV("type", "ordinary");
        unpackOrdinaryTrans(record_trans_ord, uvObjTransDescription);
        return uvObjTransDescription;
      }
      break;
    case block::gen::TransactionDescr::trans_merge_prepare:
      if (transactionDescr.cell_unpack(description, record_trans_merge_prepare)) {
        uvObjTransDescription.pushKV("type", "merge_prepare");
        unpackMergePrepareTrans(record_trans_merge_prepare, uvObjTransDescription);
        return uvObjTransDescription;
      }
      break;
    case block::gen::TransactionDescr::trans_merge_install:
      if (transactionDescr.cell_unpack(description, record_trans_merge_install)) {
        uvObjTransDescription.pushKV("type", "merge_install");
        unpackMergeInstallTrans(record_trans_merge_install, uvObjTransDescription);
        return uvObjTransDescription;
      }
      break;
    case block::gen::TransactionDescr::trans_split_install:
      if (transactionDescr.cell_unpack(description, record_trans_split_install)) {
        uvObjTransDescription.pushKV("type", "split_install");
        unpackSplitInstallTrans(record_trans_split_install, uvObjTransDescription);
        return uvObjTransDescription;
      }
      break;
    case block::gen::TransactionDescr::trans_split_prepare:
      if (transactionDescr.cell_unpack(description, record_trans_split_prepare)) {
        uvObjTransDescription.pushKV("type", "split_prepare");
        unpackSplitPrepareTrans(record_trans_split_prepare, uvObjTransDescription);
        return uvObjTransDescription;
      }
      break;
    case block::gen::TransactionDescr::trans_storage:
      if (transactionDescr.cell_unpack(description, record_trans_storage)) {
        uvObjTransDescription.pushKV("type", "storage");
        unpackStorageTrans(record_trans_storage, uvObjTransDescription);
        return uvObjTransDescription;
      }
      break;
    case block::gen::TransactionDescr::trans_tick_tock:
      if (transactionDescr.cell_unpack(description, record_trans_tick_tock)) {
        uvObjTransDescription.pushKV("type", "tick_tock");
        unpackTickTockTrans(record_trans_tick_tock, uvObjTransDescription);
        return uvObjTransDescription;
      }
      break;
    default:
      uvObjTransDescription.pushKV("error", "Uncknown description");
      return uvObjTransDescription;
  }

  uvObjTransDescription.pushKV("error", "Can't unpack description");
  return uvObjTransDescription;
}

unpackRC HttpAnswer::unpackBalance(td::Ref<vm::CellSlice> balance_ref, int64_t &balance_out) {
  vm::CellSlice balance_slice = *balance_ref;
  td::RefInt256 balance = block::tlb::t_Grams.as_integer_skip(balance_slice);
  if (balance.is_null()) {
    return uNULL_ptr;
  }
  td::BigInt256::word_t res = balance->to_long();
  if (res == td::int64(~0ULL << 63)) {
    return uNULL_ptr;
  }
  balance_out = res;
  return uOK;
}

// Maybe we should to think about dimension of balance_out
unpackRC HttpAnswer::unpackBalance(td::Ref<vm::CellSlice> balance_ref, int64_t &balance_out, td::BigInt256 &fees) {
  vm::CellSlice balance_slice = balance_ref.write();
  td::RefInt256 balance = block::tlb::t_Grams.as_integer_skip(balance_slice);
  if (balance.is_null()) {
    return uNULL_ptr;
  }
  td::BigInt256::word_t res = balance->to_long();
  if (res == td::int64(~0ULL << 63)) {
    return uNULL_ptr;
  }
  balance_out = res;
  fees = *balance;
  return uOK;
}

unpackRC HttpAnswer::unpackTransactionSmall(vm::CellSlice transaction, ton::BlockIdExt block_id, UniValue &uvObj,
                                            std::shared_ptr<HttpAnswer::TransactionDescr> transDescr) {
  block::gen::Transaction::Record trans;
  block::CurrencyCollection total_fees;
  std::string total_feesInt;
  std::string transactionType;
  td::BigInt256 feesTotal;

  if (tlb::unpack(transaction, trans)) {
    uvObj.pushKV("address",
                 block::StdAddress(block_id.id.workchain, trans.account_addr.cbits(), true, false).rserialize(true));
    if (transDescr != nullptr) {
      transDescr->addr = block::StdAddress(block_id.id.workchain, trans.account_addr.cbits(), true, false);
    }

    if (total_fees.validate_unpack(trans.total_fees)) {
      switch (unpack_CurrencyCollection(total_fees, total_feesInt, feesTotal)) {
        case uOK:
          this->fees += feesTotal;
          break;
        case uERR:
        case uNULL_ptr:
        default:
          break;
      }
    }

    if (!checkTransactionType(trans.description, transactionTypes::trans_ord, transactionType)) {
      uvObj.pushKV("type", transactionType);
      uvObj.pushKV("lt", (uint64_t)trans.lt);
      if (transDescr != nullptr) {
        transDescr->lt = trans.lt;
      }
      uvObj.pushKV("time", (uint64_t)trans.now);
      return uOK;
    }

    uvObj.pushKV("type", transactionType);
    uvObj.pushKV("lt", (uint64_t)trans.lt);
    if (transDescr != nullptr) {
      transDescr->lt = trans.lt;
    }
    uvObj.pushKV("time", (uint64_t)trans.now);

    vm::Dictionary dict{trans.r1.out_msgs, 15};
    UniValue uvArr;
    UniValue uvObjMessage;
    uvObjMessage.setObject();
    uvArr.setArray();

    td::Ref<vm::Cell> in_msg = trans.r1.in_msg->prefetch_ref();
    if (in_msg.not_null()) {
      unpackMessage(MessageCell{in_msg}, uvObjMessage, messageInType);
      uvArr.push_back(uvObjMessage);
      uvObj.pushKV("messageIn", uvArr);
    }

    uvObjMessage.clear();
    uvObjMessage.setObject();
    uvArr.clear();
    uvArr.setArray();

    for (int x = 0; x < trans.outmsg_cnt && x < 100; x++) {
      auto bit_array = td::BitArray<15>{x + 1};
      td::Ref<vm::Cell> out_msg = dict.lookup_ref(std::move(bit_array));
      if (out_msg.not_null()) {
        unpackMessage(MessageCell{std::move(out_msg)}, uvObjMessage, messageOutType);
      }
    }
    uvObj.pushKV("fee", this->fees.to_dec_string());
    unpackMessageBody(trans.r1.in_msg.write(), trans.outmsg_cnt, trans.r1.out_msgs.write(), uvObj);
    return uOK;
  }
  return uERR;
}

void HttpAnswer::serializeObject(TransactionSmall trans_c, UniValue &uvObj,
                                 std::shared_ptr<HttpAnswer::TransactionDescr> transDescr) {
  if (trans_c.root.is_null()) {
    abort("transaction not found");
    return;
  }

  try {
    vm::CellSlice cs = vm::load_cell_slice(trans_c.root);
    switch (unpackTransactionSmall(std::move(cs), trans_c.block_id, uvObj, transDescr)) {
      case uOK:
        break;
      case uNULL_ptr:
      case uERR:
      default:
        abort("Can't unpack transaction");
        return;
    }
  } catch (const std::exception &e) {
    abort("Can't unpack transaction");
    return;
  }

  if (transDescr != nullptr) {
    transDescr->hash = trans_c.root->get_hash().as_bitslice().bits();
  }
  uvObj.pushKV("hash", trans_c.root->get_hash().to_hex());
  this->fees = 0;
}

HttpAnswer::TransactionDescr HttpAnswer::serializeTransactionSmall(TransactionSmall trans_c, UniValue &uvObj) {
  std::shared_ptr<HttpAnswer::TransactionDescr> transDescr =
      std::make_shared<HttpAnswer::TransactionDescr>(HttpAnswer::TransactionDescr());
  this->serializeObject(trans_c, uvObj, transDescr);
  return {transDescr->addr, transDescr->lt, transDescr->hash};
}

std::string HttpAnswer::transactionType2String(transactionTypes type) {
  switch (type) {
    case transactionTypes::trans_merge_install:
      return "merge_install";
    case transactionTypes::trans_merge_prepare:
      return "merge_prepare";
    case transactionTypes::trans_ord:
      return "ordinary";
    case transactionTypes::trans_split_install:
      return "split_install";
    case transactionTypes::trans_split_prepare:
      return "split_prepare";
    case transactionTypes::trans_storage:
      return "storage";
    case transactionTypes::trans_tick_tock:
      return "tick_tock";
    default:
      return "";
  }
  return "";
}

bool HttpAnswer::checkTransactionType(vm::Ref<vm::Cell> description, transactionTypes type, std::string &currentType) {
  vm::CellSlice slice = vm::load_cell_slice(description);
  block::gen::TransactionDescr transactionDescr;
  currentType = transactionType2String((transactionTypes)transactionDescr.get_tag(slice));

  if (transactionDescr.get_tag(slice) == type) {
    return true;
  } else {
    return false;
  }
}

bool HttpAnswer::unpackTransaction(vm::CellSlice transaction, UniValue &uvObj) {
  block::gen::Transaction::Record trans;
  td::BigInt256 total_fees;

  if (tlb::unpack(transaction, trans)) {
    int64_t fees = 0;
    switch (unpackBalance(trans.total_fees, fees, total_fees)) {
      case uOK:
        uvObj.pushKV("fees", total_fees.to_dec_string());
        this->fees += total_fees;
        break;
      case uNULL_ptr:
      default:
        uvObj.pushKV("fees", UniValue::VNULL);
        break;
    }

    uvObj.pushKV("description", MonteCarloDescription(trans.description));
    uvObj.pushKV("lt", (uint64_t)trans.lt);
    uvObj.pushKV("time", (uint64_t)trans.now);

    vm::Dictionary dict{trans.r1.out_msgs, 15};
    UniValue uvArr;
    uvArr.setArray();

    auto in_msg = trans.r1.in_msg->prefetch_ref();
    if (in_msg.not_null()) {
      uvArr.setObject();
      unpackMessage(MessageCell{in_msg}, uvArr, messageInType, false);
      uvObj.pushKV("messageIn", uvArr);
    }

    uvArr.clear();
    uvArr.setArray();
    for (int x = 0; x < trans.outmsg_cnt && x < 100; x++) {
      UniValue uvMessage;
      uvMessage.setObject();
      auto out_msg = dict.lookup_ref(td::BitArray<15>{x});
      unpackMessage(MessageCell{out_msg}, uvMessage, messageOutType, false);
      uvArr.push_back(uvMessage);
    }

    uvObj.pushKV("messageOut", uvArr);
    uvObj.pushKV("total_fees", this->fees.to_dec_string());
    return true;
  }
  return false;
}

void HttpAnswer::serializeObject(TransactionCell trans_c) {
  if (trans_c.root.is_null()) {
    abort("transaction not found");
    return;
  }

  try {
    if (!unpackTransaction(vm::load_cell_slice(trans_c.root), jsonObject)) {
      abort("Can't unpack transaction");
      return;
    }
  } catch (const std::exception &e) {
    abort("Can't unpack transaction");
    return;
  }
  jsonObject.pushKV("block", serializeObject(BlockLink{trans_c.block_id}));
  jsonObject.pushKV("workchain", trans_c.addr.workchain);
  jsonObject.pushKV("account_hex", trans_c.addr.addr.to_hex());
  jsonObject.pushKV("account", trans_c.addr.rserialize(true));
  jsonObject.pushKV("hash", trans_c.root->get_hash().to_hex());
}

void HttpAnswer::serializeObject(block::StdAddress addr, ton::LogicalTime lt, ton::Bits256 hash, UniValue &uvObj) {
  uvObj.pushKV("addr", addr.rserialize(true));
  uvObj.pushKV("lt", (uint64_t)lt);
  uvObj.pushKV("hash", hash.to_hex());
}

void HttpAnswer::serializeObject(AccountCell acc_c) {
  ton::BlockIdExt block_id = acc_c.block_id;
  ton::LogicalTime last_trans_lt = 0;
  ton::Bits256 last_trans_hash;
  last_trans_hash.set_zero();

  if (!block_id.is_valid_full()) {
    abort(PSTRING() << "shard block id " << block_id.to_str() << " in answer is invalid");
    return;
  }

  if (!ton::shard_contains(block_id.shard_full(), ton::extract_addr_prefix(acc_c.addr.workchain, acc_c.addr.addr))) {
    abort(PSTRING() << "received data from shard block " << block_id.to_str()
                    << " that cannot contain requested account " << acc_c.addr.workchain << ":"
                    << acc_c.addr.addr.to_hex());
    return;
  }

  if (acc_c.q_roots.size() != 2) {
    abort(PSTRING() << "account state proof must have exactly two roots");
    return;
  }

  try {
    td::Ref<vm::Cell> state_root = vm::MerkleProof::virtualize(acc_c.q_roots[1], 1);
    if (state_root.is_null()) {
      abort("account state proof is invalid");
      return;
    }
    block::gen::ShardStateUnsplit::Record sstate;
    if (!(tlb::unpack_cell(std::move(state_root), sstate))) {
      abort("cannot unpack state header");
      return;
    }
    vm::AugmentedDictionary accounts_dict{vm::load_cell_slice_ref(sstate.accounts), 256, block::tlb::aug_ShardAccounts};
    td::Ref<vm::CellSlice> acc_csr = accounts_dict.lookup(acc_c.addr.addr);
    if (acc_csr.not_null()) {
      if (acc_c.root.is_null()) {
        abort(PSTRING() << "account state proof shows that account state for " << acc_c.addr.workchain << ":"
                        << acc_c.addr.addr.to_hex() << " must be non-empty, but it actually is empty");
        return;
      }
      block::gen::ShardAccount::Record acc_info;
      if (!tlb::csr_unpack(std::move(acc_csr), acc_info)) {
        abort("cannot unpack ShardAccount from proof");
        return;
      }
      if (acc_info.account->get_hash().bits().compare(acc_c.root->get_hash().bits(), 256)) {
        abort(PSTRING() << "account state hash mismatch: Merkle proof expects "
                        << acc_info.account->get_hash().bits().to_hex(256) << " but received data has "
                        << acc_c.root->get_hash().bits().to_hex(256));
        return;
      }
      last_trans_hash = acc_info.last_trans_hash;
      last_trans_lt = acc_info.last_trans_lt;
    } else if (acc_c.root.not_null()) {
      abort(PSTRING() << "account state proof shows that account state for " << acc_c.addr.workchain << ":"
                      << acc_c.addr.addr.to_hex() << " must be empty, but it is not");
      return;
    }
  } catch (vm::VmError err) {
    abort(PSTRING() << "error while traversing account proof : " << err.get_msg());
    return;
  } catch (vm::VmVirtError err) {
    abort(PSTRING() << "virtualization error while traversing account proof : " << err.get_msg());
    return;
  }

  jsonObject.pushKV("block", serializeObject(BlockLink{acc_c.block_id}));

  block::gen::Account::Record_account acc;
  block::gen::AccountStorage::Record store;
  block::CurrencyCollection balance;
  block::gen::AccountState state;
  if (tlb::unpack_cell(acc_c.root, acc) && tlb::csr_unpack(acc.storage, store) && balance.unpack(store.balance)) {
    std::string grams = "";
    switch (unpack_CurrencyCollection(balance, grams)) {
      case uOK:
        jsonObject.pushKV("balance", grams);
        break;
      case uNULL_ptr:
      case uERR:
      default:
        jsonObject.pushKV("error", "Can't unpack balance");
        break;
    }
  } else {
    jsonObject.pushKV("balance", UniValue::VNULL);
  }

  switch (state.check_tag(*(store.state))) {
    case block::gen::AccountState::account_uninit:
      jsonObject.pushKV("state", "uninit");
      break;
    case block::gen::AccountState::account_active: {
      jsonObject.pushKV("state", "active");
      block::gen::AccountState::Record_account_active record_account_active;
      if (tlb::unpack(store.state.write(), record_account_active)) {
        unpackStateInit(record_account_active.x.write(), acc.addr, jsonObject);
      }
    } break;
    case block::gen::AccountState::account_frozen:
      jsonObject.pushKV("state", "frozen");
      break;
  }

  jsonObject.pushKV("account_hex", acc_c.addr.addr.to_hex());
  jsonObject.pushKV("account", acc_c.addr.rserialize(true));

  if (last_trans_lt > 0) {
    jsonObject.pushKV("last_transaction",
                      serializeTransaction(TransactionLink{acc_c.addr, last_trans_lt, last_trans_hash}));
  } else {
    jsonObject.pushKV("last_transaction", UniValue::VNULL);
  }

  return;
}

std::string HttpAnswer::walletType(std::string code) {
  std::transform(code.begin(), code.end(), code.begin(), [](unsigned char c) { return std::toupper(c); });
  if (code == v1r1) {
    return "v1r1";
  }
  if (code == v1r2) {
    return "v1r2";
  }
  if (code == v1r3) {
    return "v1r3";
  }
  if (code == v2r1) {
    return "v2r1";
  }
  if (code == v2r2) {
    return "v2r2";
  }
  if (code == v3r1) {
    return "v3r1";
  }
  if (code == v3r2) {
    return "v3r2";
  }
  if (code == v4r2) {
    return "v4r2";
  }
  if (code == lock_up) {
    return "Lockup";
  }
  return "unknown";
}

unpackRC HttpAnswer::unpackStateInit(vm::CellSlice cs, td::Ref<vm::CellSlice> destination_cs, UniValue &uvObj) {
  UniValue uvState;
  uvState.setObject();
  StateInit dataStateInit;
  TransferData transferData;

  buildContractDataStruct(cs, dataStateInit);
  getContractData(dataStateInit, destination_cs, uvState, transferData);
  serializeStateInit(dataStateInit, uvState);
  uvObj.pushKV("contract_state", uvState);
  return uOK;
}

UniValue HttpAnswer::serializeBlockHeaderCellLite(BlockHeaderCell head_c) {
  UniValue uvObj;
  uvObj.setObject();
  vm::CellSlice cs{vm::NoVm(), head_c.root};
  ton::BlockIdExt block_id = head_c.block_id;
  try {
    td::Ref<vm::Cell> virt_root = vm::MerkleProof::virtualize(head_c.root, 1);
    if (virt_root.is_null()) {
      abort("invalid merkle proof");
    }
    ton::RootHash vhash{virt_root->get_hash().bits()};
    std::vector<ton::BlockIdExt> prev;
    ton::BlockIdExt mc_blkid;
    bool after_split;
    td::Status res = block::unpack_block_prev_blk_ext(virt_root, block_id, prev, mc_blkid, after_split);
    if (res.is_error()) {
      abort(PSTRING() << "cannot unpack header for block " << block_id.to_str() << ": " << res);
    }
    block::gen::Block::Record blk;
    block::gen::BlockInfo::Record info;
    // block::gen::ValueFlow::Record flow;
    if (!(tlb::unpack_cell(virt_root, blk) && tlb::unpack_cell(blk.info, info))) {
      abort(PSTRING() << "cannot unpack header for block " << block_id.to_str());
    }
    bool before_split = info.before_split;

    uvObj.pushKV("block", serializeBlockId(block_id.id));
    uvObj.pushKV("roothash", block_id.root_hash.to_hex());
    uvObj.pushKV("filehash", block_id.file_hash.to_hex());
    uvObj.pushKV("time", (uint64_t)info.gen_utime);

    UniValue uvArrPrevs;
    uvArrPrevs.setArray();
    for (auto prv : prev) {
      UniValue uvObjPrev;
      uvObjPrev.setObject();
      uvObjPrev.pushKV("workchain", prv.id.workchain);
      uvObjPrev.pushKV("shard", ton::shard_to_str(prv.id.shard));
      uvObjPrev.pushKV("seqno", (uint64_t)prv.id.seqno);
      uvObjPrev.pushKV("roothash", prv.root_hash.to_hex());
      uvObjPrev.pushKV("filehas", prv.file_hash.to_hex());
      uvArrPrevs.push_back(uvObjPrev);
    }
    uvObj.pushKV("previous", uvArrPrevs);

    if (!before_split) {
      uvObj.pushKV("next_block",
                   serializeBlockId(ton::BlockId{block_id.id.workchain, block_id.id.shard, block_id.id.seqno + 1}));
    } else {
      uvObj.pushKV("next_block_left",
                   serializeBlockId(ton::BlockId{block_id.id.workchain, ton::shard_child(block_id.id.shard, true),
                                                 block_id.id.seqno + 1}));
      uvObj.pushKV("next_block",
                   serializeBlockId(ton::BlockId{block_id.id.workchain, ton::shard_child(block_id.id.shard, false),
                                                 block_id.id.seqno + 1}));
    }

  } catch (vm::VmError err) {
    abort(PSTRING() << "error processing header : " << err.get_msg());
  } catch (vm::VmVirtError err) {
    abort(PSTRING() << "error processing header : " << err.get_msg());
  }
  return uvObj;
}

void HttpAnswer::serializeObject(BlockHeaderCell head_c) {
  UniValue uvObj;
  uvObj.setObject();
  vm::CellSlice cs{vm::NoVm(), head_c.root};
  auto block_id = head_c.block_id;
  try {
    auto virt_root = vm::MerkleProof::virtualize(head_c.root, 1);
    if (virt_root.is_null()) {
      abort("invalid merkle proof");
      // return *this;
    }
    ton::RootHash vhash{virt_root->get_hash().bits()};
    std::vector<ton::BlockIdExt> prev;
    ton::BlockIdExt mc_blkid;
    bool after_split;
    auto res = block::unpack_block_prev_blk_ext(virt_root, block_id, prev, mc_blkid, after_split);
    if (res.is_error()) {
      abort(PSTRING() << "cannot unpack header for block " << block_id.to_str() << ": " << res);
      // return *this;
    }
    block::gen::Block::Record blk;
    block::gen::BlockInfo::Record info;
    // block::gen::ValueFlow::Record flow;
    if (!(tlb::unpack_cell(virt_root, blk) && tlb::unpack_cell(blk.info, info))) {
      abort(PSTRING() << "cannot unpack header for block " << block_id.to_str());
      // return *this;
    }
    bool before_split = info.before_split;

    uvObj.pushKV("block", serializeBlockId(block_id.id));
    uvObj.pushKV("roothash", block_id.root_hash.to_hex());
    uvObj.pushKV("filehash", block_id.file_hash.to_hex());
    uvObj.pushKV("time", (uint64_t)info.gen_utime);
    uvObj.pushKV("lt_start", (uint64_t)info.start_lt);
    uvObj.pushKV("lt_end", (uint64_t)info.end_lt);

    uvObj.pushKV("global_id", blk.global_id);
    uvObj.pushKV("version", (uint64_t)info.version);
    uvObj.pushKV("flags", info.flags);
    uvObj.pushKV("key_block", info.key_block);
    uvObj.pushKV("not_master", info.not_master);

    uvObj.pushKV("after_merge", info.after_merge);
    uvObj.pushKV("after_split", info.after_split);
    uvObj.pushKV("before_split", info.before_split);
    uvObj.pushKV("want_merge", info.want_merge);
    uvObj.pushKV("want_split", info.want_split);
    uvObj.pushKV("validator_list_hash_short", (uint64_t)info.gen_validator_list_hash_short);

    uvObj.pushKV("catchain_seqno", (uint64_t)info.gen_catchain_seqno);
    uvObj.pushKV("min_ref_mc_seqno", (uint64_t)info.min_ref_mc_seqno);
    uvObj.pushKV("vert_seqno", info.vert_seq_no);
    uvObj.pushKV("vert_seqno_incr", info.vert_seqno_incr);
    uvObj.pushKV("prev_key_block_seqno",
                 serializeBlockId(ton::BlockId{ton::masterchainId, ton::shardIdAll, info.prev_key_block_seqno}));

    UniValue uvArrPrevs;
    uvArrPrevs.setArray();
    for (auto prv : prev) {
      UniValue uvObjPrev;
      uvObjPrev.setObject();
      uvObjPrev.pushKV("workchain", prv.id.workchain);
      uvObjPrev.pushKV("shard", ton::shard_to_str(prv.id.shard));
      uvObjPrev.pushKV("seqno", (uint64_t)prv.id.seqno);
      uvObjPrev.pushKV("roothash", prv.root_hash.to_hex());
      uvObjPrev.pushKV("filehas", prv.file_hash.to_hex());
      uvArrPrevs.push_back(uvObjPrev);
    }
    uvObj.pushKV("previous", uvArrPrevs);

    if (!before_split) {
      uvObj.pushKV("next_block",
                   serializeBlockId(ton::BlockId{block_id.id.workchain, block_id.id.shard, block_id.id.seqno + 1}));
    } else {
      uvObj.pushKV("next_block_left",
                   serializeBlockId(ton::BlockId{block_id.id.workchain, ton::shard_child(block_id.id.shard, true),
                                                 block_id.id.seqno + 1}));
      uvObj.pushKV("next_block",
                   serializeBlockId(ton::BlockId{block_id.id.workchain, ton::shard_child(block_id.id.shard, false),
                                                 block_id.id.seqno + 1}));
    }
    uvObj.pushKV("masterchain_block", serializeBlockIdExt(mc_blkid));

  } catch (vm::VmError err) {
    abort(PSTRING() << "error processing header : " << err.get_msg());
  } catch (vm::VmVirtError err) {
    abort(PSTRING() << "error processing header : " << err.get_msg());
  }

  jsonObject.pushKV("header", uvObj);
}

void HttpAnswer::serializeObject(BlockShardsCellSmall shards_c) {
  UniValue uvObjMaster;
  uvObjMaster.setObject();
  UniValue uvObj;
  uvObj.setObject();
  uvObj.pushKV("seqno", (uint64_t)block_id_.id.seqno);
  uvObj.pushKV("roothash", block_id_.root_hash.to_hex());
  uvObj.pushKV("filehash", block_id_.file_hash.to_hex());
  uvObjMaster.pushKV(ton::shard_to_str(block_id_.id.shard), uvObj);
  jsonObject.pushKV(std::to_string(block_id_.id.workchain), uvObjMaster);

  block::ShardConfig sh_conf;

  if (!sh_conf.unpack(vm::load_cell_slice_ref(shards_c.root))) {
    abort("cannot extract shard block list from shard configuration");
    return;
  } else {
    auto ids = sh_conf.get_shard_hash_ids(true);

    auto workchain = ton::masterchainId;
    UniValue uvObj;  
    uvObj.setObject();
    bool flag = false;
    for (auto id : ids) {
      auto ref = sh_conf.get_shard_hash(ton::ShardIdFull(id));
      if (id.workchain != workchain && flag) {
        workchain = id.workchain;
        jsonObject.pushKV(std::to_string(id.workchain), uvObj);
        uvObj.clear();
        uvObj.setObject();
        flag = false;
      }
      if (id.workchain != workchain) {
        workchain = id.workchain;
      }
      flag = true;

      if (ref.not_null()) {
        UniValue uvObjShard; 
        uvObjShard.setObject();
        uvObjShard.pushKV("seqno", (uint64_t)ref->top_block_id().id.seqno);
        uvObjShard.pushKV("roothash", ref->top_block_id().root_hash.to_hex());
        uvObjShard.pushKV("filehash", ref->top_block_id().file_hash.to_hex());
        uvObj.pushKV(ton::shard_to_str(id.shard), uvObjShard);

      } else {
        uvObj.pushKV("workchain", id.workchain);
        uvObj.pushKV("shard", ton::shard_to_str(id.shard));
      }
    }
    jsonObject.pushKV(std::to_string(workchain), uvObj);
  }
}

UniValue HttpAnswer::serializeBlockShardsCellLite(BlockShardsCell shards_c) {
  UniValue uvArr;
  uvArr.setArray();
  block::ShardConfig sh_conf;
  if (!sh_conf.unpack(vm::load_cell_slice_ref(shards_c.root))) {
    abort("cannot extract shard block list from shard configuration");
    uvArr.clear();  
    return uvArr;
  } else {
    std::vector<ton::BlockId> ids = sh_conf.get_shard_hash_ids(true);

    ton::WorkchainId workchain = ton::masterchainId;
    for (auto id : ids) {
      td::Ref<block::McShardHash> ref = sh_conf.get_shard_hash(ton::ShardIdFull(id));
      UniValue uvObj;  
      uvObj.setObject();
      if (id.workchain != workchain) {
        workchain = id.workchain;
      }
      if (ref.not_null()) {
        uvObj.pushKV("workchain", id.workchain);
        uvObj.pushKV("shard", ton::shard_to_str(id.shard));
        uvObj.pushKV("seqno", std::to_string(ref->top_block_id().id.seqno));
        uvObj.pushKV("created", (uint64_t)ref->created_at());
        uvObj.pushKV("beforesplit", ref->before_split_);
        uvObj.pushKV("beforemerge", ref->before_merge_);

      } else {
        uvObj.pushKV("workchain", id.workchain);
        uvObj.pushKV("shard", ton::shard_to_str(id.shard));
      }
      uvArr.push_back(uvObj);
    }
    return uvArr;
  }
}

void HttpAnswer::serializeObject(BlockShardsCell shards_c) {
  UniValue uvArr;  
  uvArr.setArray();
  block::ShardConfig sh_conf;
  if (!sh_conf.unpack(vm::load_cell_slice_ref(shards_c.root))) {
    abort("cannot extract shard block list from shard configuration");
    return;
  } else {
    auto ids = sh_conf.get_shard_hash_ids(true);
    auto workchain = ton::masterchainId;
    for (auto id : ids) {
      auto ref = sh_conf.get_shard_hash(ton::ShardIdFull(id));
      UniValue uvObj; 
      uvObj.setObject();
      if (id.workchain != workchain) {
        workchain = id.workchain;
      }
      UniValue uvShard;
      uvShard.setObject();
      uvShard.pushKV("workchain", id.workchain);
      uvShard.pushKV("shard", ton::shard_to_str(id.shard));
      if (ref.not_null()) {
        uvObj.pushKV("shard", uvShard);
        uvObj.pushKV("seqno", std::to_string(ref->top_block_id().id.seqno));
        uvObj.pushKV("created", (uint64_t)ref->created_at());
        uvObj.pushKV("wantsplit", ref->want_split_);
        uvObj.pushKV("wantmerge", ref->want_merge_);
        uvObj.pushKV("beforesplit", ref->before_split_);
        uvObj.pushKV("beforemerge", ref->before_merge_);
      } else {
        uvObj.pushKV("shard", uvShard);
      }
      uvArr.push_back(uvObj);
    }
    jsonObject.pushKV("shards", uvArr);
  }
}

UniValue HttpAnswer::serializeObject(AccountLink account) {
  UniValue uvObj;
  uvObj.setObject();
  if (account.block_id.is_valid()) {
    uvObj.pushKV("block", block_id_link(account.block_id));
  }
  uvObj.pushKV("account", account.account_id.rserialize(true));
  return uvObj;
}

UniValue HttpAnswer::serializeObject(MessageLink msg) {
  UniValue uvObj;
  uvObj.setObject();
  uvObj.pushKV("msg", msg.root->get_hash().to_hex());
  return uvObj;
}

UniValue HttpAnswer::serializeTransaction(TransactionLink trans) {
  UniValue uvObj;
  std::stringstream os;
  os << trans.hash;  
  uvObj.setObject();
  uvObj.pushKV("account", trans.account_id.rserialize(true));
  uvObj.pushKV("lt", trans.lt);
  uvObj.pushKV("hash", trans.hash.to_hex());
  return uvObj;
}

UniValue HttpAnswer::serializeObject(BlockLink block) {
  return block_id_link(block.block_id);
}

void HttpAnswer::unpackBlockVectorForOneChain(td::vector<ton::BlockIdExt> block_id) {
  for (auto block : block_id) {
    UniValue uvObj;
    uvObj.setObject();
    std::stringstream filehash;
    filehash << block.file_hash;
    std::stringstream roothash;
    roothash << block.root_hash;
    uvObj.pushKV("seqno", (uint64_t)block.id.seqno);
    uvObj.pushKV("roothash", roothash.str());
    uvObj.pushKV("filehash", filehash.str());
    jsonObject.pushKV(ton::shard_to_str(block.id.shard), uvObj);
  }
}

void HttpAnswer::serializeObject(TransactionList trans) {
  UniValue uvArr;
  uvArr.setArray();
  td::uint32 idx = 0;
  for (auto &x : trans.vec) {
    UniValue uvObj;
    uvObj.setObject();
    uvObj.pushKV("seq", (uint64_t)++idx);
    uvObj.pushKV("account", x.addr.rserialize(true));
    uvObj.pushKV("lt", x.lt);
    uvObj.pushKV("hash", x.hash.to_hex());
    uvArr.push_back(uvObj);
  }
  jsonObject.pushKV("transactions", uvArr);
}

void HttpAnswer::serializeObject(ConfigParam conf) {
  if (conf.idx >= 0) {
    jsonObject.pushKV(std::to_string(conf.idx), serializeObject(RawData<block::gen::ConfigParam>{conf.root, conf.idx}));
  } else {
    jsonObject.pushKV(std::to_string(conf.idx), serializeObject(RawData<void>{conf.root}));
  }
}

unpackRC HttpAnswer::unpackValidators(vm::CellSlice cs, UniValue &uvObj) {
  block::gen::ValidatorSet::Record_validators validatorsRecord;
  if (tlb::unpack(cs, validatorsRecord)) {
    uvObj.pushKV("utime_since", (int64_t)validatorsRecord.utime_since);
    uvObj.pushKV("utime_until", (int64_t)validatorsRecord.utime_until);
    uvObj.pushKV("total", (int64_t)validatorsRecord.total);
    uvObj.pushKV("main", (int64_t)validatorsRecord.main);
    return uOK;
  }
  return uERR;
}

unpackRC HttpAnswer::unpackValidatorRecord(vm::CellSlice cs, UniValue &uvObj) {
  block::gen::ValidatorDescr::Record_validator validatorsRecord;
  if (tlb::unpack(cs, validatorsRecord)) {
    uvObj.pushKV("weight", (int64_t)validatorsRecord.weight);
    return uOK;
  }
  return uERR;
}

unpackRC HttpAnswer::unpackSigPubKey(vm::CellSlice cs, UniValue &uvObj) {
  block::gen::SigPubKey::Record sigPubKeyRec;
  if (tlb::unpack(cs, sigPubKeyRec)) {
    uvObj.pushKV("pubkey", sigPubKeyRec.pubkey.to_hex());
    return uOK;
  }
  return uERR;
}

unpackRC HttpAnswer::unpackValidatorRecordAddr(vm::CellSlice cs, UniValue &uvObj) {
  block::gen::ValidatorDescr::Record_validator_addr validatorsRecord;
  if (tlb::unpack(cs, validatorsRecord)) {
    uvObj.pushKV("weight", (int64_t)validatorsRecord.weight);
    uvObj.pushKV(
        "adnl_addr",
        block::StdAddress(block_id_.id.workchain, validatorsRecord.adnl_addr.cbits(), true, false).rserialize(true));
    switch (unpackSigPubKey(validatorsRecord.public_key.write(), uvObj)) {
      case uOK:
        break;
      case uNULL_ptr:
      case uERR:
      default:
        uvObj.pushKV("error", "can't unpack");
    }
    return uOK;
  }
  return uERR;
}

unpackRC HttpAnswer::unpackValidatorDescr(vm::CellSlice cs, UniValue &uvObj) {
  block::gen::ValidatorDescr validatorDescr;
  switch (validatorDescr.get_tag(cs)) {
    case block::gen::ValidatorDescr::validator:
      unpackValidatorRecord(cs, uvObj);
      break;
    case block::gen::ValidatorDescr::validator_addr:
      unpackValidatorRecordAddr(cs, uvObj);
      break;
    default:
      abort("Can't unpack validtors set");
      break;
  }
  return uOK;
}

unpackRC HttpAnswer::unpackValidatorsExt(vm::CellSlice cs, UniValue &uvObj) {
  block::gen::ValidatorSet::Record_validators_ext validatorsRecord;
  if (tlb::unpack(cs, validatorsRecord)) {
    uvObj.pushKV("utime_since", (int64_t)validatorsRecord.utime_since);
    uvObj.pushKV("utime_until", (int64_t)validatorsRecord.utime_until);
    uvObj.pushKV("total", (int64_t)validatorsRecord.total);
    uvObj.pushKV("main", (int64_t)validatorsRecord.main);
    uvObj.pushKV("total_weight", (int64_t)validatorsRecord.total_weight);
    td::BitArray<16> key_buffer;
    auto dict_root = validatorsRecord.list->prefetch_ref();
    vm::Dictionary dict{std::move(dict_root), 16};
    UniValue uvArr;
    uvArr.setArray();
    for (int x = 0; x < validatorsRecord.total; x++) {
      UniValue uvValDescr;
      uvValDescr.setObject();
      key_buffer.store_ulong(x);
      auto validatorDescr = dict.lookup(key_buffer.bits(), 16);
      unpackValidatorDescr(*validatorDescr, uvValDescr);
      uvArr.push_back(uvValDescr);
    }
    uvObj.pushKV("list", uvArr);
    return uOK;
  }
  return uERR;
}

UniValue HttpAnswer::serializeObject(ValidatorSet conf) {
  UniValue uvObj;
  uvObj.setObject();
  block::gen::ValidatorSet validatorSet;
  vm::CellSlice cs{vm::NoVm(), conf.root};
  switch (validatorSet.get_tag(cs)) {
    case block::gen::ValidatorSet::validators:
      unpackValidators(cs, uvObj);
      break;
    case block::gen::ValidatorSet::validators_ext:
      unpackValidatorsExt(cs, uvObj);
      break;
    default:
      abort("Can't unpack validtors set");
      break;
  }
  return uvObj;
}

void HttpAnswer::unpackValidatorsTime(ValidatorSet conf, std::int64_t &from, std::int64_t &to) {
  UniValue uvObj;
  uvObj.setObject();
  block::gen::ValidatorSet validatorSet;
  vm::CellSlice cs{vm::NoVm(), conf.root};
  switch (validatorSet.get_tag(cs)) {
    case block::gen::ValidatorSet::validators:
      unpackValidators(cs, uvObj);
      break;
    case block::gen::ValidatorSet::validators_ext:
      unpackValidatorsExt(cs, uvObj);
      break;
    default:
      abort("Can't unpack validtors set");
      break;
  }
  from = (uvObj["utime_since"]).get_int64();
  to = (uvObj["utime_until"]).get_int64();
}

void HttpAnswer::serializeObject(Error error) {
  jsonObject.pushKV("error", error.error.to_string());
}

UniValue HttpAnswer::block_id_link(ton::BlockIdExt block_id) {
  UniValue uvObj;
  std::stringstream filehash;
  filehash << block_id.file_hash;
  std::stringstream roothash;
  roothash << block_id.root_hash;
  uvObj.setObject();
  uvObj.pushKV("workchain", block_id.id.workchain);
  uvObj.pushKV("shard", ton::shard_to_str(block_id.id.shard));
  uvObj.pushKV("seqno", (uint64_t)block_id.id.seqno);
  uvObj.pushKV("roothash", roothash.str());
  uvObj.pushKV("filehash", filehash.str());
  return uvObj;
}

void HttpAnswer::abort(td::Status error) {
  if (error_.is_ok()) {
    error_ = std::move(error);
  }
  jsonObject.clear();
  jsonObject.setObject();
  jsonObject.pushKV("error", error_.to_string());
}

void HttpAnswer::abort(std::string error) {
  abort(td::Status::Error(404, error));
  return;
}

std::string HttpAnswer::finish() {
  return jsonObject.write();
}


bool HttpAnswer::serializeBlockData(td::Ref<vm::Cell> block_root_, ton::BlockIdExt block_id) {
  UniValue uvObj;
  uvObj.setObject();
  try {
    std::vector<ton::BlockIdExt> prev;
    ton::BlockIdExt mc_blkid;
    bool after_split;
    auto res = block::unpack_block_prev_blk_ext(block_root_, block_id, prev, mc_blkid, after_split);
    if (res.is_error()) {
      abort(PSTRING() << "cannot unpack header for block " << block_id.to_str() << ": " << res);
    }

    block::gen::Block::Record blk;
    block::gen::BlockInfo::Record info;
    block::gen::BlockExtra::Record extra;
    if (!(tlb::unpack_cell(block_root_, blk) && tlb::unpack_cell(blk.extra, extra) &&
          tlb::unpack_cell(blk.info, info))) {
      return false;  //("cannot unpack Block header");
    }
    td::Ref<vm::Cell> value_flow_root = blk.value_flow;
    vm::CellSlice cs{vm::NoVmOrd(), value_flow_root};
    block::ValueFlow value_flow_;

    bool before_split = info.before_split;

    uvObj.pushKV("block", serializeBlockId(block_id.id));
    uvObj.pushKV("roothash", block_id.root_hash.to_hex());
    uvObj.pushKV("filehash", block_id.file_hash.to_hex());
    uvObj.pushKV("time", (uint64_t)info.gen_utime);
    uvObj.pushKV("lt_start", (uint64_t)info.start_lt);
    uvObj.pushKV("lt_end", (uint64_t)info.end_lt);

    uvObj.pushKV("global_id", blk.global_id);
    uvObj.pushKV("version", (uint64_t)info.version);
    uvObj.pushKV("flags", info.flags);
    uvObj.pushKV("key_block", info.key_block);
    uvObj.pushKV("not_master", info.not_master);

    uvObj.pushKV("after_merge", info.after_merge);
    uvObj.pushKV("after_split", info.after_split);
    uvObj.pushKV("before_split", info.before_split);
    uvObj.pushKV("want_merge", info.want_merge);
    uvObj.pushKV("want_split", info.want_split);
    uvObj.pushKV("validator_list_hash_short", (uint64_t)info.gen_validator_list_hash_short);

    uvObj.pushKV("catchain_seqno", (uint64_t)info.gen_catchain_seqno);
    uvObj.pushKV("min_ref_mc_seqno", (uint64_t)info.min_ref_mc_seqno);
    uvObj.pushKV("vert_seqno", info.vert_seq_no);
    uvObj.pushKV("vert_seqno_incr", info.vert_seqno_incr);
    uvObj.pushKV("prev_key_block_seqno",
                 serializeBlockId(ton::BlockId{ton::masterchainId, ton::shardIdAll, info.prev_key_block_seqno}));

    UniValue uvArrPrevs;
    uvArrPrevs.setArray();
    for (auto prv : prev) {
      UniValue uvObjPrev;
      uvObjPrev.setObject();
      uvObjPrev.pushKV("workchain", prv.id.workchain);
      uvObjPrev.pushKV("shard", ton::shard_to_str(prv.id.shard));
      uvObjPrev.pushKV("seqno", (uint64_t)prv.id.seqno);
      uvObjPrev.pushKV("roothash", prv.root_hash.to_hex());
      uvObjPrev.pushKV("filehas", prv.file_hash.to_hex());
      uvArrPrevs.push_back(uvObjPrev);
    }
    uvObj.pushKV("previous", uvArrPrevs);

    if (!before_split) {
      uvObj.pushKV("next_block",
                   serializeBlockId(ton::BlockId{block_id.id.workchain, block_id.id.shard, block_id.id.seqno + 1}));
    } else {
      uvObj.pushKV("next_block_left",
                   serializeBlockId(ton::BlockId{block_id.id.workchain, ton::shard_child(block_id.id.shard, true),
                                                 block_id.id.seqno + 1}));
      uvObj.pushKV("next_block",
                   serializeBlockId(ton::BlockId{block_id.id.workchain, ton::shard_child(block_id.id.shard, false),
                                                 block_id.id.seqno + 1}));
    }
    uvObj.pushKV("masterchain_block", serializeBlockIdExt(mc_blkid));

    jsonObject.pushKV("header", uvObj);

    uvObj.clear();

    if (!(cs.is_valid() && value_flow_.fetch(cs) && cs.empty_ext())) {
      return false;
    }
    std::ostringstream os;
    value_flow_.show(os);
    if (!value_flow_.validate()) {
      return false;
    }
    uvObj.setObject();
    std::string out = "";

#define unpacCurr(name, in)                     \
  switch (unpack_CurrencyCollection(in, out)) { \
    case uOK:                                   \
      uvObj.pushKV(name, out);                  \
      break;                                    \
    case uNULL_ptr:                             \
    case uERR:                                  \
    default:                                    \
      uvObj.pushKV(name, UniValue::VNULL);      \
      break;                                    \
  }

    unpacCurr("from_prev_blk", value_flow_.from_prev_blk);
    unpacCurr("to_next_blk", value_flow_.to_next_blk);
    unpacCurr("imported", value_flow_.imported);
    unpacCurr("exported", value_flow_.exported);
    unpacCurr("fees_collected", value_flow_.fees_collected);
    unpacCurr("fees_imported", value_flow_.fees_imported);
    unpacCurr("recovered", value_flow_.recovered);
    unpacCurr("created", value_flow_.created);
    unpacCurr("minted", value_flow_.minted);

#undef unpacCurr
    jsonObject.pushKV("value_flow", uvObj);

  } catch (vm::VmError err) {
    abort(PSTRING() << "error processing header : " << err.get_msg());
  } catch (vm::VmVirtError err) {
    abort(PSTRING() << "error processing header : " << err.get_msg());
  }

  return true;
}

unpackRC HttpAnswer::unpack_CurrencyCollection(block::CurrencyCollection collection, std::string &grams) {
  if (!collection.is_valid()) {
    return uERR;
  }
  grams = collection.grams->to_dec_string();
  return uOK;
}

unpackRC HttpAnswer::unpack_CurrencyCollection(block::CurrencyCollection collection, std::string &grams,
                                               td::BigInt256 &fees) {
  if (!collection.is_valid()) {
    return uERR;
  }
  grams = collection.grams->to_dec_string();
  fees = *(collection.grams);
  return uOK;
}
