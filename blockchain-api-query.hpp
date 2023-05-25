#pragma once

#include "td/actor/actor.h"
#include "td/utils/crypto.h"

#include "ton/ton-types.h"
#include "ton/ton-tl.hpp"
#include "ton/lite-tl.hpp"

#include "block/block.h"
#include "block/block-auto.h"
#include "block/mc-config.h"

#include "blockchain-api.hpp"
#include "blockchain-api-http.hpp"

#include <libpq-fe.h>

#include <map>

#include <cstdint>    // fixed precison integers
#include <numeric>    // std::accumulate
#include <algorithm>  // std::count

#include <microhttpd.h>
#include "univalue.h"

#include "crypto/block/check-proof.h"
#include "crypto/vm/utils.h"

#include "auto/tl/lite_api.h"

#include "tl-utils/tl-utils.hpp"
#include "tl-utils/lite-utils.hpp"

#include "tl/tl_json.h"

#include "tdutils/td/utils/JsonBuilder.h"

#include "common/errorcode.h"

#include "vm/boc.h"
#include "vm/cellops.h"
#include "vm/cells/MerkleProof.h"
#include "vm/vm.h"
#include "vm/cp0.h"

td::Ref<vm::Tuple> prepare_vm_c7(ton::UnixTime now, ton::LogicalTime lt, td::Ref<vm::CellSlice> my_addr,
                                 const block::CurrencyCollection &balance);
td::Result<ton::BlockIdExt> parse_block_id(std::map<std::string, std::string> &opts, bool allow_empty = false);
td::Result<block::StdAddress> parse_account_addr(std::map<std::string, std::string> &opts);
td::Result<ton::AccountIdPrefixFull> parse_account_prefix(std::map<std::string, std::string> &opts, bool allow_empty);

class HttpAnswer;

typedef struct todoS {
  uint64_t pos;
  uint32_t depth;
  std::string sofar;
} todoS;

struct BlockData {
  BlockData(ton::BlockIdExt block_id, td::BufferSlice data, td::BufferSlice shard_data,
            std::vector<td::BufferSlice> transactionsInfo)
      : block_id_(std::move(block_id))
      , data_(std::move(data))
      , shard_data_(std::move(shard_data))
      , transactionsInfo(std::move(transactionsInfo)) {
  }
  ton::BlockIdExt block_id_;
  td::BufferSlice data_;
  td::BufferSlice shard_data_;
  std::vector<td::BufferSlice> transactionsInfo;
};

struct TransactionDescr {
  TransactionDescr(block::StdAddress addr, ton::LogicalTime lt, ton::Bits256 hash) 
  : addr(addr), lt(lt), hash(hash) {
  }
  block::StdAddress addr;
  ton::LogicalTime lt;
  ton::Bits256 hash;
};

class SearchBlockRunner {
 public:
  SearchBlockRunner(std::function<void(td::Promise<std::unique_ptr<BlockData>>)> func,
                    std::shared_ptr<td::actor::Scheduler> scheduler_ptr3) {
    auto P = td::PromiseCreator::lambda([Self = this](td::Result<std::unique_ptr<BlockData>> R) {
      if (R.is_ok()) {
        Self->finish(R.move_as_ok());
      } else {
        Self->finish(nullptr);
      }
    });
    mutex_.lock();
    scheduler_ptr3->run_in_context_external([&]() { func(std::move(P)); });
  }

  void finish(std::unique_ptr<BlockData> response) {
    response_ = std::move(response);
    mutex_.unlock();
  }
  std::unique_ptr<BlockData> wait() {
    mutex_.lock();
    mutex_.unlock();
    return std::move(response_);
  }

 private:
  std::unique_ptr<BlockData> response_;
  std::mutex mutex_;
};

class HttpQueryCommon : public td::actor::Actor {
 public:
  HttpQueryCommon(std::string prefix, td::Promise<MHD_Response *> promise)
      : prefix_(std::move(prefix)), promise_(std::move(promise)) {
  }
  HttpQueryCommon(std::string prefix, td::Promise<MHD_Response *> promise, ton::BlockIdExt block_id)
      : block_id_(std::move(block_id)), prefix_(std::move(prefix)), promise_(std::move(promise)) {
  }
  HttpQueryCommon(std::string prefix, td::Promise<std::unique_ptr<BlockData>> promise)
      : prefix_(std::move(prefix)), promise_data_(std::move(promise)) {
  }
  void start_up() override {
    if (error_.is_error()) {
      abort_query(std::move(error_));
      return;
    }
    start_up_query();
  }
  virtual void start_up_query() {
    UNREACHABLE();
  }
  virtual void finish_query() {
    UNREACHABLE();
  }

