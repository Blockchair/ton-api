#pragma once

#include <ton/ton-types.h>
#include "vm/boc.h"
#include "vm/cellops.h"
#include "td/utils/Random.h"
#include "td/utils/base64.h"
#include "td/utils/crypto.h"
#include "block/block.h"
#include "univalue.h"
#include "block/block-auto.h"

#include "crypto/block/check-proof.h"
#include "crypto/vm/utils.h"

#include "vm/boc.h"
#include "vm/cellops.h"
#include "vm/cells/MerkleProof.h"
#include "vm/vm.h"
#include "vm/cp0.h"

extern bool local_scripts;

enum unpackRC {
  uOK = 0,
  uNULL_ptr,

  uERR = 255,
};

class HttpAnswer {
 public:
  struct MessageCell {
    td::Ref<vm::Cell> root;
  };
  struct AddressCell {
    td::Ref<vm::CellSlice> root;
  };
  struct TransactionCell {
    block::StdAddress addr;
    ton::BlockIdExt block_id;
    td::Ref<vm::Cell> root;
  };
  struct TransactionSmall {
    ton::BlockIdExt block_id;
    td::Ref<vm::Cell> root;
  };
  struct Stack {
    td::Ref<vm::Stack> stack;
  };

  struct AccountCell {
    block::StdAddress addr;
    ton::BlockIdExt block_id;
    td::Ref<vm::Cell> root;
    std::vector<td::Ref<vm::Cell>> q_roots;
  };
  struct BlockHeaderCell {
    ton::BlockIdExt block_id;
    td::Ref<vm::Cell> root;
  };
  struct BlockShardsCell {
    ton::BlockIdExt block_id;
    td::Ref<vm::Cell> root;
  };
    struct BlockShardsCellSmall {
    ton::BlockIdExt block_id;
    td::Ref<vm::Cell> root;
  };

  struct AccountLink {
    block::StdAddress account_id;
    ton::BlockIdExt block_id;
  };
  struct MessageLink {
    td::Ref<vm::Cell> root;
  };
  struct TransactionLink {
    block::StdAddress account_id;
    ton::LogicalTime lt;
    ton::Bits256 hash;
  };
  struct TransactionLinkShort {
    ton::BlockIdExt block_id;
    block::StdAddress account_id;
    ton::LogicalTime lt;
  };
  struct BlockLink {
    ton::BlockIdExt block_id;
  };
  struct BlockViewLink {
    ton::BlockIdExt block_id;
  };
  struct ConfigViewLink {
    ton::BlockIdExt block_id;
  };
  struct BlockDownloadLink {
    ton::BlockIdExt block_id;
  };
  struct BlockSearch {
    ton::BlockIdExt block_id;
  };
  struct AccountSearch {
    ton::BlockIdExt block_id;
    block::StdAddress addr;
  };
  struct TransactionSearch {
    ton::BlockIdExt block_id;
    block::StdAddress addr;
    ton::LogicalTime lt;
    ton::Bits256 hash;
  };

   typedef struct TransactionDescr {
    TransactionDescr(block::StdAddress addr, ton::LogicalTime lt, ton::Bits256 hash) : addr(addr), lt(lt), hash(hash) {
    }
    TransactionDescr() {
    }
    TransactionDescr(const TransactionDescr& transDescr) : addr(transDescr.addr), lt(transDescr.lt), hash(transDescr.hash) {
      
    }
    ~TransactionDescr() {
    }
    block::StdAddress addr;
    ton::LogicalTime lt;
    ton::Bits256 hash;
  } TransactionDescr;

  struct TransactionList {
    ton::BlockIdExt block_id;
    std::vector<TransactionDescr> vec;
    td::uint32 req_count_;
  };
  struct CodeBlock {
    std::string data;
  };
  struct ConfigParam {
    td::int32 idx;
    td::Ref<vm::Cell> root;
  };
  struct ValidatorSet {
    td::int32 idx;
    td::Ref<vm::Cell> root;
  };
  struct Error {
    td::Status error;
  };

  template <class T>

