#ifndef CEPH_RGW_SYNC_H
#define CEPH_RGW_SYNC_H

#include "rgw_coroutine.h"
#include "rgw_http_client.h"
#include "rgw_meta_sync_status.h"

#include "common/RWLock.h"


struct rgw_mdlog_info {
  uint32_t num_shards;

  rgw_mdlog_info() : num_shards(0) {}

  void decode_json(JSONObj *obj);
};


class RGWAsyncRadosProcessor;
class RGWMetaSyncStatusManager;
class RGWMetaSyncCR;
class RGWRESTConn;


#define DEFAULT_BACKOFF_MAX 30

class RGWSyncBackoff {
  int cur_wait;
  int max_secs;

  void update_wait_time();
public:
  RGWSyncBackoff(int _max_secs = DEFAULT_BACKOFF_MAX) : cur_wait(0), max_secs(_max_secs) {}

  void backoff_sleep();
  void reset() {
    cur_wait = 0;
  }

  void backoff(RGWCoroutine *op);
};

struct RGWMetaSyncEnv {
  CephContext *cct;
  RGWRados *store;
  RGWRESTConn *conn;
  RGWAsyncRadosProcessor *async_rados;
  RGWHTTPManager *http_manager;

  RGWMetaSyncEnv() : cct(NULL), store(NULL), conn(NULL), async_rados(NULL), http_manager(NULL) {}

  string shard_obj_name(int shard_id);
  string status_oid();
};


class RGWRemoteMetaLog : public RGWCoroutinesManager {
  RGWRados *store;
  RGWRESTConn *conn;
  RGWAsyncRadosProcessor *async_rados;

  RGWHTTPManager http_manager;
  RGWMetaSyncStatusManager *status_manager;

  RGWMetaSyncCR *meta_sync_cr;

  RGWSyncBackoff backoff;

  RGWMetaSyncEnv sync_env;

  void init_sync_env(RGWMetaSyncEnv *env) {
    env->cct = store->ctx();
    env->store = store;
    env->conn = conn;
    env->async_rados = async_rados;
    env->http_manager = &http_manager;
  }

public:
  RGWRemoteMetaLog(RGWRados *_store, RGWMetaSyncStatusManager *_sm) : RGWCoroutinesManager(_store->ctx()), store(_store),
                                       conn(NULL), async_rados(nullptr),
                                       http_manager(store->ctx(), &completion_mgr),
                                       status_manager(_sm), meta_sync_cr(NULL) {}

  int init();
  void finish();

  int read_log_info(rgw_mdlog_info *log_info);
  int list_shard(int shard_id);
  int list_shards(int num_shards);
  int get_shard_info(int shard_id);
  int clone_shards(int num_shards, vector<string>& clone_markers);
  int fetch(int num_shards, vector<string>& clone_markers);
  int read_sync_status(rgw_meta_sync_status *sync_status);
  int init_sync_status(int num_shards);
  int set_sync_info(const rgw_meta_sync_info& sync_info);
  int run_sync(int num_shards, rgw_meta_sync_status& sync_status);

  void wakeup(int shard_id);

  RGWMetaSyncEnv& get_sync_env() {
    return sync_env;
  }
};

class RGWMetaSyncStatusManager {
  RGWRados *store;
  librados::IoCtx ioctx;

  RGWRemoteMetaLog master_log;

  rgw_meta_sync_status sync_status;
  map<int, rgw_obj> shard_objs;

  int num_shards;

  struct utime_shard {
    utime_t ts;
    int shard_id;

    utime_shard() : shard_id(-1) {}

    bool operator<(const utime_shard& rhs) const {
      if (ts == rhs.ts) {
	return shard_id < rhs.shard_id;
      }
      return ts < rhs.ts;
    }
  };

  RWLock ts_to_shard_lock;
  map<utime_shard, int> ts_to_shard;
  vector<string> clone_markers;

public:
  RGWMetaSyncStatusManager(RGWRados *_store) : store(_store), master_log(store, this), num_shards(0),
                                               ts_to_shard_lock("ts_to_shard_lock") {}
  int init();
  void finish();

  rgw_meta_sync_status& get_sync_status() { return sync_status; }

  int read_sync_status() { return master_log.read_sync_status(&sync_status); }
  int init_sync_status() { return master_log.init_sync_status(num_shards); }
  int fetch() { return master_log.fetch(num_shards, clone_markers); }
  int clone_shards() { return master_log.clone_shards(num_shards, clone_markers); }

  int run() { return master_log.run_sync(num_shards, sync_status); }

  void wakeup(int shard_id) { return master_log.wakeup(shard_id); }
  void stop() {
    master_log.finish();
  }
};

template <class T>
class RGWSyncShardMarkerTrack {
  typename std::map<T, uint64_t> pending;

  T high_marker;
  uint64_t high_index;

  int window_size;
  int updates_since_flush;


protected:
  virtual RGWCoroutine *store_marker(const T& new_marker, uint64_t index_pos) = 0;
  virtual void handle_finish(const T& marker) { }

public:
  RGWSyncShardMarkerTrack(int _window_size) : high_index(0), window_size(_window_size), updates_since_flush(0) {}
  virtual ~RGWSyncShardMarkerTrack() {}

  void start(const T& pos, int index_pos) {
    pending[pos] = index_pos;
  }

  RGWCoroutine *finish(const T& pos) {
    assert(!pending.empty());

    typename std::map<T, uint64_t>::iterator iter = pending.begin();
    const T& first_pos = iter->first;

    typename std::map<T, uint64_t>::iterator pos_iter = pending.find(pos);
    assert(pos_iter != pending.end());

    if (!(pos <= high_marker)) {
      high_marker = pos;
      high_index = pos_iter->second;
    }

    pending.erase(pos);

    handle_finish(pos);

    updates_since_flush++;

    if (pos == first_pos && (updates_since_flush >= window_size || pending.empty())) {
      return update_marker(high_marker, high_index);
    }
    return NULL;
  }

  RGWCoroutine *update_marker(const T& new_marker, uint64_t index_pos) {
    updates_since_flush = 0;
    return store_marker(new_marker, index_pos);
  }
};

#endif