  virtual void abort_query(td::Status error);
  void create_header(HttpAnswer &ans) {
  }

 protected:
  td::int32 pending_queries_ = 0;
  td::Status error_;
  td::BufferSlice data_;
  ton::BlockIdExt block_id_;

  td::BufferSlice shard_data_;
  void got_shard_info(td::BufferSlice result);
  void failed_to_get_shard_info(td::Status error);
  virtual void got_block_header(td::BufferSlice result);
  void next(td::Status error);

  std::string prefix_;
  td::Promise<MHD_Response *> promise_;
  td::Promise<std::unique_ptr<BlockData>> promise_data_;
};

class HttpQueryBlockInfo : public HttpQueryCommon {
 public:
  HttpQueryBlockInfo(ton::BlockIdExt block_id, std::string prefix, td::Promise<MHD_Response *> promise);
  HttpQueryBlockInfo(std::map<std::string, std::string> opts, std::string prefix, td::Promise<MHD_Response *> promise);
  HttpQueryBlockInfo(std::map<std::string, std::string> opts, std::string prefix, td::Promise<MHD_Response *> promise,
                     DatabaseConfigParams *conn = nullptr);

  void finish_query() override;

  void start_up_query() override;
  void got_transactions(td::BufferSlice result);
  void got_full_transaction(td::BufferSlice result);
  void got_config(td::BufferSlice result);

  void start_transaction_query(block::StdAddress addr, ton::LogicalTime lt, ton::Bits256 hash);

 private:
  
  std::vector<td::BufferSlice> transactionsInfo;
  td::BufferSlice shard_data_;
  

  std::vector<TransactionDescr> transactions_;
  std::vector<TransactionDescr> transactionsNotPassed;
  td::uint32 trans_req_count_;

  td::BufferSlice state_proof_;
  td::BufferSlice config_proof_;
  std::shared_ptr<PGconn> conn;
  std::mutex *mtx;

  bool unpackValidators(HttpAnswer &A, std::shared_ptr<PGconn> conn);
};

class HttpQueryBlockSearch2 : public HttpQueryCommon {
 public:
  HttpQueryBlockSearch2(std::map<std::string, std::string> opts, std::string prefix,
                        td::Promise<MHD_Response *> promise, std::shared_ptr<td::actor::Scheduler> scheduler_ptr);

  void finish_query() override;
  void start_up_query() override;

 private:
  ton::AccountIdPrefixFull account_prefix_;

  ton::BlockSeqno seqnoFrom = 0;
  ton::BlockSeqno seqnoTo = 0;

  std::vector<std::unique_ptr<BlockData>> dataTo;
  std::shared_ptr<td::actor::Scheduler> scheduler_ptr2;

  ton::BlockSeqno start;
  td::uint32 trans_req_count_;
};

class HttpQueryBlockSearchHash : public HttpQueryCommon {
 public:
  HttpQueryBlockSearchHash(std::map<std::string, std::string> opts, std::string prefix,
                           td::Promise<MHD_Response *> promise);

  void finish_query() override;

  void start_up_query() override;

 private:
  // ton::BlockIdExt block_id_;
  ton::AccountIdPrefixFull account_prefix_;
  td::uint32 mode_ = 0;
  ton::BlockSeqno seqno_ = 0;
  // td::BufferSlice data_;
};

class HttpQuerySearchByHeight : public HttpQueryCommon {
 public:
  HttpQuerySearchByHeight(std::map<std::string, std::string> opts, std::string prefix,
                          td::Promise<MHD_Response *> promise, DatabaseConfigParams *conn);

  void finish_query() override;

  void find_shards();
  void start_up_query() override;

 private:
  ton::AccountIdPrefixFull account_prefix_;
  td::uint32 mode_ = 0;
  ton::BlockSeqno seqno_ = 0;
  std::vector<td::BufferSlice> data_;
  std::vector<ton::BlockIdExt> block_id_;

  ton::LogicalTime lt_ = 0;
  ton::UnixTime utime_ = 0;

  std::vector<std::string> shards;
  std::shared_ptr<PGconn> conn;
  std::mutex *mtx;

};

std::vector<uint64_t> buildMemo(const std::string &S);
uint64_t findChildren(std::string &S, uint64_t pos, uint32_t depth, std::vector<uint64_t> &memo);
void unpackNode(std::string &S, uint64_t pos, uint32_t depth, std::vector<uint64_t> &memo, std::string sofar,
                std::vector<todoS> &todo, std::vector<std::string> &ret);