  struct RawData {
    td::Ref<vm::Cell> root;
    T x;
    template <typename... Args>
    RawData(td::Ref<vm::Cell> root, Args &&... args) : root(std::move(root)), x(std::forward<Args>(args)...) {
    }
  };

 public:
  HttpAnswer(std::string title, std::string prefix) : title_(title), prefix_(prefix) {
    buf_ = td::BufferSlice{1 << 28};
    sb_ = std::make_unique<td::StringBuilder>(buf_.as_slice());
    jsonObject.setObject();
  }

  HttpAnswer(std::string title, std::string prefix, bool code) : title_(title), code_(code), prefix_(prefix) {
    buf_ = td::BufferSlice{1 << 28};
    sb_ = std::make_unique<td::StringBuilder>(buf_.as_slice());
    jsonObject.setObject();
  }

  void set_title(std::string title) {
    title_ = title;
  }
  void set_block_id(ton::BlockIdExt block_id) {
    block_id_ = block_id;
    workchain_id_ = block_id_.id.workchain;
  }
  void set_account_id(block::StdAddress addr) {
    account_id_ = addr;
  }
  void set_workchain(ton::WorkchainId workchain_id) {
    workchain_id_ = workchain_id;
  }

  void abort(td::Status error);
  void abort(std::string error);


  std::string finish();   // return json in txt(json) format
  std::string footer();

 template <typename T>
  void serializeObject(T x) {
    jsonObject.pushKV("answer", x);
  }

  td::StringBuilder &sb() {
    return *sb_;
  }

  void serializeObject(block::StdAddress addr, ton::LogicalTime lt, ton::Bits256 hash, UniValue &uvObj);
  
  UniValue serializeObject(Stack stack);
  UniValue serializeStackEntry(vm::StackEntry se);
  UniValue serializeCellSlice(vm::CellSlice cs);

  bool checkNeededType(vm::StackEntry se, std::string expectedType);

  UniValue serializeObject(AddressCell addr_c);
  unpackRC serializeObject(AddressCell addr_c, std::string &address);

  // UniValue serializeObject(MessageCell msg, int messageType);

  inline UniValue serializeBlockIdExt(ton::BlockIdExt block_id);
  void unpackBlockVectorForOneChain(td::vector<ton::BlockIdExt> block_id);

  UniValue serializeBlockId(ton::BlockId block_id);

  void serializeObject(TransactionCell trans_c);
  void serializeObject(TransactionSmall trans_c, UniValue &uvObj,
                       std::shared_ptr<HttpAnswer::TransactionDescr> transDescr = nullptr);
  TransactionDescr serializeTransactionSmall(TransactionSmall trans_c, UniValue &uvObj);

  void serializeObject(AccountCell acc_c);

  void serializeObject(BlockHeaderCell head_c);
  UniValue serializeBlockHeaderCellLite(BlockHeaderCell head_c);

  void serializeObject(BlockShardsCell shards_c);
  UniValue serializeBlockShardsCellLite(BlockShardsCell shards_c);
  void serializeObject(BlockShardsCellSmall shards_c);

  UniValue serializeObject(AccountLink account);

  UniValue serializeObject(MessageLink msg);

  UniValue serializeTransaction(TransactionLink trans);
  // void serializeObject(TransactionLink trans);

  UniValue serializeObject(BlockLink block);

  void serializeObject(Error error);

  void serializeObject(TransactionList trans);

  UniValue serializeObject(ValidatorSet conf);
  void unpackValidatorsTime(ValidatorSet conf, std::int64_t &from, std::int64_t &to);

  unpackRC unpackValidators(vm::CellSlice cs, UniValue &uvObj);
  unpackRC unpackValidatorsExt(vm::CellSlice cs, UniValue &uvObj);
  unpackRC unpackValidatorDescr(vm::CellSlice cs, UniValue &uvObj);
  unpackRC unpackValidatorRecord(vm::CellSlice cs, UniValue &uvObj);
  unpackRC unpackValidatorRecordAddr(vm::CellSlice cs, UniValue &uvObj);
  unpackRC unpackSigPubKey(vm::CellSlice cs, UniValue &uvObj);

