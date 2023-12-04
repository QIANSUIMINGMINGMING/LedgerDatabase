#include "distributed/store/common/backend/versionstore.h"
#include <sys/time.h>

#include "ledger/common/utils.h"

using namespace std;

VersionedKVStore::VersionedKVStore() { }

VersionedKVStore::VersionedKVStore(const string& db_path, int timeout) {
  ldb.reset(new ledgebase::ledgerdb::LedgerDB(timeout));
}
    
VersionedKVStore::~VersionedKVStore() { }

bool VersionedKVStore::GetDigest(strongstore::proto::Reply* reply) {
  uint64_t tip;
  std::string hash;
  ldb->GetRootDigest(&tip, &hash);
  auto digest = reply->mutable_digest();
  digest->set_block(tip);
  digest->set_hash(hash);
  return true;
}

bool VersionedKVStore::GetNVersions(
    std::vector<std::pair<std::string, size_t>>& ver_keys,
    strongstore::proto::Reply* reply) {
  std::vector<std::vector<
      std::pair<uint64_t, std::pair<size_t, std::string>>>> get_val_res;
  std::vector<std::string> keys;
  for (auto& k : ver_keys) {
    keys.emplace_back(k.first);
  }
  size_t i = 0;
  ldb->GetVersions(keys, get_val_res, ver_keys[0].second);
  for (auto& kres : get_val_res) {
    for (auto& res : kres) {
      auto kv = reply->add_values();
      kv->set_key(keys[i]);
      kv->set_val(res.second.second);
      kv->set_estimate_block(res.second.first);
    }
    ++i;
  }
  return true;
}

bool VersionedKVStore::BatchGet(const std::vector<std::string>& keys,
    strongstore::proto::Reply* reply) {
  std::vector<std::pair<uint64_t, std::pair<size_t, std::string>>> get_val_res;
  ldb->GetValues(keys, get_val_res);
  size_t i = 0;
  for (auto& res : get_val_res) {
    auto kv = reply->add_values();
    kv->set_key(keys[i]);
    kv->set_val(res.second.second);
    kv->set_estimate_block(res.second.first);
    reply->add_timestamps(res.first);
    ++i;
  }
  return true;
}

bool VersionedKVStore::GetRange(const std::string &start,
    const std::string &end, strongstore::proto::Reply* reply) {
  std::map<std::string,
      std::pair<uint64_t, std::pair<size_t, std::string>>> range_res;
  ldb->GetRange(start, end, range_res);
  for (auto& res : range_res) {
    auto kv = reply->add_values();
    kv->set_key(res.first);
    kv->set_val(res.second.second.second);
    kv->set_estimate_block(res.second.second.first);
    reply->add_timestamps(res.second.first);
  }
  return true;
}
namespace util {
inline const uint8_t *element_start(
  const int *indexs, int i, const uint8_t *all_bytes) {
  return &all_bytes[indexs[2 * i]];
}

inline const uint8_t *element_start(
  const int64_t *indexs, int i, const uint8_t *all_bytes) {
  return &all_bytes[indexs[2 * i]];
}

inline int element_size(const int *indexs, int i) {
  return int(indexs[2 * i + 1] - indexs[2 * i] + 1);
}
}

