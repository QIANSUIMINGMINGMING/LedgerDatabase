#include "distributed/store/strongstore/shardclient.h"
#include "ledger/gpumpt/libgmpt.h"
#include <sys/time.h>

namespace strongstore {

using namespace std;
using namespace proto;

ShardClient::ShardClient(Mode mode, const string &configPath,
             Transport *transport, uint64_t client_id, int
             shard, int closestReplica)
  : transport(transport), client_id(client_id), shard(shard)
{
  ifstream configStream(configPath);
  if (configStream.fail()) {
    fprintf(stderr, "unable to read configuration file: %s\n",
        configPath.c_str());
  }
  transport::Configuration config(configStream);

  client = new replication::vr::VRClient(config, transport);

  if (mode == MODE_OCC || mode == MODE_SPAN_OCC) {
    if (closestReplica == -1) {
      replica = client_id % config.n;
    } else {
      replica = closestReplica;
    }
      } else {
    replica = 0;
  }

  waiting = NULL;
  blockingBegin = NULL;
  uid = 0;
  tip_block = 0;
  audit_block = -1;
  auto status = db_.Open("/tmp/auditor"+std::to_string(shard)+".store");
  if (!status) std::cout << "auditor db open failed" << std::endl;

  assert(getenv("device"));
  device_ = getenv("device")[0];
  assert(device_ == 'c' || device_ == 'g');
}

ShardClient::~ShardClient()
{
  delete client;
}

/* Sends BEGIN to a single shard indexed by i. */
void
ShardClient::Begin(uint64_t id)
{

  // Wait for any previous pending requests.
  if (blockingBegin != NULL) {
    blockingBegin->GetReply();
    delete blockingBegin;
    blockingBegin = NULL;
  }
}

void
ShardClient::BatchGet(uint64_t id, const std::vector<std::string> &keys,
    Promise *promise) {
  // create prepare request
  string request_str;
  Request request;
  request.set_op(Request::BATCH_GET);
  request.set_txnid(id);
  auto batchget = request.mutable_batchget();
  for (auto k : keys) {
    batchget->add_keys(k);
  }
  request.SerializeToString(&request_str);

  int timeout = 100000;
  transport->Timer(0, [=]() {
    waiting = promise;
    client->InvokeUnlogged(replica,
                           request_str,
                           bind(&ShardClient::BatchGetCallback,
                            this,
                            placeholders::_1,
                            placeholders::_2),
                           bind(&ShardClient::GetTimeout,
                            this),
                           timeout);
  });
}

void
ShardClient::GetRange(uint64_t id, const std::string& from,
                      const std::string& to, Promise* promise) {
  // create request
  string request_str;
  Request request;
  request.set_op(Request::RANGE);
  request.set_txnid(id);
  auto range = request.mutable_range();
  range->set_from(from);
  range->set_to(to);
  request.SerializeToString(&request_str);

  // set to 1 second by default
  int timeout = 100000;
  transport->Timer(0, [=]() {
    waiting = promise;
    client->InvokeUnlogged(replica,
                 request_str,
                 bind(&ShardClient::GetRangeCallback,
                  this,
                  placeholders::_1,
                  placeholders::_2),
                 bind(&ShardClient::GetTimeout,
                  this),
                 timeout); // timeout in ms
  });
}

bool
ShardClient::GetProofMultiBlock(
  const std::map<uint64_t, std::vector<string>> &keys,
  Promise *promise) {
  // printf("ShardClient::GetProofMultiBlock() unimplemented\n");
  // create request

  string request_str;
  Request request;
  request.set_op(Request::BATCH_VERIFY);
  request.set_txnid(0);
  auto verify_multi = request.mutable_verifymulti();
  for (auto& k : keys) {
    auto verify = verify_multi->add_verifys();
    verify->set_block(k.first);
    for (auto& kk : k.second) {
      verify->add_keys(kk);
    }
  }
  request.SerializeToString(&request_str);

  int timeout = 100000;
  if (device_ == 'c') {
    transport->Timer(0, [=]() {
      verifyPromise.emplace(uid, promise);
      ++uid;
      client->InvokeUnlogged(replica,
                            request_str,
                            bind(&ShardClient::GetProofMultiBlockCallback,
                              this,
                              uid - 1,
                              keys,
                              placeholders::_1,
                              placeholders::_2),
                            bind(&ShardClient::GetTimeout,
                              this),
                            timeout);
    });
  } else {
    assert(device_ == 'g');
    transport->Timer(0, [=]() {
      verifyPromise.emplace(uid, promise);
      ++uid;
      client->InvokeUnlogged(replica,
                  request_str,
                  bind(&ShardClient::GetProofMultiBlockCallbackGPU,
                    this,
                    uid - 1,
                    keys,
                    placeholders::_1,
                    placeholders::_2),
                  bind(&ShardClient::GetTimeout,
                    this),
                  timeout); // timeout in ms
    });
  }
  return true;
  
}

bool
ShardClient::BlockVerifiable(const uint64_t block) {
  if (tip_block < block) {
    printf("tip_block < block, unverifiable\n");
    return false;
  } 
  return true;
}

bool
ShardClient::GetProof(const uint64_t block,
                      const std::vector<string>& keys,
                      Promise* promise) {
  if (tip_block < block) {
    printf("tip_block < block, unverifiable\n");
    promise->Reply(REPLY_OK);
    return false;
  }
  // create request
  string request_str;
  Request request;
  request.set_op(Request::VERIFY_GET);
  request.set_txnid(0);
  auto verify = request.mutable_verify();
  verify->set_block(block);
  for (auto& k : keys) {
    verify->add_keys(k);
  }
  request.SerializeToString(&request_str);

  // set to 1 second by default
  int timeout = 100000;

  // if (device_ == 'c') {
  transport->Timer(0, [=]() {
    verifyPromise.emplace(uid, promise);
    ++uid;
    client->InvokeUnlogged(replica,
                request_str,
                bind(&ShardClient::GetProofCallback,
                  this,
                  uid - 1,
                  keys,
                  placeholders::_1,
                  placeholders::_2),
                bind(&ShardClient::GetTimeout,
                  this),
                timeout); // timeout in ms
  });
  // } else {
  // assert(device_ == 'g');
  // transport->Timer(0, [=]() {
  //   verifyPromise.emplace(uid, promise);
  //   ++uid;
  //   client->InvokeUnlogged(replica,
  //               request_str,
  //               bind(&ShardClient::GetProofCallbackGPU,
  //                 this,
  //                 uid - 1,
  //                 keys,
  //                 placeholders::_1,
  //                 placeholders::_2),
  //               bind(&ShardClient::GetTimeout,
  //                 this),
  //               timeout); // timeout in ms
  // });
  // }
  return true;
}

bool
ShardClient::Audit(const uint64_t& seq, Promise* promise) {
  // create request
  string request_str;
  Request request;
  request.set_op(Request::AUDIT);
  request.set_txnid(0);
  auto verify = request.mutable_audit();
  verify->set_seq(seq);
  request.SerializeToString(&request_str);

  // set to 1 second by default
  int timeout = 100000;
  transport->Timer(0, [=]() {
    verifyPromise.emplace(uid, promise);
    ++uid;
    client->InvokeUnlogged(replica,
                 request_str,
                 bind(&ShardClient::AuditCallback,
                  this,
                  seq,
                  uid - 1,
                  placeholders::_1,
                  placeholders::_2),
                 bind(&ShardClient::GetTimeout,
                  this),
                 timeout); // timeout in ms
  });
  return true;
}

void
ShardClient::Prepare(uint64_t id, const Transaction &txn,
          const Timestamp &timestamp, Promise *promise)
{

  // create prepare request
  string request_str;
  Request request;
  request.set_op(Request::PREPARE);
  request.set_txnid(id);
  txn.serialize(request.mutable_prepare()->mutable_txn()); 
  request.SerializeToString(&request_str);

  timeval t;
  gettimeofday(&t, NULL);
  transport->Timer(0, [=]() {
	  waiting = promise;
      client->Invoke(request_str,
               bind(&ShardClient::PrepareCallback,
                this,
                placeholders::_1,
                placeholders::_2));
    });
}

void
ShardClient::Commit(uint64_t id, const Transaction &txn,
    const std::vector<std::pair<std::string, size_t>>& versionedKeys,
    uint64_t timestamp, Promise *promise)
{
  // create commit request
  string request_str;
  Request request;
  request.set_op(Request::COMMIT);
  request.set_txnid(id);
  request.mutable_commit()->set_timestamp(timestamp);
  if (versionedKeys.size() > 0) {
    auto ver_msg = request.mutable_version();
    for (auto& vk : versionedKeys) {
      auto ver_keys = ver_msg->add_versionedkeys();
      ver_keys->set_key(vk.first);
      ver_keys->set_nversions(vk.second);
    }
  }
  request.SerializeToString(&request_str);

  blockingBegin = new Promise(COMMIT_TIMEOUT);
  timeval t;
  gettimeofday(&t, NULL);
  transport->Timer(0, [=]() {
    waiting = promise;

    client->Invoke(request_str,
      bind(&ShardClient::CommitCallback,
        this,
        placeholders::_1,
        placeholders::_2));
  });
}

/* Aborts the ongoing transaction. */
void
ShardClient::Abort(uint64_t id, const Transaction &txn, Promise *promise)
{

  // create abort request
  string request_str;
  Request request;
  request.set_op(Request::ABORT);
  request.set_txnid(id);
  txn.serialize(request.mutable_abort()->mutable_txn());
  request.SerializeToString(&request_str);

  blockingBegin = new Promise(ABORT_TIMEOUT);
  transport->Timer(0, [=]() {
	  waiting = promise;

	  client->Invoke(request_str,
			   bind(&ShardClient::AbortCallback,
				this,
				placeholders::_1,
				placeholders::_2));
  });
}

void
ShardClient::AuditCallback(uint64_t seq, size_t uid,
                           const std::string& request_str,
                           const std::string& reply_str) {
  /* Replies back from a shard. */
  Reply reply;
  reply.ParseFromString(reply_str);
  if (verifyPromise[uid] != NULL) {
    tip_block = reply.digest().block();
    VerifyStatus res;
    if (seq <= tip_block) {
      size_t nblocks = 0, ntxns = 0;
      ledgebase::ledgerdb::Auditor auditor;
      auto reply_audit = reply.laudit();
      nblocks = reply_audit.blocks_size();
      ntxns = nblocks;
      auditor.digest = reply_audit.digest();
      auditor.commit_seq = reply_audit.commit_seq();
      auditor.first_block_seq = reply_audit.first_block_seq();
      for (int i = 0; i < reply_audit.commits_size(); ++i) {
        auditor.commits.emplace_back(reply_audit.commits(i));
      }
      for (int i = 0; i < reply_audit.blocks_size(); ++i) {
        auditor.blocks.emplace_back(reply_audit.blocks(i));
      }
      for (int i = 0; i < reply_audit.mptproofs_size(); ++i) {
        auto p = reply_audit.mptproofs(i);
        ledgebase::ledgerdb::MPTProof mptproof;
        mptproof.SetValue(p.value());
        for (int j = 0; j < p.chunks_size(); ++j) {
          mptproof.AppendProof(
              reinterpret_cast<const unsigned char*>(p.chunks(j).c_str()),
              p.pos(j));
        }
        auditor.mptproofs.emplace_back(mptproof);
      }
      if (auditor.Audit(&db_)) {
        res = VerifyStatus::PASS;
      } else {
        res = VerifyStatus::FAILED;
      }
      std::cout << "# " << nblocks << " " << ntxns << std::endl;
    }

    Promise *w = verifyPromise[uid];
    verifyPromise.erase(uid);
    w->Reply(reply.status(), res);
  }
}

void 
ShardClient::GetProofMultiBlockCallback(size_t uid,
                                        const std::map<uint64_t, std::vector<std::string>>& keys,
                                        const std::string& request_str,
                                        const std::string& reply_str) {
  // printf("GetProofMultiBlockCallback is implemented\n");
  /* Replies back from a shard. */
  auto start = std::chrono::steady_clock::now();

  Reply reply;
  reply.ParseFromString(reply_str);
  if (verifyPromise[uid] != NULL) {
    tip_block = reply.digest().block();
    VerifyStatus res = VerifyStatus::PASS;

    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    ledgebase::Hash mptdigest = ledgebase::Hash::FromBase32(reply.digest().mpthash());

    auto blks_it = keys.begin();
    int keys_it_inner = 0;

    for (size_t i = 0; i < reply.proof_size(); ++i) {

      // MPT
      assert(blks_it != keys.end());

      auto p = reply.proof(i);
      ledgebase::ledgerdb::MPTProof prover;
      prover.SetValue(p.mptvalue());
      for (size_t j = 0; j < p.mpt_chunks_size(); ++j) {
        prover.AppendProof(
            reinterpret_cast<const unsigned char*>(p.mpt_chunks(j).c_str()),
            p.mpt_pos(j));
      }

      if (p.mpt_chunks_size() > 0 && 
          !prover.VerifyProof(mptdigest, blks_it->second.at(keys_it_inner))) {
        res = VerifyStatus::FAILED;
        printf("Verification Failed: block[%d] key: %s\n",
               blks_it->first, blks_it->second[keys_it_inner].c_str());
      } else {
        // printf("Verification Succeed: block[%d] key: %s\n",
        //         blks_it->first, blks_it->second[keys_it_inner].c_str());
      }

      // iterate to the next key
      if (++keys_it_inner >= blks_it->second.size()) {
        keys_it_inner = 0;
        blks_it++;
      }

      // MT
      ledgebase::ledgerdb::Proof mtprover;
      mtprover.value = p.val();
      mtprover.digest = p.hash();
      for (size_t j = 0; j < p.proof_size(); ++j) {
        mtprover.proof.emplace_back(p.proof(j));
      }
      for (size_t j = 0; j < p.mt_pos_size(); ++j) {
        mtprover.pos.emplace_back(p.mt_pos(j));
      }
      if (!mtprover.Verify()) {
        res = VerifyStatus::FAILED;
      }
    }
    gettimeofday(&t1, NULL);
    auto elapsed = ((t1.tv_sec - t0.tv_sec)*1000000 +
                    (t1.tv_usec - t0.tv_usec));
  //   //std::cout << "verify " << elapsed << " " << reply.ByteSizeLong() << " " << keys.size() << " " << res << std::endl;

    Promise *w = verifyPromise[uid];
    verifyPromise.erase(uid);
    w->Reply(REPLY_OK);
  }

  auto end = std::chrono::steady_clock::now();
    
  std::cout 
    << " GetProofMultiBlockCallback time = " 
    << std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() 
    << std::endl;
}

void
ShardClient::GetProofCallback(size_t uid,
                              const std::vector<std::string>& keys,
                              const std::string& request_str,
                              const std::string& reply_str) {
  /* Replies back from a shard. */
  Reply reply;
  reply.ParseFromString(reply_str);
  if (verifyPromise[uid] != NULL) {
    tip_block = reply.digest().block();
    VerifyStatus res = VerifyStatus::PASS;

    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    ledgebase::Hash mptdigest = ledgebase::Hash::FromBase32(reply.digest().mpthash());
    printf("reply.proof.size == %d, keys.size = %d\n", reply.proof_size(), keys.size());
    for (size_t i = 0; i < reply.proof_size(); ++i) {
      // proof mpt
      // printf("MPT proof key[%d]: %s\n", i, keys[i].c_str());
      auto p = reply.proof(i);
      ledgebase::ledgerdb::MPTProof prover;
      prover.SetValue(p.mptvalue());
      for (size_t j = 0; j < p.mpt_chunks_size(); ++j) {
        prover.AppendProof(
            reinterpret_cast<const unsigned char*>(p.mpt_chunks(j).c_str()),
            p.mpt_pos(j));
      }
      if (p.mpt_chunks_size() > 0 && !prover.VerifyProof(mptdigest, "keys[i]")) {
        res = VerifyStatus::FAILED;
      }

      // proof mt
      ledgebase::ledgerdb::Proof mtprover;
      mtprover.value = p.val();
      mtprover.digest = p.hash();
      for (size_t j = 0; j < p.proof_size(); ++j) {
        mtprover.proof.emplace_back(p.proof(j));
      }
      for (size_t j = 0; j < p.mt_pos_size(); ++j) {
        mtprover.pos.emplace_back(p.mt_pos(j));
      }
      if (!mtprover.Verify()) {
        res = VerifyStatus::FAILED;
      }
    }
    gettimeofday(&t1, NULL);
    auto elapsed = ((t1.tv_sec - t0.tv_sec)*1000000 +
                    (t1.tv_usec - t0.tv_usec));
    //std::cout << "verify " << elapsed << " " << reply.ByteSizeLong() << " " << keys.size() << " " << res << std::endl;

    Promise *w = verifyPromise[uid];
    verifyPromise.erase(uid);
    w->Reply(reply.status(), res);
  }
}

static std::string KeybytesToHex(const std::string& key) {
    size_t l = key.length() * 2 + 1;
    std::string nibbles;
    nibbles.resize(l);
    for (size_t i = 0; i < key.length(); ++i) {
      nibbles[i*2] = *reinterpret_cast<const int8_t*>(key.c_str() + i) / 16;
      nibbles[i*2+1] = *reinterpret_cast<const int8_t*>(key.c_str() + i) % 16;
    }
    nibbles[l-1] = 16;
    return nibbles;
  }

// void
// ShardClient::GetProofMultiBlockCallback(size_t uid,
//                                         const std::map<uint64_t, std::vector<std::string>>& keys,
//                                         const std::string& request_str,
//                                         const std::string& reply_str) {

void
ShardClient::GetProofMultiBlockCallbackGPU(size_t uid,
                                 const std::map<uint64_t, std::vector<std::string>>& keys,
                                 const std::string& request_str,
                                 const std::string& reply_str) {
  /* Replies back from a shard. */
  auto start = std::chrono::steady_clock::now();

  Reply reply;
  reply.ParseFromString(reply_str);
  if (verifyPromise[uid] != NULL) {
    tip_block = reply.digest().block();
    VerifyStatus res = VerifyStatus::PASS;

    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    ledgebase::Hash mptdigest = ledgebase::Hash::FromBase32(reply.digest().mpthash());
    printf("reply.proof.size == %d, keys.size = %d\n", reply.proof_size(), keys.size());

    // proof MPT in GPU
    auto blks_it = keys.begin();
    int keys_it_inner = 0;

    for (size_t i = 0; i < reply.proof_size(); ++i) {

      // MPT
      assert(blks_it != keys.end());
      
      auto p = reply.proof(i);
      
      std::string key_hex_str = KeybytesToHex(blks_it->second.at(keys_it_inner));
      const uint8_t *key_hex = reinterpret_cast<const uint8_t*>(key_hex_str.c_str());
      const int key_hex_size = key_hex_str.size();
      const uint8_t *value = reinterpret_cast<const uint8_t*>(p.mptvalue().c_str());
      const int value_size = p.mptvalue().size();
      const uint8_t *proof = reinterpret_cast<const uint8_t*>(p.mpt_chunks(0).c_str());
      const int proof_size = p.mpt_chunks(0).size();

      const uint8_t *digest = mptdigest.value();
      const int digest_size = mptdigest.kByteLength; // hash is cut in kByteLength
      bool ok = verify_proof_single(
        key_hex, key_hex_size, 
        digest, digest_size, 
        value, value_size, 
        proof, proof_size);

      if (!ok) {
        res = VerifyStatus::FAILED;
        printf("Verification Failed: block[%d] key: %s\n",
               blks_it->first, blks_it->second[keys_it_inner].c_str());
      } else {
        // printf("Verification Succeed: block[%d] key: %s\n",
        //       blks_it->first, blks_it->second[keys_it_inner].c_str());
      }

      // iterate to the next key
      if (++keys_it_inner >= blks_it->second.size()) {
        keys_it_inner = 0;
        blks_it++;
      }
      // if (!verify_proof_single(key_hex, key_hex_size, ))
      // ledgebase::ledgerdb::MPTProof prover;

      // prover.SetValue(p.mptvalue());
      // for (size_t j = 0; j < p.mpt_chunks_size(); ++j) {
      //   prover.AppendProof(
      //       reinterpret_cast<const unsigned char*>(p.mpt_chunks(j).c_str()),
      //       p.mpt_pos(j));
      // }
      // if (p.mpt_chunks_size() > 0 && !prover.VerifyProof(mptdigest, keys[i])) {
      //   res = VerifyStatus::FAILED;
      // }
    }


    for (size_t i = 0; i < reply.proof_size(); ++i) {
      // proof mpt
      // printf("MPT proof key[%d]: %s\n", i, keys[i].c_str());
      auto p = reply.proof(i);
      // proof mt
      ledgebase::ledgerdb::Proof mtprover;
      mtprover.value = p.val();
      mtprover.digest = p.hash();
      for (size_t j = 0; j < p.proof_size(); ++j) {
        mtprover.proof.emplace_back(p.proof(j));
      }
      for (size_t j = 0; j < p.mt_pos_size(); ++j) {
        mtprover.pos.emplace_back(p.mt_pos(j));
      }
      if (!mtprover.Verify()) {
        res = VerifyStatus::FAILED;
      }
    }
    gettimeofday(&t1, NULL);
    auto elapsed = ((t1.tv_sec - t0.tv_sec)*1000000 +
                    (t1.tv_usec - t0.tv_usec));
    //std::cout << "verify " << elapsed << " " << reply.ByteSizeLong() << " " << keys.size() << " " << res << std::endl;

    Promise *w = verifyPromise[uid];
    verifyPromise.erase(uid);
    w->Reply(reply.status(), res);
  }

  auto end = std::chrono::steady_clock::now();
    
  std::cout 
    << " GetProofMultiBlockCallback time = " 
    << std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() 
    << std::endl;
}


void
ShardClient::GetRangeCallback(const string &request_str, const string &reply_str)
{
  Reply reply;
  reply.ParseFromString(reply_str);
  if (waiting != NULL) {
    std::vector<Timestamp> timestamps;
    std::map<std::string, std::string> values;
    std::vector<std::string> unverified_keys;
    std::vector<uint64_t> estimate_blocks;

    tip_block = reply.digest().block();
    for (size_t i = 0; i < reply.values_size(); ++i) {
      auto v = reply.values(i);
      unverified_keys.emplace_back(v.key());
      estimate_blocks.push_back(v.estimate_block());
      values.emplace(v.key(), v.val());
    }
    for (size_t i = 0; i < reply.timestamps_size(); ++i) {
      timestamps.emplace_back(reply.timestamps(i));
    }
    Promise *w = waiting;
    waiting = NULL;
    w->Reply(reply.status(), timestamps, values, unverified_keys, estimate_blocks);
  }
}

void
ShardClient::BatchGetCallback(const string &request_str, const string &reply_str)
{
  Reply reply;
  reply.ParseFromString(reply_str);
  if (waiting != NULL) {
    std::vector<Timestamp> timestamps;
    std::map<std::string, std::string> values;
    std::vector<uint64_t> estimate_blocks;
    std::vector<std::string> unverified_keys;
    tip_block = reply.digest().block();
    for (size_t i = 0; i < reply.values_size(); ++i) {
      auto v = reply.values(i);
      unverified_keys.emplace_back(v.key());
      estimate_blocks.push_back(v.estimate_block());
      values.emplace(v.key(), v.val());
    }
    for (size_t i = 0; i < reply.timestamps_size(); ++i) {
      timestamps.emplace_back(reply.timestamps(i));
    }
    Promise *w = waiting;
    waiting = NULL;
    w->Reply(reply.status(), timestamps, values, unverified_keys, estimate_blocks);
  }
}

void
ShardClient::CommitCallback(const string &request_str, const string &reply_str)
{
  Reply reply;
  reply.ParseFromString(reply_str);
  ASSERT(reply.status() == REPLY_OK);

  ASSERT(blockingBegin != NULL);
  blockingBegin->Reply(0);

  if (waiting != NULL) {
    std::vector<uint64_t> estimate_blocks;
    std::vector<std::string> unverified_keys;
    VerifyStatus vs;
    vs = VerifyStatus::UNVERIFIED;
    tip_block = reply.digest().block();
    // printf("Commit Callback: tip_block = %d\n", tip_block);
    // printf("Commit Callback: reply.values_size() = %d\n", reply.values_size());
    for (size_t i = 0; i < reply.values_size(); ++i) {
      auto values = reply.values(i);
      unverified_keys.emplace_back(values.key());
      estimate_blocks.push_back(values.estimate_block());
      // printf("[C] Commit Callback: block[%ld] key: %s\n",  //bugs
      // values.estimate_block(), values.key().c_str());
    }
    Promise *w = waiting;
    waiting = NULL;
    w->Reply(reply.status(), vs, unverified_keys, estimate_blocks);
  }
}

void
ShardClient::GetTimeout()
{
  if (waiting != NULL) {
    Promise *w = waiting;
    waiting = NULL;
    w->Reply(REPLY_TIMEOUT);
  }
}

/* Callback from a shard replica on prepare operation completion. */
void
ShardClient::PrepareCallback(const string &request_str, const string &reply_str)
{
  Reply reply;

  timeval t;
  gettimeofday(&t, NULL);
  // std::cout << "plat " << (t.tv_sec * 1000000 + t.tv_usec - psend) << std::endl;
  // std::cout << "pressize " << reply_str.size() << std::endl;
  reply.ParseFromString(reply_str);

  if (waiting != NULL) {
    Promise *w = waiting;
    waiting = NULL;
    if (reply.has_timestamp()) {
      w->Reply(reply.status(), Timestamp(reply.timestamp(), 0));
    } else {
      w->Reply(reply.status(), Timestamp());
    }
  }
}

/* Callback from a shard replica on abort operation completion. */
void
ShardClient::AbortCallback(const string &request_str, const string &reply_str)
{
  // ABORTs always succeed.
  Reply reply;
  reply.ParseFromString(reply_str);
  ASSERT(reply.status() == REPLY_OK);

  ASSERT(blockingBegin != NULL);
  blockingBegin->Reply(0);

  if (waiting != NULL) {
    Promise *w = waiting;
    waiting = NULL;
    w->Reply(reply.status());
  }
  }

} // namespace strongstore