  UniValue serializeObject(CodeBlock block) {
    UniValue uvObj;
    uvObj.setObject();
    if(code_) {
      uvObj.pushKV("code", block.data);
    }
    return uvObj;
  }

  void serializeObject(ConfigParam conf);

  template <class T>
  UniValue serializeObject(RawData<T> data) {
    std::ostringstream outp;
    data.x.print_ref(outp, data.root);
    auto dt = vm::load_cell_slice(data.root);
    return serializeObject(CodeBlock{outp.str()});
  }

  bool serializeBlockData(td::Ref<vm::Cell> block_root_, ton::BlockIdExt block_id);

  void putInJson(std::string key, UniValue uvObj) {
    jsonObject.pushKV(key, uvObj);
  }

  void uvObj2Json(UniValue uvObj) {
    jsonObject = uvObj;
  }

  void putInJsonArray(UniValue uvObj) {
    if (jsonObject.isArray()) {
      jsonObject.push_back(uvObj);
    }
  }

  void changeJsonObject2Array() {
    jsonObject.setArray();
  }

 private:
  enum TokenType {
    Jetton = 0,
    NFT = 1,
    Comment = 2,

    undef = 255,
  };

  public:
    static std::map<TokenType, std::string> tokenTypeString;

 private:

  enum TransferType {
    InterWallet = 0,
    TransferNotify = 1,
    OwnershipAssigned = 2,

    NoType = 255,
  };

  const std::map<TransferType, std::string> transferTypeString{
      {TransferType::InterWallet, "internal_transfer"},
      {TransferType::TransferNotify, "transfer_notification"},
      {TransferType::OwnershipAssigned, "ownership_assigned"}};

  ton::LogicalTime lt;
  ton::UnixTime now;

  std::string v1r1 =
      "B5EE9C72410101010044000084FF0020DDA4F260810200D71820D70B1FED44D0D31FD3FFD15112BAF2A122F901541044F910F2A2F80001D3"
      "1F3120D74A96D307D402FB00DED1A4C8CB1FCBFFC9ED5441FDF089";
  std::string v1r2 =
      "B5EE9C724101010100530000A2FF0020DD2082014C97BA9730ED44D0D70B1FE0A4F260810200D71820D70B1FED44D0D31FD3FFD15112BAF2"
      "A122F901541044F910F2A2F80001D31F3120D74A96D307D402FB00DED1A4C8CB1FCBFFC9ED54D0E2786F";
  std::string v1r3 =
      "B5EE9C7241010101005F0000BAFF0020DD2082014C97BA218201339CBAB19C71B0ED44D0D31FD70BFFE304E0A4F260810200D71820D70B1F"
      "ED44D0D31FD3FFD15112BAF2A122F901541044F910F2A2F80001D31F3120D74A96D307D402FB00DED1A4C8CB1FCBFFC9ED54B5B86E42";
  std::string v2r1 =
      "B5EE9C724101010100570000AAFF0020DD2082014C97BA9730ED44D0D70B1FE0A4F2608308D71820D31FD31F01F823BBF263ED44D0D31FD3"
      "FFD15131BAF2A103F901541042F910F2A2F800029320D74A96D307D402FB00E8D1A4C8CB1FCBFFC9ED54A1370BB6";
  std::string v2r2 =
      "B5EE9C724101010100630000C2FF0020DD2082014C97BA218201339CBAB19C71B0ED44D0D31FD70BFFE304E0A4F2608308D71820D31FD31F"
      "01F823BBF263ED44D0D31FD3FFD15131BAF2A103F901541042F910F2A2F800029320D74A96D307D402FB00E8D1A4C8CB1FCBFFC9ED54044C"
      "D7A1";
  std::string v3r1 =
      "B5EE9C724101010100620000C0FF0020DD2082014C97BA9730ED44D0D70B1FE0A4F2608308D71820D31FD31FD31FF82313BBF263ED44D0D3"
      "1FD31FD3FFD15132BAF2A15144BAF2A204F901541055F910F2A3F8009320D74A96D307D402FB00E8D101A4C8CB1FCB1FCBFFC9ED543FBE6E"
      "E0";
  std::string v3r2 =
      "B5EE9C724101010100710000DEFF0020DD2082014C97BA218201339CBAB19F71B0ED44D0D31FD31F31D70BFFE304E0A4F2608308D71820D3"
      "1FD31FD31FF82313BBF263ED44D0D31FD31FD3FFD15132BAF2A15144BAF2A204F901541055F910F2A3F8009320D74A96D307D402FB00E8D1"
      "01A4C8CB1FCB1FCBFFC9ED5410BD6DAD";