std::string bits2hex(const char *bit_str);
std::vector<std::string> unpackShards(std::string shardstate);
std::string hex2bits(const char *hex_str);
std::string rtrim(const std::string &s);
std::string pad2flush(const std::string &bit_str, size_t target);

class HttpQueryViewAccount : public HttpQueryCommon {
 public:
  HttpQueryViewAccount(ton::BlockIdExt block_id, block::StdAddress addr, std::string prefix,
                       td::Promise<MHD_Response *> promise);
  HttpQueryViewAccount(std::map<std::string, std::string> opts, std::string prefix,
                       td::Promise<MHD_Response *> promise);

  void finish_query() override;

  void start_up_query() override;
  void got_account(td::BufferSlice result);

 private:
  block::StdAddress addr_;

  td::BufferSlice proof_;
  ton::BlockIdExt res_block_id_;
};

class HttpQueryViewTransaction : public HttpQueryCommon {
 public:
  HttpQueryViewTransaction(block::StdAddress addr, ton::LogicalTime lt, ton::Bits256 hash, std::string prefix,
                           td::Promise<MHD_Response *> promise);
  HttpQueryViewTransaction(std::map<std::string, std::string> opts, std::string prefix,
                           td::Promise<MHD_Response *> promise, bool fromDB = false,
                           DatabaseConfigParams *dbConfParams = nullptr);

  void finish_query() override;

  void start_up_query() override;
  void got_transaction(td::BufferSlice result);

 private:
  void readTransactionsFromDB();
  void unpackFromDB(std::string data, ton::BlockIdExt res_block_id_);
  void unpackRestart();

  bool fromDB = false;
  block::StdAddress addr_;
  ton::LogicalTime lt_;
  ton::Bits256 hash_;
  bool lastCountTry = false;

  std::shared_ptr<PGconn> conn;
  std::mutex *mtx;

  ton::BlockIdExt res_block_id_;
  bool code_ = false;
  td::int32 count = 10;
  UniValue uvArr;
  bool firstTryToGetTransactions = true;
};

class HttpQueryViewTransaction2 : public HttpQueryCommon {
 public:
  HttpQueryViewTransaction2(ton::BlockIdExt block_id, block::StdAddress addr, ton::LogicalTime lt, std::string prefix,
                            td::Promise<MHD_Response *> promise);
  HttpQueryViewTransaction2(std::map<std::string, std::string> opts, std::string prefix,
                            td::Promise<MHD_Response *> promise, bool fromDB = false, DatabaseConfigParams *dbConfParams = nullptr);

  void finish_query() override;

  void start_up_query() override;
  void got_transaction(td::BufferSlice result);

 private:
  void readFromFile(td::BufferSlice &data);
  bool fromDB = false;
  std::shared_ptr<PGconn> conn;
  std::mutex *mtx;
  block::StdAddress addr_;
  ton::LogicalTime lt_;
  ton::Bits256 hash_;

};

class HttpQueryViewLastBlock : public HttpQueryCommon {
 public:
  HttpQueryViewLastBlock(std::string prefix, td::Promise<MHD_Response *> promise);
  HttpQueryViewLastBlock(std::map<std::string, std::string> opts, std::string prefix,
                         td::Promise<MHD_Response *> promise);

  void finish_query() override;

  void start_up() override;
  void got_result(td::BufferSlice result);

 private:
  ton::BlockIdExt res_block_id_;
};

class HttpQueryViewLastBlockNumber : public HttpQueryCommon {
 public:
  HttpQueryViewLastBlockNumber(std::string prefix, td::Promise<MHD_Response *> promise);
  HttpQueryViewLastBlockNumber(std::map<std::string, std::string> opts, std::string prefix,
                               td::Promise<MHD_Response *> promise, bool fromDB = false,
                           DatabaseConfigParams *dbConfParams = nullptr);

  void finish_query() override;

  void start_up() override;
  void get_shards();
  void got_result(td::BufferSlice result);

  void getFromDB();


 private:
  ton::BlockIdExt res_block_id_;
  td::int32 workchain_;
  bool fromDb = false;
  std::shared_ptr<PGconn> conn;
  std::mutex *mtx;
};

class HttpQueryConfig : public HttpQueryCommon {
 public:
  HttpQueryConfig(std::string prefix, ton::BlockIdExt block_id, std::vector<td::int32> params,
                  td::Promise<MHD_Response *> promise);
  HttpQueryConfig(std::map<std::string, std::string> opts, std::string prefix, td::Promise<MHD_Response *> promise);

  void finish_query() override;

  void start_up() override;
  void got_block(td::BufferSlice result);
  void send_main_query();
  void got_result(td::BufferSlice result);

 private:
  std::vector<td::int32> params_ = {32, 34, 36};