bool VersionedKVStore::GetProof(
    const std::map<uint64_t, std::vector<std::string>>& keys,
    strongstore::proto::Reply* reply) {
  timeval t0, t1;
  gettimeofday(&t0, NULL);
  int nkey = 0;
  std::vector<ledgebase::ledgerdb::Proof> mtproof;
  std::vector<ledgebase::ledgerdb::MPTProof> mptproof;
  std::vector<std::string> blk_datas;
  std::string mtdigest;
  std::string mptdigest;
  size_t block;

  // printf("[V] Start a GetProof:\n");
  std::vector<std::string> ks;
  std::vector<uint64_t> blks;
  for (auto& vkey : keys) {
    // printf("[V] block[%ld] has %ld keys\n", vkey.first, vkey.second.size());
    for (auto& k : vkey.second) {
      ks.push_back(k);
      blks.push_back(vkey.first);
    }
  }
  nkey = ks.size();
  // printf("[V] before start a getproof\n");
  

  const char *cpu_or_gpu = getenv("device");
  assert(cpu_or_gpu);
  if (cpu_or_gpu[0] == 'c') {
    ldb->GetProofs(ks, blks, mtproof, mptproof, &mtdigest,
        &block, &mptdigest);

    auto digest = reply->mutable_digest();
    digest->set_block(block);
    digest->set_hash(mtdigest);
    digest->set_mpthash(mptdigest);
    
    for (size_t i = 0; i < mtproof.size(); ++i) {
      auto p = reply->add_proof();
      p->set_val(mtproof[i].value);
      p->set_hash(mtproof[i].digest);
      for (size_t j = 0; j < mtproof[i].proof.size(); ++j) {
        p->add_proof(mtproof[i].proof[j]);
        p->add_mt_pos(mtproof[i].pos[j]);
      }

      printf("Set mpt proof[%d] for key[%s]\n", i, ks[i].c_str());
      p->set_mptvalue(mptproof[i].GetValue());
      for (size_t j = 0; j < mptproof[i].MapSize(); ++j) {
        p->add_mpt_chunks(mptproof[i].GetMapChunk(j));
        p->add_mpt_pos(mptproof[i].GetMapPos(j));
      }
    }

  } else {
    assert(cpu_or_gpu[0] == 'g');
    const uint8_t *mpt_proofs = nullptr;
    const int *mpt_proof_indexs = nullptr;
    const uint8_t **mpt_values_hps = nullptr;
    const int *mpt_values_sizes = nullptr;

    // ldb->GetProofsGPU(ks, blks, mtproof, mptproof, &mtdigest,
    //       &block, &mptdigest);
    ldb->GetProofsGPU(ks, blks, mtproof, 
      mpt_proofs, mpt_proof_indexs, 
      mpt_values_hps, mpt_values_sizes, 
      &mtdigest, &block, &mptdigest);

    auto digest = reply->mutable_digest();

    digest->set_block(block);
    digest->set_hash(mtdigest);
    digest->set_mpthash(mptdigest);
    
    for (size_t i = 0; i < mtproof.size(); ++i) {
      auto p = reply->add_proof();
      p->set_val(mtproof[i].value);
      p->set_hash(mtproof[i].digest);
      for (size_t j = 0; j < mtproof[i].proof.size(); ++j) {
        p->add_proof(mtproof[i].proof[j]);
        p->add_mt_pos(mtproof[i].pos[j]);
      }

      // p->set_mptvalue(mptproof[i].GetValue());
      // for (size_t j = 0; j < mptproof[i].MapSize(); ++j) {
      //   p->add_mpt_chunks(mptproof[i].GetMapChunk(j));
      //   p->add_mpt_pos(mptproof[i].GetMapPos(j));
      // }
      const uint8_t *value = mpt_values_hps[i];
      const int value_size = mpt_values_sizes[i];
      const uint8_t *proof = util::element_start(mpt_proof_indexs, i, mpt_proofs);
      const int proof_size = util::element_size(mpt_proof_indexs, i);
      p->set_mptvalue(value, value_size);
      p->add_mpt_chunks(proof, proof_size);
    }
  } 



  printf("Verify nkeys = %d, mpt_proof.size = %d, mt_proof.size = %d, reply.proof_size= %d\n", 
         nkey, mptproof.size(), mtproof.size(), reply->proof_size());

  gettimeofday(&t1, NULL);
  auto lat = (t1.tv_sec - t0.tv_sec)*1000000 + t1.tv_usec - t0.tv_usec;
  //std::cout << "getproof " << lat << " " << nkey << std::endl;
  return true;
}

bool VersionedKVStore::GetProof(const uint64_t& seq,
    strongstore::proto::Reply* reply) {
  auto auditor = ldb->GetAudit(seq);
  auto reply_auditor = reply->mutable_laudit();
  reply_auditor->set_digest(auditor.digest);
  reply_auditor->set_commit_seq(auditor.commit_seq);
  reply_auditor->set_first_block_seq(auditor.first_block_seq);
  for (size_t i = 0; i < auditor.commits.size(); ++i) {
    reply_auditor->add_commits(auditor.commits[i]);
  }
  for (size_t i = 0; i < auditor.blocks.size(); ++i) {
    reply_auditor->add_blocks(auditor.blocks[i]);
  }
  for (auto& mptproof : auditor.mptproofs) {
    auto reply_mptproof = reply_auditor->add_mptproofs();
    reply_mptproof->set_value(mptproof.GetValue());
    for (size_t i = 0; i < mptproof.MapSize(); ++i) {
      reply_mptproof->add_chunks(mptproof.GetMapChunk(i));
      reply_mptproof->add_pos(mptproof.GetMapPos(i));
    }
  }
  return true;
}


void VersionedKVStore::put(const vector<string> &keys,
    const vector<string> &values, const Timestamp &t,
    strongstore::proto::Reply* reply)
{
  auto estimate_blocks = ldb->Set(keys, values, t.getTimestamp());
  if (reply != nullptr) {
    auto kv = reply->add_values();
    for (size_t i = 0; i < keys.size(); ++i) {
      kv->set_key(keys[i]);
      kv->set_val(values[i]);
      kv->set_estimate_block(estimate_blocks);
    }
  }
}

bool VersionedKVStore::get(const std::string &key,
                           const Timestamp &t,
                           std::pair<Timestamp, std::string> &value)
{
    std::vector<std::pair<uint64_t, std::pair<size_t, std::string>>> get_val_res;
    ldb->GetValues({key}, get_val_res);
    value = std::make_pair(get_val_res[0].second.first,
        get_val_res[0].second.second);
    return true;
}