  std::string v4r2 =
      "B5EE9C72410214010002D4000114FF00F4A413F4BCF2C80B010201200203020148040504F8F28308D71820D31FD31FD31F02F823BBF264ED"
      "44D0D31FD31FD3FFF404D15143BAF2A15151BAF2A205F901541064F910F2A3F80024A4C8CB1F5240CB1F5230CBFF5210F400C9ED54F80F01"
      "D30721C0009F6C519320D74A96D307D402FB00E830E021C001E30021C002E30001C0039130E30D03A4C8CB1F12CB1FCBFF1011121302E6D0"
      "01D0D3032171B0925F04E022D749C120925F04E002D31F218210706C7567BD22821064737472BDB0925F05E003FA403020FA4401C8CA07CB"
      "FFC9D0ED44D0810140D721F404305C810108F40A6FA131B3925F07E005D33FC8258210706C7567BA923830E30D03821064737472BA925F06"
      "E30D06070201200809007801FA00F40430F8276F2230500AA121BEF2E0508210706C7567831EB17080185004CB0526CF1658FA0219F400CB"
      "6917CB1F5260CB3F20C98040FB0006008A5004810108F45930ED44D0810140D720C801CF16F400C9ED540172B08E23821064737472831EB1"
      "7080185005CB055003CF1623FA0213CB6ACB1FCB3FC98040FB00925F03E20201200A0B0059BD242B6F6A2684080A06B90FA0218470D40808"
      "47A4937D29910CE6903E9FF9837812801B7810148987159F31840201580C0D0011B8C97ED44D0D70B1F8003DB29DFB513420405035C87D01"
      "0C00B23281F2FFF274006040423D029BE84C600201200E0F0019ADCE76A26840206B90EB85FFC00019AF1DF6A26840106B90EB858FC0006E"
      "D207FA00D4D422F90005C8CA0715CBFFC9D077748018C8CB05CB0222CF165005FA0214CB6B12CCCCC973FB00C84014810108F451F2A70200"
      "70810108D718FA00D33FC8542047810108F451F2A782106E6F746570748018C8CB05CB025006CF165004FA0214CB6A12CB1FCB3FC973FB00"
      "02006C810108D718FA00D33F305224810108F459F2A782106473747270748018C8CB05CB025005CF165003FA0213CB6ACB1F12CB3FC973FB"
      "00000AF400C9ED54696225E5";

  std::string lock_up =
      "B5EE9C7241021E01000261000114FF00F4A413F4BCF2C80B010201200203020148040501F2F28308D71820D31FD31FD31F802403F823BB13"
      "F2F2F003802251A9BA1AF2F4802351B7BA1BF2F4801F0BF9015410C5F9101AF2F4F8005057F823F0065098F823F0062071289320D74A8E8B"
      "D30731D4511BDB3C12B001E8309229A0DF72FB02069320D74A96D307D402FB00E8D103A4476814154330F004ED541D0202CD060702012013"
      "1402012008090201200F100201200A0B002D5ED44D0D31FD31FD3FFD3FFF404FA00F404FA00F404D1803F7007434C0C05C6C2497C0F83E90"
      "0C0871C02497C0F80074C7C87040A497C1383C00D46D3C00608420BABE7114AC2F6C2497C338200A208420BABE7106EE86BCBD20084AE084"
      "0EE6B2802FBCBD01E0C235C62008087E4055040DBE4404BCBD34C7E00A60840DCEAA7D04EE84BCBD34C034C7CC0078C3C412040DD78CA00C"
      "0D0E00130875D27D2A1BE95B0C60000C1039480AF00500161037410AF0050810575056001010244300F004ED540201201112004548E1E228"
      "020F4966FA520933023BB9131E2209835FA00D113A14013926C21E2B3E6308003502323287C5F287C572FFC4F2FFFD00007E80BD00007E80"
      "BD00326000431448A814C4E0083D039BE865BE803444E800A44C38B21400FE809004E0083D10C06002012015160015BDE9F780188242F847"
      "800C02012017180201481B1C002DB5187E006D88868A82609E00C6207E00C63F04EDE20B30020158191A0017ADCE76A268699F98EB85FFC0"
      "0017AC78F6A268698F98EB858FC00011B325FB513435C2C7E00017B1D1BE08E0804230FB50F620002801D0D3030178B0925B7FE0FA4031FA"
      "403001F001A80EDAA4";

