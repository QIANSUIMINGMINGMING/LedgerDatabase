#include "ledger/ledgerdb/ledgerdb.h"

#include <thread>
#include <chrono>
#include <atomic>
#include <iostream>
#include <sys/time.h>
#include <assert.h>

#include "ledger/common/utils.h"
#include "ledger/ledgerdb/types.h"
#include "ledger/gpumpt/libgmpt.h"

namespace ledgebase {

namespace ledgerdb {

LedgerDB::LedgerDB(int timeout,
                   std::string dbpath,
                   std::string ledgerPath) {
  assert(db_.Open(dbpath));
  assert(ledger_.Open(ledgerPath));
  mt_.reset(new MerkleTree(&ledger_));
  sl_.reset(new SkipList(&db_));
  next_block_seq_ = 0;
  commit_seq_ = 0;
  stop_.store(false);
  assert(getenv("device"));
  assert(getenv("insert"));
  olc_or_2pi_ = getenv("insert")[0];

  if (getenv("device")[0] == 'c') {
    buildThread_.reset(new std::thread(&LedgerDB::buildTree, this, timeout));
  } else if(getenv("device")[0] == 'g')  {
    buildThread_.reset(new std::thread(&LedgerDB::buildTreeGPU, this, timeout));
  } else {
    printf("error\n");
  }
  gpumpt_ = init_mpt();
}

LedgerDB::~LedgerDB() {
  stop_.store(true);

  if (buildThread_ != nullptr) {
    if (buildThread_->joinable()) buildThread_->join();
  }
  free_mpt(gpumpt_);
}

// my_blk_hash|{blk_seq} -> {hash}
// latest_commit -> {latest_built_blk}|{mt_root}

namespace util {
inline int key_bytes_to_hex(
  const uint8_t *key_bytes, int key_bytes_size, uint8_t *key_hexs) {
  int l = key_bytes_size * 2 + 1;
  for (int i = 0; i < key_bytes_size; ++i) {
    key_hexs[i * 2] = key_bytes[i] / 16;
    key_hexs[i * 2 + 1] = key_bytes[i] % 16;
  }
  key_hexs[l - 1] = 16;
  return key_bytes_size * 2 + 1;
}

inline int key_bytes_to_hex_size(int key_bytes_size) {
  return key_bytes_size * 2 + 1;
}

inline uint8_t *element_start(
  int *indexs, int i, uint8_t *all_bytes) {
  return &all_bytes[indexs[2 * i]];
}

inline uint8_t *element_start(
  int64_t *indexs, int i, uint8_t *all_bytes) {
  return &all_bytes[indexs[2 * i]];
}

inline int element_size(const int *indexs, int i) {
  return int(indexs[2 * i + 1] - indexs[2 * i] + 1);
}

inline void println_str(const uint8_t *str, size_t size) {
  for (size_t i = 0; i < size; ++i) {
    printf("%c ", str[i]);
  }
  printf("\n");
}
inline void println_hex(const uint8_t *str, size_t size) {
  static const char hex2str[] = {'0', '1', '2', '3', '4', '5', '6', '7',
                                 '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  char *buf = new char[size * 2 + 1]{};
  for (size_t i = 0; i < size; ++i) {
    int h = str[i] / 16;
    int l = str[i] % 16;
    buf[i * 2] = hex2str[h];
    buf[i * 2 + 1] = hex2str[l];
  }
  buf[size * 2] = '\0';
  printf("%s\n", buf);
  delete[] buf;
}
inline void print_hex(const uint8_t *str, size_t size) {
  for (size_t i = 0; i < size; ++i) {
    printf("%02x ", str[i]);
  }
}
}


void LedgerDB::buildTreeGPU(int timeout) {
  while (!stop_.load()) {
    timeval t0, t1;
    gettimeofday(&t0, NULL);

    bool added = false;
    uint64_t first_block;
    uint64_t last_block;
    Tree_Block blk;
    std::vector<std::string> mt_new_hashes;
    std::map<std::string, std::string> mpt_blks;

    std::chrono::steady_clock::time_point begin1 = std::chrono::steady_clock::now();  // =====================
    while (tree_queue_.try_pop(blk)) {
      if (!added) {
        first_block = blk.blk_seq;
      }
      last_block = blk.blk_seq;
      added = true;
      std::string mt_blk_val;
      ledger_.Get("ledger-"+std::to_string(blk.blk_seq), &mt_blk_val);
      auto hash = Hash::ComputeFrom(mt_blk_val);
      mt_new_hashes.push_back(hash.ToBase32());
      for (size_t i = 0; i < blk.mpt_ks.size(); i++) {
        mpt_blks[blk.mpt_ks[i]] = blk.mpt_ts;
      }
    }

    // std::chrono::steady_clock::time_point begine2 = std::chrono::steady_clock::now(); // =====================
    if (added) {
      // update merkle tree
      std::string prev_commit_seq = (commit_seq_ == 0) ?
          "" : std::to_string(commit_seq_ - 1);
      std::string root_key, root_hash;
      mt_->update(first_block, mt_new_hashes, prev_commit_seq,
          &root_key, &root_hash);


      Hash mpt_root, newmptroot;
      CommitInfo prev, curr;
      std::string prev_digest;
      if (commit_seq_ > 0) {
        std::string commit_info;
        ledger_.Get("commit" + prev_commit_seq, &commit_info);
        prev = CommitInfo(commit_info);
        mpt_root = Hash::FromBase32(prev.mptroot);
        prev_digest = Hash::FromBase32(commit_info).ToBase32();
      }


      // start update MPT --------------------------------------------------
      // TODO: collect into continuous
      std::chrono::steady_clock::time_point beginMPT = std::chrono::steady_clock::now();

      int keys_hexs_size = 0;
      int values_bytes_size = 0;
      const int nkv = mpt_blks.size();
      int keys_index_size = mpt_blks.size() * 2;
      int values_index_size = mpt_blks.size() * 2;
      int *keys_hexs_index = new int[keys_index_size];
      int64_t *values_bytes_index = new int64_t[values_index_size];
      const uint8_t **values_hps = new const uint8_t*[nkv];

      // printf("Start, nKey = nvalue = %d\n", keys_index_size, values_index_size);
      int i = 0;
      for (auto iter = mpt_blks.begin(); iter != mpt_blks.end(); iter++) {
        // printf("i=%d\n", i);
        keys_hexs_index[2 * i] = keys_hexs_size;
        keys_hexs_size += util::key_bytes_to_hex_size(iter->first.size());
        keys_hexs_index[2 * i + 1] = keys_hexs_size - 1;

        values_bytes_index[2 * i] = values_bytes_size;
        values_bytes_size += iter->second.size();
        values_bytes_index[2 * i + 1] = values_bytes_size - 1;
        i++;
      }

      // printf("Finish 1\n");

      uint8_t *keys_hexs = new uint8_t[keys_hexs_size];
      uint8_t *values_bytes = new uint8_t[values_bytes_size];

      i = 0;
      for (auto iter = mpt_blks.begin(); iter != mpt_blks.end(); iter++) {
        const uint8_t *k = (uint8_t *)iter->first.c_str();
        const uint8_t *v = (uint8_t *)iter->second.c_str();
        const int ksize = iter->first.size();
        const int vsize = iter->second.size();

        util::key_bytes_to_hex(k, ksize, util::element_start(keys_hexs_index, i, keys_hexs));
        memcpy(util::element_start(values_bytes_index, i, values_bytes), v, vsize);

        values_hps[i] = util::element_start(values_bytes_index, i, values_bytes);
        i++;
      }

      // std::vector<std::string> mpt_ks, mpt_vs;
      // for(auto iter = mpt_blks.begin(); iter != mpt_blks.end(); iter++) {
      //   mpt_ks.push_back(iter->first);
      //   mpt_vs.push_back(iter->second);
      // } 

      // if (mpt_root.empty()) {
      //   printf("MPT is empty\n");
      // }
      // printf("Insert %d keys GPU\n", nkv);
      if (olc_or_2pi_ == 'o') {
        printf("insert olc\n");
        auto hash = insert_mpt_olc(gpumpt_, keys_hexs, keys_hexs_index, 
                    values_bytes, values_bytes_index, values_hps, nkv);
        newmptroot = Hash(hash);
      } else {
        printf("insert 2phase\n");
        assert(olc_or_2pi_ == '2');
        auto hash = insert_mpt_2phase(gpumpt_, keys_hexs, keys_hexs_index, 
                    values_bytes, values_bytes_index, values_hps, nkv);
        newmptroot = Hash(hash);
      }
      // printf("Finish Insert GPU MPT\n");
      // std::cout << "New Root From Me" << newmptroot << std::endl;

      // if (mpt_root.empty()) {
      //   printf("MPT is empty\n");
      //   printf("Insert %lu keys\n", nkv);
      //   auto mpt = Trie(&db_, mpt_ks, mpt_vs);
      //   newmptroot = mpt.hash().Clone();

      // } else {
      //   printf("Insert %lu keys\n", nkv);
      //   newmptroot = Trie(&db_, mpt_root).Set(mpt_ks, mpt_vs).Clone();
      // }

      // std::cout << "New Root from LedgerDB: " << newmptroot << std::endl;
      // return;
      std::chrono::steady_clock::time_point endMPT = std::chrono::steady_clock::now();

      
      std::string commit_entry = CommitInfo(commit_seq_, prev_digest,
          last_block, root_hash, root_key, newmptroot.ToBase32()).ToString();
      ledger_.Put("commit" + std::to_string(commit_seq_), commit_entry);
      std::string newdigest = DigestInfo(commit_seq_, last_block,
          Hash::ComputeFrom(commit_entry).ToBase32()).ToString();
      ledger_.Put("digest", newdigest);
      ++commit_seq_;

      std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
      // fprintf(stderr, "All time = %ld us\n", std::chrono::duration_cast<std::chrono::microseconds>(end - begin1).count());
      // fprintf(stderr, "MPT time = %ld us\n", std::chrono::duration_cast<std::chrono::microseconds>(endMPT - beginMPT).count());
      // fprintf(stderr, "Update MT&DB time = %ld us\n", std::chrono::duration_cast<std::chrono::microseconds>(end - begine2).count());
      // gettimeofday(&t1, NULL);
      // auto latency = (t1.tv_sec - t0.tv_sec)*1000000 + t1.tv_usec - t0.tv_usec;
      // std::cerr << "persist " << latency << " " << mpt_ks.size() << " " << mt_new_hashes.size() << std::endl;

      auto time_mpt   = std::chrono::duration_cast<std::chrono::microseconds>(endMPT - beginMPT).count();
      auto time_all   = std::chrono::duration_cast<std::chrono::microseconds>(end - begin1).count();
      fprintf(stderr, "ADS update %d keys, time_mpt: %d, time_all %d\n", nkv, time_mpt, time_all);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(timeout));
  }
}

void LedgerDB::buildTree(int timeout) {
  // buildTreeGPU(timeout);
  // return;
  while (!stop_.load()) {
    timeval t0, t1;
    gettimeofday(&t0, NULL);

    std::chrono::steady_clock::time_point begin1 = std::chrono::steady_clock::now(); // =====================

    bool added = false;
    uint64_t first_block;
    uint64_t last_block;
    Tree_Block blk;
    std::vector<std::string> mt_new_hashes;
    std::map<std::string, std::string> mpt_blks;

    while (tree_queue_.try_pop(blk)) {
      if (!added) {
        first_block = blk.blk_seq;
      }
      last_block = blk.blk_seq;
      added = true;
      std::string mt_blk_val;
      ledger_.Get("ledger-"+std::to_string(blk.blk_seq), &mt_blk_val);
      auto hash = Hash::ComputeFrom(mt_blk_val);
      mt_new_hashes.push_back(hash.ToBase32());
      for (size_t i = 0; i < blk.mpt_ks.size(); i++) {
        mpt_blks[blk.mpt_ks[i]] = blk.mpt_ts;
      }
    }

    // std::chrono::steady_clock::time_point begine2 = std::chrono::steady_clock::now(); // =====================
    if (added) {
      // update merkle tree
      std::string prev_commit_seq = (commit_seq_ == 0) ?
          "" : std::to_string(commit_seq_ - 1);
      std::string root_key, root_hash;
      mt_->update(first_block, mt_new_hashes, prev_commit_seq,
          &root_key, &root_hash);

      Hash mpt_root, newmptroot;
      CommitInfo prev, curr;
      std::string prev_digest;
      if (commit_seq_ > 0) {
        std::string commit_info;
        ledger_.Get("commit" + prev_commit_seq, &commit_info);
        prev = CommitInfo(commit_info);
        mpt_root = Hash::FromBase32(prev.mptroot);
        prev_digest = Hash::FromBase32(commit_info).ToBase32();
      }

      // update MPT --------------------------------------------------
      std::chrono::steady_clock::time_point beginMPT = std::chrono::steady_clock::now();
      std::vector<std::string> mpt_ks, mpt_vs;
      for(auto iter = mpt_blks.begin(); iter != mpt_blks.end(); iter++) {
        mpt_ks.push_back(iter->first);
        mpt_vs.push_back(iter->second);
      }

      if (mpt_root.empty()) {
        // printf("MPT is empty\n");
        // printf("Insert %lu keys\n", mpt_ks.size());
        auto mpt = Trie(&db_, mpt_ks, mpt_vs);
        newmptroot = mpt.hash().Clone();
      } else {
        // printf("Insert %lu keys\n", mpt_ks.size());
        newmptroot = Trie(&db_, mpt_root).Set(mpt_ks, mpt_vs).Clone();
      }

      std::chrono::steady_clock::time_point endMPT = std::chrono::steady_clock::now();
      // update MPT --------------------------------------------------


      std::string commit_entry = CommitInfo(commit_seq_, prev_digest,
          last_block, root_hash, root_key, newmptroot.ToBase32()).ToString();
      ledger_.Put("commit" + std::to_string(commit_seq_), commit_entry);
      std::string newdigest = DigestInfo(commit_seq_, last_block,
          Hash::ComputeFrom(commit_entry).ToBase32()).ToString();
      ledger_.Put("digest", newdigest);
      ++commit_seq_;

      std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
      // fprintf(stderr, "All time = %ld us\n", std::chrono::duration_cast<std::chrono::microseconds>(end - begin1).count());
      // fprintf(stderr, "MPT time = %ld us\n", std::chrono::duration_cast<std::chrono::microseconds>(endMPT - beginMPT).count());
      // fprintf(stderr, "Update MT&DB time = %ld us\n");
      // auto time_other = std::chrono::duration_cast<std::chrono::microseconds>(beginMPT - begin1).count();
      auto time_mpt   = std::chrono::duration_cast<std::chrono::microseconds>(endMPT - beginMPT).count();
      auto time_all   = std::chrono::duration_cast<std::chrono::microseconds>(end - begin1).count();
      fprintf(stderr, "ADS update %d keys, time_mpt: %d, time_all %d\n", mpt_ks.size(), time_mpt, time_all);
      // gettimeofday(&t1, NULL);
      // auto latency = (t1.tv_sec - t0.tv_sec)*1000000 + t1.tv_usec - t0.tv_usec;
      // std::cerr << "persist " << latency << " " << mpt_ks.size() << " " << mt_new_hashes.size() << std::endl;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(timeout));
  }
}

// {key} -> {value}|{ts}
// {key}|{ts} -> {blk_seq}|{value}
// my_ledger_block|{blk_seq} -> {key1}|{val1}/{key2}|{val2}/...
// my_blk_hash|{blk_seq} -> {hash}
uint64_t LedgerDB::Set(const std::vector<std::string> &keys,
                   const std::vector<std::string> &values,
                   const uint64_t &timestamp) {
  // printf("The block contains %d kvs\n", keys.size());
  if (keys.size() == 0) return true;
  auto ts_str = std::to_string(timestamp);
  auto blk_seq = next_block_seq_++;
  auto blk_seq_str = std::to_string(blk_seq);
  auto blk_key = "ledger-" + blk_seq_str;
  std::string blk_val = BlockData(keys, values).ToString();

  ledger_.Put(blk_key, blk_val);
  
  std::vector<std::string> mpt_ks;
  for (size_t i = 0; i < keys.size(); i++) {
    mpt_ks.push_back(keys[i]);
    // printf("value = %s@%s, blk_seq = %d\n", blk_seq_str.c_str(), values[i].c_str(), blk_seq);
    sl_->insert("skiplist_" + keys[i], timestamp,
        blk_seq_str + "@" + values[i]);
    skiplist_head_[keys[i]] = timestamp;
  }

  tree_queue_.push({blk_seq, mpt_ks, ts_str});
  
  return blk_seq;
}

int LedgerDB::binarySearch(std::vector<std::string> &vec, int l, int r, std::string key) {
	if (r >= l) {
		int mid = l + (r - l) / 2;
		if (vec[mid] == key)
			return mid;
		if (vec[mid] > key)
			return binarySearch(vec, l, mid - 1, key);
		return binarySearch(vec, mid + 1, r, key);
	}
	return -1;
}

// {key} -> {value}|{ts}
// {key}|{ts} -> {blk_seq}|{value}
// my_ledger_block|{blk_seq} -> {key1}|{val1}/{key2}|{val2}/...
// my_blk_hash|{blk_seq} -> {hash}
bool LedgerDB::GetValues(const std::vector<std::string> &keys,
                         std::vector<std::pair<uint64_t, std::pair<size_t, std::string>>> &values) {
  for (size_t i = 0; i < keys.size(); i++) {
    std::string val;

    auto node = sl_->find("skiplist_" + keys[i], skiplist_head_[keys[i]]);
    if (node.size() == 0) {
      values.push_back(std::make_pair(0, std::make_pair(0, "")));
      continue;
    }
    SkipNode skipnode(node);
    // printf("skipnode.value = %s\n", skipnode.value.c_str());
    auto res = Utils::splitBy(skipnode.value, '@');

    values.push_back(std::make_pair(skiplist_head_[keys[i]],
        std::make_pair(std::stoul(res[0]), res[1])));
  }

  return true;
}

bool LedgerDB::GetRange(const std::string &start, const std::string &end,
                        std::map<std::string, std::pair<uint64_t, std::pair<size_t, std::string>>> &values) {
  auto from = skiplist_head_.lower_bound(start);
  auto to = skiplist_head_.upper_bound(end);
  for (auto it = from; it != to && it != skiplist_head_.end(); ++it) {
    auto node = sl_->find("skiplist_" + it->first, it->second);
    SkipNode skipnode(node);
    auto res = Utils::splitBy(skipnode.value, '@');
    values.emplace(it->first, std::make_pair(it->second,
        std::make_pair(std::stoul(res[0]), res[1])));
  }
  return true;
}

// {key} -> {value}|{ts}|{blk_seq}
// {key}|{ts} -> {blk_seq}|{value}
// my_ledger_block|{blk_seq} -> {key1}|{val1}/{key2}|{val2}/...
// my_blk_hash|{blk_seq} -> {hash}
bool LedgerDB::GetVersions(const std::vector<std::string> &keys,
                           std::vector<std::vector<std::pair<uint64_t, std::pair<size_t, std::string>>>> &values,
                           size_t nversions){
  for (int i = 0; i < keys.size(); i++) {
    std::vector<std::string> nodes;
    sl_->scan("skiplist_" + keys[i], nversions, nodes);
    std::vector<std::pair<uint64_t, std::pair<size_t, std::string>>> vs;
    for (auto& node : nodes) {
      SkipNode skipnode(node);
      auto res = Utils::splitBy(skipnode.value, '@');
      vs.push_back(std::make_pair(skipnode.key,
          std::make_pair(std::stoul(res[0]), res[1])));
    }
    values.push_back(vs);
    // auto iter = db_.NewIterater();
    // size_t cnt = 0;
    // std::vector<std::pair<uint64_t, std::pair<size_t, std::string>>> vs;
    // for (iter->Seek("skiplist_"+keys[i]+"|");
    //      iter->Valid() && iter->key().starts_with("skiplist_"+keys[i]+"|");
    //      iter->Next()) {
    //   auto k = iter->key().ToString();
    //   auto v = iter->value().ToString();
    //   auto blk_val = Utils::splitBy(v, '|');
    //   if (blk_val.size() != 2) return false;

    //   uint64_t blk_seq = std::stoul(blk_val[0]);
    //   uint64_t ts = std::stoul(k.substr(k.find_last_of("|")+1));

    //   vs.push_back(std::make_pair(ts, std::make_pair(blk_seq, blk_val[1])));
    //   cnt ++;
    //   if (cnt >= nversions) break;
    // }
    // values.push_back(vs);
  }

  return true;
}

bool LedgerDB::GetRootDigest(uint64_t *blk_seq, std::string *root_digest) {
  std::string digestinfo;
  ledger_.Get("digest", &digestinfo);
  if (digestinfo.size() == 0) {
    *root_digest = "";
    *blk_seq = 0;
  } else {
    DigestInfo digest(digestinfo);
    *root_digest = digest.digest;
    *blk_seq = digest.tip_block;
  }
  return true;
}

// TODO
bool LedgerDB::GetProofsGPU(const std::vector<std::string> &keys,
                            const std::vector<size_t> key_blk_seqs,
                            std::vector<Proof> &mt_proofs,

                            const uint8_t *&mpt_proofs,
                            const int *&mpt_proofs_indexs,
                            const uint8_t **&mpt_values_hps,
                            const int *&mpt_values_sizes,

                            // std::vector<MPTProof> &mpt_proofs,
                            std::string *root_digest,
                            size_t *blk_seq,
                            std::string *mpt_digest) {
  std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();

  std::string digest, commit;
  ledger_.Get("digest", &digest);
  auto commit_seq = std::to_string(DigestInfo(digest).commit_seq);
  ledger_.Get("commit" + commit_seq, &commit);
  CommitInfo cinfo(commit);
  std::string mt_root_key = cinfo.mtrootkey;
  *root_digest = cinfo.mtroot;
  *blk_seq = cinfo.tip_block;
  *mpt_digest = cinfo.mptroot;
  // auto mpt_hash = Hash::FromBase32(*mpt_digest);

  // auto mpt = Trie(&db_, mpt_hash);
  size_t current = 0;
  // printf("Get %d Proofs\n", keys.size());
  
  // std::chrono::steady_clock::time_point 
  //   begin_mt = std::chrono::steady_clock::now();

  for (size_t i = 0; i < keys.size(); i++) {
    if (key_blk_seqs[i] != current) {
      auto mt_proof = mt_->getProof(commit_seq, mt_root_key,
          *blk_seq, key_blk_seqs[i]);
      mt_proofs.push_back(mt_proof);
      current = key_blk_seqs[i];
    }
  }

  // std::chrono::steady_clock::time_point
  //   end_mt = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point 
    begin_mpt = std::chrono::steady_clock::now();

  // collect into continous
  int keys_hexs_size = 0;
  const int n_keys = keys.size();
  int keys_index_size = n_keys * 2;
  int *keys_hexs_index = new int[keys_index_size];
  
  for (int i = 0; i < keys.size(); i++) {
    keys_hexs_index[2 * i] = keys_hexs_size;
    keys_hexs_size += util::key_bytes_to_hex_size(keys[i].size());
    keys_hexs_index[2 * i + 1] = keys_hexs_size - 1;
  }

  uint8_t *keys_hexs = new uint8_t[keys_hexs_size];

  for (int i = 0; i < keys.size(); i++) {
    const uint8_t *k = reinterpret_cast<const uint8_t*>(keys[i].c_str());
    const int ksize = keys[i].size();
    util::key_bytes_to_hex(k, ksize, util::element_start(keys_hexs_index, i, keys_hexs));
  }
  
  const uint8_t *hash = nullptr;
  int hash_size = 0;

  auto begin_get_proofs_gpu = std::chrono::steady_clock::now();
  get_proofs(
    gpumpt_, keys_hexs, keys_hexs_index, n_keys,    // in
    mpt_values_hps, mpt_values_sizes,               // value
    mpt_proofs, mpt_proofs_indexs,                  // proof
    hash, hash_size);                               // hash
  auto end_get_proofs_gpu = std::chrono::steady_clock::now();
  printf("get proof libgmpt time = %d us\n", 
    std::chrono::duration_cast<std::chrono::microseconds>
      (end_get_proofs_gpu - begin_get_proofs_gpu).count());
      
  // TODO compare mptroot

  std::string newmptroot = Hash(hash).ToBase32();
  if (newmptroot != cinfo.mptroot) {
    // printf("The returned root is not the same with it from DB\n");
    // std::cout << "newmptroot: " << newmptroot << std::endl
    //           << "from DB: " << cinfo.mptroot << std::endl;
    *mpt_digest = newmptroot;
  }

  // for (size_t i = 0; i < keys.size(); i++) {
  //   auto mpt_proof = mpt.GetProof(keys[i]);
  //   mpt_proofs.push_back(mpt_proof);
  // }

  std::chrono::steady_clock::time_point 
  end_mpt = std::chrono::steady_clock::now();

  // printf("get proof mt time = %d us, mpt_time = %d us\n", 
  //   std::chrono::duration_cast<std::chrono::microseconds>(end_mt - begin_mt).count(),
  //   std::chrono::duration_cast<std::chrono::microseconds>(end_mpt - begin_mpt).count()
  // );

  auto time_all = std::chrono::duration_cast<std::chrono::microseconds>(end_mpt - begin).count();
  auto time_mpt = std::chrono::duration_cast<std::chrono::microseconds>(end_mpt - begin_mpt).count();

  fprintf(stderr, "ADS getproof %d keys, time_mpt: %d, time_all %d\n", keys.size(), time_mpt, time_all);

  return true;
}

// {key} -> {value}|{ts}|{blk_seq}
// {key}|{ts} -> {blk_seq}|{value}
// my_ledger_block|{blk_seq} -> {key1}|{val1}/{key2}|{val2}/...
// my_blk_hash|{blk_seq} -> {hash}
// latest_commit -> {latest_built_blk}|{mt_root}
bool LedgerDB::GetProofs(const std::vector<std::string> &keys,
                         const std::vector<size_t> key_blk_seqs,
                         std::vector<Proof> &mt_proofs,
                         std::vector<MPTProof> &mpt_proofs,
                         std::string *root_digest,
                         size_t *blk_seq,
                         std::string *mpt_digest) {
  std::chrono::steady_clock::time_point 
    begin = std::chrono::steady_clock::now();

  std::string digest, commit;
  ledger_.Get("digest", &digest);
  auto commit_seq = std::to_string(DigestInfo(digest).commit_seq);
  ledger_.Get("commit" + commit_seq, &commit);
  CommitInfo cinfo(commit);
  std::string mt_root_key = cinfo.mtrootkey;
  *root_digest = cinfo.mtroot;
  *blk_seq = cinfo.tip_block;
  *mpt_digest = cinfo.mptroot;
  auto mpt_hash = Hash::FromBase32(*mpt_digest);

  // std::chrono::steady_clock::time_point 
  //   begin_mt = std::chrono::steady_clock::now();

  size_t current = 0;
  // printf("Get %d Proofs\n", keys.size());
  for (size_t i = 0; i < keys.size(); i++) {
    if (key_blk_seqs[i] != current) {
      // MT get proof: get the proof of a block
      auto mt_proof = mt_->getProof(commit_seq, mt_root_key,
          *blk_seq, key_blk_seqs[i]);
      mt_proofs.push_back(mt_proof);
      current = key_blk_seqs[i];
    }
  }

  // std::chrono::steady_clock::time_point 
  //   end_mt = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point 
    begin_mpt = std::chrono::steady_clock::now();

  auto mpt = Trie(&db_, mpt_hash);

  for (size_t i = 0; i < keys.size(); i++) {    // MPT get proof: get the proof of a key in the MPT
    auto mpt_proof = mpt.GetProof(keys[i]);
    mpt_proofs.push_back(mpt_proof);
  }

  std::chrono::steady_clock::time_point 
    end_mpt = std::chrono::steady_clock::now();

  // len(mpt_proofs) == #n keys
  // len(mt_proofs) == #n blocks
  // printf("get proof mt time = %d us, mpt_time = %d us\n", 
  //   std::chrono::duration_cast<std::chrono::microseconds>(end_mt - begin_mt).count(),
  //   std::chrono::duration_cast<std::chrono::microseconds>(end_mpt - begin_mpt).count()
  // );

  auto time_all = std::chrono::duration_cast<std::chrono::microseconds>(end_mpt - begin).count();
  auto time_mpt = std::chrono::duration_cast<std::chrono::microseconds>(end_mpt - begin_mpt).count();

  fprintf(stderr, "ADS getproof %d keys, time_mpt: %d, time_all %d\n", keys.size(), time_mpt, time_all);
  return true;
}

Auditor LedgerDB::GetAudit(const uint64_t seq) {
  Auditor auditor;
  std::string digest;
  ledger_.Get("digest", &digest);
  DigestInfo dinfo(digest);
  auditor.digest = dinfo.digest;

  std::string target_commit;
  for (size_t i = seq; i <= dinfo.tip_block; ++i) {
    std::string c;
    ledger_.Get("commit"+std::to_string(i), &c);
    auditor.commits.emplace_back(c);
    if (i == seq) {
      target_commit = c;
    }
  }

  CommitInfo cinfo(target_commit);
  auditor.commit_seq = cinfo.commit_seq;

  if (seq > 0) {
    std::string prev_commit;
    ledger_.Get("commit"+std::to_string(seq-1), &prev_commit);
    auditor.first_block_seq = CommitInfo(prev_commit).tip_block + 1;
  } else {
    auditor.first_block_seq = 0;
  }
  
  auto mpt_hash = Hash::FromBase32(cinfo.mptroot);
  auto mpt = Trie(&db_, mpt_hash);
  for (size_t i = auditor.first_block_seq; i <= cinfo.tip_block; ++i) {
    std::string blockdata;
    ledger_.Get("ledger-" + std::to_string(i), &blockdata);
    auditor.blocks.emplace_back(blockdata);
    BlockData binfo(blockdata);
    for (size_t j = 0; j < binfo.keys.size(); ++j) {
      auto mpt_proof = mpt.GetProof(binfo.keys[j]);
      auditor.mptproofs.emplace_back(mpt_proof);
    }
  }

  return auditor;
}

std::string LedgerDB::splitAndFind(const std::string &str, char delim, const::std::string &target) {
  auto res_val = "NOTFOUND";
  std::stringstream ss(str);
  while(ss.good()) {
    std::string s;
    getline(ss, s, delim);
    auto pos = s.find("|");
    if (pos == s.npos) return "NOTFOUND";
    if (s.substr(0, pos) == target) {
      return s.substr(pos+1);
    }
  }
  return res_val;
}

bool Auditor::Audit(DB* db) {
  bool res = true;
  MerkleTree mt(db);
  std::string target = digest;
  std::string mtroot, mptroot, prev_commit_seq;
  uint64_t last_block;

  for (auto it = commits.rbegin(); it != commits.rend(); ++it) {
    if (Hash::ComputeFrom(*it).ToBase32().compare(target) != 0) return false;
    CommitInfo cinfo(*it);
    target = cinfo.prev_digest;
    mtroot = cinfo.mtroot;
    mptroot = cinfo.mptroot;
    last_block = cinfo.tip_block;
    prev_commit_seq = cinfo.commit_seq > 0?
        std::to_string(cinfo.commit_seq - 1) : "";
  }

  std::vector<std::string> mt_new_hashes;
  for (size_t i = 0; i < blocks.size(); ++i) {
    mt_new_hashes.emplace_back(Hash::ComputeFrom(blocks[i]).ToBase32());
  }

  std::string root_key, root_hash;
  mt.update(first_block_seq, mt_new_hashes, prev_commit_seq,
      &root_key, &root_hash);

  if (root_hash.compare(mtroot) != 0) res = false;

  auto mpt_hash = Hash::ComputeFrom(mptroot);
  int cnt = 0;
  for (size_t i = 0; i < blocks.size(); ++i) {
    BlockData binfo(blocks[i]);
    for (size_t j = 0; j < binfo.keys.size(); ++j) {
      res = res || mptproofs[cnt].VerifyProof(mpt_hash, binfo.keys[j]);
      ++cnt;
    }
  }
  return res;
}

}  // namespace ledgerdb

}  // namespace ledgebase
