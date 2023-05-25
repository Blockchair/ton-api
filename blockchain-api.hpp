#ifndef blockchain_explorer
#define blockchain_explorer

#include "td/actor/actor.h"
#include "td/utils/buffer.h"
#include "ton/ton-types.h"
#include "td/utils/port/IPAddress.h"
#include <microhttpd.h>
#include <bits/stdc++.h>
#include <libpq-fe.h>

#define MAX_POST_SIZE (64 << 10)

// Beginning with v0.9.71, libmicrohttpd changed the return type of most
// functions from int to enum MHD_Result
// https://git.gnunet.org/gnunet.git/tree/src/include/gnunet_mhd_compat.h
// proposes to define a constant for the return type so it works well
// with all versions of libmicrohttpd
#if MHD_VERSION >= 0x00097002
#define MHD_RESULT enum MHD_Result
#else
#define MHD_RESULT int
#endif

extern bool local_scripts_;

// #define pg_ptr PGconn, decltype(&PQfinish)

typedef struct DatabaseConfigParams {
  DatabaseConfigParams(std::shared_ptr<PGconn> conn, std::mutex *mtxDB) : conn(std::move(conn)) {
    this->mtxDB = mtxDB;
  }
  std::shared_ptr<PGconn> conn;
  std::mutex *mtxDB;
} DatabaseConfigParams;

class CoreActorInterface : public td::actor::Actor {
 public:
  struct RemoteNodeStatus {
    std::vector<ton::BlockIdExt> values_;
    td::Timestamp ts_;
    RemoteNodeStatus(size_t size, td::Timestamp ts) : ts_(ts) {
      values_.resize(size);
    }
  };

  struct RemoteNodeStatusList {
    std::vector<td::IPAddress> ips;
    std::vector<std::shared_ptr<RemoteNodeStatus>> results;
  };
  virtual ~CoreActorInterface() = default;

  virtual void send_lite_query(td::BufferSlice data, td::Promise<td::BufferSlice> promise) = 0;
  virtual void get_last_result(td::Promise<std::shared_ptr<RemoteNodeStatus>> promise) = 0;
  virtual void get_results(td::uint32 max, td::Promise<RemoteNodeStatusList> promise) = 0;

  static td::actor::ActorId<CoreActorInterface> instance_actor_id();
};

#endif