  UniValue jsonObject;
  td::BigInt256 fees = td::BigInt256(0);

  UniValue block_id_link(ton::BlockIdExt block_id);

  enum transactionTypes {
    trans_ord,
    trans_storage,
    trans_tick_tock,
    trans_split_prepare,
    trans_split_install,
    trans_merge_prepare,
    trans_merge_install
  };

  typedef struct StateInit {
    td::Ref<vm::Cell> code, data, library;
    std::string address;
    signed char split_depth;
    std::string walletType = "";
  } StateInit;

  typedef struct TransferData {
    td::RefInt256 query_id = td::RefInt256{};
    TokenType tokenType = TokenType::undef;
    std::string from = "";
    std::string to = "";
    td::RefInt256 amount = td::RefInt256{};
    std::string token = "";
    std::string transferType = "";
    std::string comment = "";
    bool isInit = false;

    UniValue serialize() {
      if(!isInit) {
        return UniValue::VNULL;
      }
      UniValue uvToken;
      uvToken.setObject();
      uvToken.pushKV("type", HttpAnswer::tokenTypeString.find(tokenType)->second);
      if (query_id.not_null()) {
        uvToken.pushKV("query_id", query_id.write().to_dec_string());
      } else {
        uvToken.pushKV("query_id", UniValue::VNULL);
      }
      uvToken.pushKV("transfer_type", transferType);
      uvToken.pushKV("from", from);
      uvToken.pushKV("to", to);
      if (amount.not_null()) {
        uvToken.pushKV("amount", amount.write().to_dec_string());
      } else {
        uvToken.pushKV("amount", UniValue::VNULL);
      }
      uvToken.pushKV("comment", comment);

      uvToken.pushKV("token", token);
      return uvToken;
    }

    void init() {
      isInit = true;
    }

  } TransferData;

  std::string transactionStatus(char status);
  UniValue MonteCarloDescription(vm::Ref<vm::Cell> description);

  unpackRC unpackVarUInteger16(vm::CellSlice varUInteger16, td::RefInt256 &out);
  unpackRC unpackVarUInteger7(vm::CellSlice varUInteger7, td::RefInt256 &out);
  unpackRC unpackVarUInteger3(vm::CellSlice varUInteger3, td::RefInt256 &out);
  unpackRC unpackMaybeVarUInteger3(vm::CellSlice varUInteger3, td::RefInt256 &out);

  unpackRC unpackStorageUsedShort(vm::CellSlice storageUsedSHort,  UniValue &uvObj);
  
  unpackRC unpackInt32(vm::CellSlice int32, td::RefInt256 &out);
  unpackRC unpackMaybeInt32(vm::CellSlice int32, td::RefInt256 &out);

  unpackRC unpackMaybeTrStoragePhase(vm::CellSlice storage_ph, UniValue &uvObj);
  unpackRC unpackTrStoragePhase(vm::CellSlice storage_ph, UniValue &uvObj);

  unpackRC unpackMaybeTrCreditPhase(vm::CellSlice credit_ph, UniValue &uvObj);
  unpackRC unpackTrCreditPhase(vm::CellSlice credit_ph, UniValue &uvObj);

  unpackRC unpackTrComputePhase(vm::CellSlice compute_ph, UniValue &uvObj);
  unpackRC MonteCarloTrComputePhase(vm::CellSlice compute_ph, UniValue &uvObj);