  td::BufferSlice state_proof_;
  td::BufferSlice config_proof_;

  bool code_ = true;
};

class HttpQueryValidators : public HttpQueryCommon {
 public:
  HttpQueryValidators(std::string prefix, ton::BlockIdExt block_id, td::Promise<MHD_Response *> promise);
  HttpQueryValidators(std::map<std::string, std::string> opts, std::string prefix, td::Promise<MHD_Response *> promise);

  void finish_query() override;

  void start_up() override;
  void got_block(td::BufferSlice result);
  void send_main_query();
  void got_result(td::BufferSlice result);

 private:
  std::vector<td::int32> params_ = {32, 34};

  td::BufferSlice state_proof_;
  td::BufferSlice config_proof_;

  bool code_ = true;
};

class HttpQueryRunMethod : public HttpQueryCommon {
 public:
  HttpQueryRunMethod(ton::BlockIdExt block_id, block::StdAddress addr, std::string method_name,
                     std::vector<vm::StackEntry> params, std::string prefix, td::Promise<MHD_Response *> promise);
  HttpQueryRunMethod(std::map<std::string, std::string> opts, std::string prefix, td::Promise<MHD_Response *> promise);

  void finish_query() override;

  void start_up_query() override;
  void got_account(td::BufferSlice result);

 private:
  block::StdAddress addr_;

  std::string method_name_;
  std::vector<vm::StackEntry> params_;

  td::BufferSlice proof_;
  td::BufferSlice shard_proof_;
  ton::BlockIdExt res_block_id_;

  bool code_ = true;
};

class BlockSearch : public HttpQueryCommon {
 public:
  BlockSearch(ton::WorkchainId workchain, ton::AccountIdPrefix account, ton::BlockSeqno seqno, std::string prefix,
              td::Promise<MHD_Response *> promise);
  BlockSearch(ton::WorkchainId workchain, ton::AccountIdPrefix account, ton::LogicalTime lt, std::string prefix,
              td::Promise<MHD_Response *> promise);
  BlockSearch(ton::WorkchainId workchain, ton::AccountIdPrefix account, bool dummy, ton::UnixTime utime,
              std::string prefix, td::Promise<MHD_Response *> promise);

  BlockSearch(std::map<std::string, std::string> opts, std::string prefix, td::Promise<MHD_Response *> promise);

  BlockSearch(std::map<std::string, std::string> opts, std::string prefix, td::Promise<MHD_Response *> promise,
              bool returnData);

  BlockSearch(ton::WorkchainId wc, ton::ShardId shard, ton::BlockSeqno seqno_, std::string prefix,
              td::Promise<std::unique_ptr<BlockData>> promise)
      : HttpQueryCommon(std::move(prefix), std::move(promise)), account_prefix_{wc, shard}, seqno_(seqno_) {
  }

  void start_up_query() override;
  void got_block_header(td::BufferSlice result) override;

  void got_transactions(td::BufferSlice result);
  void got_full_transaction(td::BufferSlice result);

  void start_transaction_query(block::StdAddress addr, ton::LogicalTime lt, ton::Bits256 hash);

 protected:
  ton::AccountIdPrefixFull account_prefix_;
  td::uint32 mode_ = 0;
  ton::BlockSeqno seqno_ = 0;
  ton::LogicalTime lt_ = 0;
  ton::UnixTime utime_ = 0;

  std::vector<TransactionDescr> transactions_;
  td::uint32 trans_req_count_;
  std::vector<td::BufferSlice> transactionsInfo;

  bool returnData = false;
};

class HttpQueryBlockSearch : public BlockSearch {
 
 public:
  HttpQueryBlockSearch(std::map<std::string, std::string> opts, std::string prefix, td::Promise<MHD_Response *> promise):
  BlockSearch(std::move(opts), std::move(prefix), std::move(promise)) { }

  void finish_query() override;

};

class HttpQueryBlockSearchNoMHD : public BlockSearch {
 public:
  HttpQueryBlockSearchNoMHD(ton::WorkchainId wc, ton::ShardId shard, ton::BlockSeqno seqno_, std::string prefix,
                            td::Promise<std::unique_ptr<BlockData>> promise)
      : BlockSearch(std::move(wc), std::move(shard), std::move(seqno_), std::move(prefix), std::move(promise)) {
    mode_ = 1;
  }

  void finish_query() override {
    if (promise_data_) {
      std::unique_ptr<BlockData> bd2(new BlockData(std::move(block_id_), std::move(data_), std::move(shard_data_), std::move(transactionsInfo)));
      promise_data_.set_value(std::move(bd2));
    }
    stop();
  }
};