  unpackRC unpackTrActionPhase(vm::CellSlice action_ph, UniValue &uvObj);
  unpackRC unpackMaybeRefTrActionPhase(vm::CellSlice action_ph, UniValue &uvObj);

  unpackRC unpackSplitMergeInfo(vm::CellSlice split_info, UniValue &uvObj);

  bool unpackComputeSkipped(block::gen::TrComputePhase::Record_tr_phase_compute_skipped record_tr_phase_compute_skipped,
                            UniValue &uvObj);
  unpackRC unpackComputeVM(block::gen::TrComputePhase::Record_tr_phase_compute_vm record_tr_phase_compute_vm,
                       UniValue &uvObj);
  bool parseTrComputePhase_aux(block::gen::TrComputePhase_aux::Record record_TrComputePhase_aux, UniValue &uvObj);

  unpackRC unpackGrams(vm::CellSlice grams, block::CurrencyCollection &gramsCurColl);
  unpackRC unpackMaybeGrams(vm::CellSlice &grams, block::CurrencyCollection &gramsCurCol);

  bool unpackOrdinaryTrans(block::gen::TransactionDescr::Record_trans_ord description, UniValue &uvObj);
  bool unpackTickTockTrans(block::gen::TransactionDescr::Record_trans_tick_tock description, UniValue &uvObj);
  bool unpackMergePrepareTrans(block::gen::TransactionDescr::Record_trans_merge_prepare description, UniValue &uvObj);
  bool unpackMergeInstallTrans(block::gen::TransactionDescr::Record_trans_merge_install description, UniValue &uvObj);
  bool unpackSplitInstallTrans(block::gen::TransactionDescr::Record_trans_split_install description, UniValue &uvObj);
  bool unpackSplitPrepareTrans(block::gen::TransactionDescr::Record_trans_split_prepare description, UniValue &uvObj);
  bool unpackStorageTrans(block::gen::TransactionDescr::Record_trans_storage description, UniValue &uvObj);
  bool unpackTransaction(vm::CellSlice transaction, UniValue &uvObj);
  unpackRC unpackTransactionSmall(vm::CellSlice transaction, ton::BlockIdExt block_id, UniValue &uvObj,
                                  std::shared_ptr<HttpAnswer::TransactionDescr> transDescr = nullptr);

  bool checkTransactionType(vm::Ref<vm::Cell> description, transactionTypes type, std::string &currentType);
  std::string transactionType2String(transactionTypes type);

  unpackRC unpack_CurrencyCollection(block::CurrencyCollection collection, std::string &grams);
  unpackRC unpack_CurrencyCollection(block::CurrencyCollection collection, std::string &grams, td::BigInt256 &fees);
  unpackRC unpackBalance(td::Ref<vm::CellSlice> balance_ref, int64_t &balance_out);
  unpackRC unpackBalance(td::Ref<vm::CellSlice> balance_ref, int64_t &balance_out, td::BigInt256 &fees);
  unpackRC unpackMaybeBalance(vm::CellSlice &grams, int64_t &gramsCurCol);
  unpackRC unpackMessage(MessageCell msg, UniValue &uvObj, int messageType, bool small = true);

  unpackRC unpackMessageBodyJetton(vm::CellSlice body, TransferData &transferData);
  unpackRC unpackMessageBodyNFT(vm::CellSlice body, TransferData &transferData);
  unpackRC unpackMessageBodyComment(vm::CellSlice body, TransferData &transferData);
  std::string unpackMsgAddress(vm::CellSlice &body);
  

  unpackRC unpackMsgAddressExt(vm::CellSlice &body, UniValue& uvObj);
  unpackRC unpackMsgAddressExt(vm::CellSlice &body, std::string& addr);

  unpackRC unpackMsgAddressInt(vm::CellSlice &body, UniValue& uvObj);
  unpackRC unpackMsgAddressInt(vm::CellSlice &body, std::string& addr);
  
  
  unpackRC buildContractDataStruct(vm::CellSlice in_msg_state, StateInit &dataStateInit);
  unpackRC unpackStateInit(vm::CellSlice cs, std::string destination, td::Ref<vm::CellSlice> destination_cs,
                           UniValue &uvObj, TransferData &transferData);
  unpackRC unpackStateInit(vm::CellSlice cs, td::Ref<vm::CellSlice> destination_cs, UniValue &uvObj);
  std::string walletType(std::string code);

  void serializeStateInit(StateInit dataStateInit, UniValue &uvObj);
  std::string printInString(char *data, size_t size);
  unpackRC processContractMethod(StateInit dataStateInit, td::Ref<vm::CellSlice> address,
                                 std::string method, td::Ref<vm::Stack> &stack);

  std::vector<std::pair<std::string, std::pair<std::string, std::vector<std::string>>>> methods = 
  {
    {"get_wallet_data", {"jetton_wallet", {"balance", "owner", "jetton", "jetton_wallet_code"}}},
    {"get_nft_data", {"nft", {"init", "index", "collection_address", "owner_address", "individual_content"}}},
    {"get_collection_data", {"nft_collection", {"next_item_index", "collection_content", "owner_address"}}},
    {"get_jetton_data", {"jetton_root", {"total_supply", "mintable", "admin_address", "jetton_content", "jetton_wallet_code"}}}
  };

  const unsigned char Off_Chain_Content = 0x01;
  const unsigned char On_Chain_Content = 0x00;
  const unsigned char Snake_Format = 0x00;
  const unsigned char Chunked_Format = 0x01;

  typedef struct Metadata {
    std::vector<std::string> data;
    std::vector<std::string> meta{
        "uri", "name", "description", "image", "image_data", "symbol", "decimals", "amount_style", "render_type",
    };

    UniValue serialize() {
      UniValue uvObj;
      uvObj.setObject();
      for (size_t i = 0; i < data.size() && i < meta.size(); i++) {
        uvObj.pushKV(meta[i], data[i]);
      }
      return uvObj;
    }

  } Metadata;

  unpackRC getContractData(StateInit &dataStateInit, td::Ref<vm::CellSlice> address, UniValue &uvObj,
                           TransferData &transferData);
  unpackRC unpackContentData(vm::CellSlice content, UniValue &uvObj);
  unpackRC unpackContractData(vm::Stack stack, UniValue &uvObj, TransferData &transferData, std::pair<std::string, std::vector<std::string>> args);
  unpackRC unpackLongHashMap(vm::CellSlice address, std::string &outInfo, bool firstTime = true);

  /////////////////////////      OLEG's CODE                                      ////////////////
  unpackRC unpackMessageBody(vm::CellSlice in_msg_raw, const int out_msg_count, vm::CellSlice out_msg_map, UniValue& uvObj);
  unpackRC unpackMsgBody_Elector(vm::CellSlice &in_msg, vm::CellSlice &out_msg, UniValue &uvObj);
  unpackRC tryExtractMsgBody(td::Ref<vm::Cell> msg_ref, vm::CellSlice &ret);
  unpackRC elector_processNewStake(vm::CellSlice &in_msg, td::BigInt256 ans_tag, UniValue &uvObj);

  std::string sender;
  std::string recipient;  //maybe use the their internal format?
  td::BigInt256 gram_amount_in, gram_amount_out;
  std::string ELECTOR_CONST = "Ef8zMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzMzM0vF";
  unpackRC elector_recoverStake(td::BigInt256 ans_tag, UniValue& uvObj);
  /////////////////////////////////////////////////////////////////////////////////////////////////

  std::string title_;
  ton::BlockIdExt block_id_;
  ton::WorkchainId workchain_id_ = ton::workchainInvalid;
  block::StdAddress account_id_;
  bool code_ = false;

  std::string prefix_;
  td::Status error_;

  std::unique_ptr<td::StringBuilder> sb_;
  td::BufferSlice buf_;
};

template <>
struct HttpAnswer::RawData<void> {
  td::Ref<vm::Cell> root;
  RawData(td::Ref<vm::Cell> root) : root(std::move(root)) {
  }
};

template <>
inline UniValue HttpAnswer::serializeObject(RawData<void> data) {
  std::ostringstream outp;
  vm::load_cell_slice(data.root).print_rec(outp);
  return serializeObject(CodeBlock{outp.str()});
}