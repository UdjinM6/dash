#include "rocksdb_wrapper.h"
#include "test_utils.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

using test_utils::Fail;
using test_utils::MakeTempDir;

namespace {
uint64_t SumWalLogFileSize(const std::string& dir) {
  std::error_code ec;
  uint64_t total = 0;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (ec) {
      return 0;
    }
    if (!entry.is_regular_file(ec) || ec) {
      if (ec) {
        return 0;
      }
      continue;
    }
    const std::filesystem::path path = entry.path();
    if (path.extension() == ".log") {
      total += static_cast<uint64_t>(entry.file_size(ec));
      if (ec) {
        return 0;
      }
    }
  }
  return total;
}

uint64_t VarintLen(uint64_t value) {
  if (value < (1ULL << 7)) {
    return 1;
  }
  if (value < (1ULL << 14)) {
    return 2;
  }
  if (value < (1ULL << 21)) {
    return 3;
  }
  if (value < (1ULL << 28)) {
    return 4;
  }
  if (value < (1ULL << 35)) {
    return 5;
  }
  if (value < (1ULL << 42)) {
    return 6;
  }
  if (value < (1ULL << 49)) {
    return 7;
  }
  if (value < (1ULL << 56)) {
    return 8;
  }
  if (value < (1ULL << 63)) {
    return 9;
  }
  return 10;
}

uint64_t PaidLen(uint64_t len) {
  return len + VarintLen(len);
}
}  // namespace

int main() {
  std::string error;
  auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  std::string dir = MakeTempDir("storage_test_" + std::to_string(now));

  grovedb::RocksDbWrapper db;
  if (!db.Open(dir, &error)) {
    Fail("open rocksdb failed: " + error);
  }

  const std::vector<uint8_t> key = {'k'};
  const std::vector<uint8_t> key_w = {'w'};
  const std::vector<uint8_t> value_a = {'a'};
  const std::vector<uint8_t> value_b = {'b'};
  const std::vector<uint8_t> value_x = {'x'};
  const std::vector<uint8_t> root = {'r'};
  const std::vector<uint8_t> child = {'c'};

  if (!db.Put(grovedb::ColumnFamilyKind::kDefault, {root}, key, value_a, &error)) {
    Fail("put default failed: " + error);
  }
  if (!db.Put(grovedb::ColumnFamilyKind::kDefault, {root, child}, key, value_b, &error)) {
    Fail("put nested failed: " + error);
  }

  std::string checkpoint_dir = dir + "_checkpoint";
  if (!db.CreateCheckpoint(checkpoint_dir, &error)) {
    Fail("create checkpoint failed: " + error);
  }
  {
    grovedb::RocksDbWrapper checkpoint_db;
    if (!checkpoint_db.Open(checkpoint_dir, &error)) {
      Fail("open checkpoint failed: " + error);
    }
    std::vector<uint8_t> checkpoint_got;
    bool checkpoint_found = false;
    if (!checkpoint_db.Get(
            grovedb::ColumnFamilyKind::kDefault, {root}, key, &checkpoint_got, &checkpoint_found, &error)) {
      Fail("get from checkpoint default failed: " + error);
    }
    if (!checkpoint_found || checkpoint_got != value_a) {
      Fail("checkpoint default value mismatch");
    }
    if (!checkpoint_db.Get(grovedb::ColumnFamilyKind::kDefault,
                           {root, child},
                           key,
                           &checkpoint_got,
                           &checkpoint_found,
                           &error)) {
      Fail("get from checkpoint nested failed: " + error);
    }
    if (!checkpoint_found || checkpoint_got != value_b) {
      Fail("checkpoint nested value mismatch");
    }
  }

  error.clear();
  if (db.CreateCheckpoint(checkpoint_dir, &error)) {
    Fail("create checkpoint overwrite should fail");
  }
  if (error.empty()) {
    Fail("create checkpoint overwrite should return error");
  }

  std::string checkpoint_dir_2 = dir + "_checkpoint_2";
  if (!db.Put(grovedb::ColumnFamilyKind::kDefault, {root}, key_w, value_x, &error)) {
    Fail("put before second checkpoint failed: " + error);
  }
  if (!db.CreateCheckpoint(checkpoint_dir_2, &error)) {
    Fail("create second checkpoint failed: " + error);
  }
  const uint64_t checkpoint_wal_size = SumWalLogFileSize(checkpoint_dir_2);
  if (checkpoint_wal_size == 0) {
    Fail("checkpoint should preserve non-empty WAL when log flush threshold is disabled");
  }

  {
    grovedb::RocksDbWrapper unopened_db;
    error.clear();
    if (unopened_db.CreateCheckpoint(dir + "_checkpoint_unopened", &error)) {
      Fail("create checkpoint on unopened db should fail");
    }
    if (error != "database not opened") {
      Fail("unexpected unopened checkpoint error: " + error);
    }
  }

  std::vector<uint8_t> got;
  bool found = false;
  if (!db.Get(grovedb::ColumnFamilyKind::kDefault, {root}, key, &got, &found, &error)) {
    Fail("get default failed: " + error);
  }
  if (!found || got != value_a) {
    Fail("get default mismatch");
  }
  if (!db.Get(grovedb::ColumnFamilyKind::kDefault, {root, child}, key, &got, &found, &error)) {
    Fail("get nested failed: " + error);
  }
  if (!found || got != value_b) {
    Fail("get nested mismatch");
  }

  {
    grovedb::RocksDbWrapper unopened_db;
    grovedb::RocksDbWrapper::PrefixedIterator uninitialized;
    error.clear();
    if (uninitialized.Init(&unopened_db, grovedb::ColumnFamilyKind::kDefault, {root}, &error)) {
      Fail("iterator init on unopened db should fail");
    }
    if (error != "storage not opened") {
      Fail("unexpected iterator init error: " + error);
    }
    error.clear();
    if (uninitialized.SeekToFirst(&error)) {
      Fail("seek first on uninitialized iterator should fail");
    }
    if (error != "iterator not initialized") {
      Fail("unexpected iterator seek first error: " + error);
    }
    error.clear();
    if (uninitialized.Seek({'a'}, &error)) {
      Fail("seek on uninitialized iterator should fail");
    }
    if (error != "iterator not initialized") {
      Fail("unexpected iterator seek error: " + error);
    }
    error.clear();
    if (uninitialized.SeekToLast(&error)) {
      Fail("seek last on uninitialized iterator should fail");
    }
    if (error != "iterator not initialized") {
      Fail("unexpected iterator seek last error: " + error);
    }
    error.clear();
    if (uninitialized.SeekForPrev({'a'}, &error)) {
      Fail("seek for prev on uninitialized iterator should fail");
    }
    if (error != "iterator not initialized") {
      Fail("unexpected iterator seek for prev error: " + error);
    }
    error.clear();
    if (uninitialized.Next(&error)) {
      Fail("next on uninitialized iterator should fail");
    }
    if (error != "iterator not initialized") {
      Fail("unexpected iterator next error: " + error);
    }
    error.clear();
    if (uninitialized.Prev(&error)) {
      Fail("prev on uninitialized iterator should fail");
    }
    if (error != "iterator not initialized") {
      Fail("unexpected iterator prev error: " + error);
    }

    grovedb::RocksDbWrapper::PrefixedIterator it;
    if (!it.Init(&db, grovedb::ColumnFamilyKind::kDefault, {root}, &error)) {
      Fail("iterator init failed: " + error);
    }
    std::vector<uint8_t> key_out;
    std::vector<uint8_t> value_out;
    error.clear();
    if (it.Key(&key_out, &error)) {
      Fail("iterator key before seek should fail");
    }
    if (error != "iterator not valid") {
      Fail("unexpected iterator key-before-seek error: " + error);
    }
    error.clear();
    if (it.Value(&value_out, &error)) {
      Fail("iterator value before seek should fail");
    }
    if (error != "iterator not valid") {
      Fail("unexpected iterator value-before-seek error: " + error);
    }
    if (!it.SeekToFirst(&error)) {
      Fail("iterator seek failed: " + error);
    }
    if (!it.Valid()) {
      Fail("iterator expected valid");
    }
    if (!it.Key(&key_out, &error) || !it.Value(&value_out, &error)) {
      Fail("iterator read failed: " + error);
    }
    if (key_out != key || value_out != value_a) {
      Fail("iterator mismatch");
    }
    if (!it.SeekToLast(&error)) {
      Fail("iterator seek last failed: " + error);
    }
    if (!it.Valid()) {
      Fail("iterator expected valid at last");
    }
    if (!it.Key(&key_out, &error) || !it.Value(&value_out, &error)) {
      Fail("iterator read last failed: " + error);
    }
    if (key_out != key_w || value_out != value_x) {
      Fail("iterator last mismatch");
    }
  }

  {
    grovedb::RocksDbWrapper::Transaction tx;
    if (!db.BeginTransaction(&tx, &error)) {
      Fail("begin transaction failed: " + error);
    }
    const std::vector<uint8_t> key_tx = {'t'};
    const std::vector<uint8_t> value_tx = {'x'};
    if (!tx.Put(grovedb::ColumnFamilyKind::kDefault, {root}, key_tx, value_tx, &error)) {
      Fail("tx put failed: " + error);
    }
    if (!tx.Get(grovedb::ColumnFamilyKind::kDefault, {root}, key_tx, &got, &found, &error)) {
      Fail("tx get failed: " + error);
    }
    if (!found || got != value_tx) {
      Fail("tx get mismatch");
    }
    if (!tx.Commit(&error)) {
      Fail("tx commit failed: " + error);
    }
    if (!db.Get(grovedb::ColumnFamilyKind::kDefault, {root}, key_tx, &got, &found, &error)) {
      Fail("get after tx commit failed: " + error);
    }
    if (!found || got != value_tx) {
      Fail("tx commit result mismatch");
    }
  }

  {
    grovedb::RocksDbWrapper::Transaction tx;
    if (!db.BeginTransaction(&tx, &error)) {
      Fail("begin transaction prefixed failed: " + error);
    }
    grovedb::RocksDbWrapper::TransactionPrefixedContext ctx(&tx,
                                                         grovedb::ColumnFamilyKind::kDefault,
                                                         {root});
    const std::vector<uint8_t> key_tx = {'p'};
    const std::vector<uint8_t> value_tx = {'q'};
    if (!ctx.Put(key_tx, value_tx, &error)) {
      Fail("prefixed tx put failed: " + error);
    }
    if (!ctx.Get(key_tx, &got, &found, &error)) {
      Fail("prefixed tx get failed: " + error);
    }
    if (found) {
      Fail("prefixed tx put without batch should be hidden/no-op");
    }
    if (!tx.Put(grovedb::ColumnFamilyKind::kDefault, {root}, key_tx, value_tx, &error)) {
      Fail("prefixed tx direct put failed: " + error);
    }
    if (!ctx.Delete(key_tx, &error)) {
      Fail("prefixed tx delete failed: " + error);
    }
    if (!ctx.Get(key_tx, &got, &found, &error)) {
      Fail("prefixed tx get after no-op delete failed: " + error);
    }
    if (!found || got != value_tx) {
      Fail("prefixed tx no-op delete should not remove direct tx write");
    }
    if (!tx.Commit(&error)) {
      Fail("prefixed tx commit failed: " + error);
    }
    if (!db.Get(grovedb::ColumnFamilyKind::kDefault, {root}, key_tx, &got, &found, &error)) {
      Fail("prefixed tx get after commit failed: " + error);
    }
    if (!found || got != value_tx) {
      Fail("prefixed tx commit mismatch");
    }
  }

  {
    grovedb::RocksDbWrapper::Transaction tx;
    if (!db.BeginTransaction(&tx, &error)) {
      Fail("begin transaction prefixed batch failed: " + error);
    }
    grovedb::RocksDbWrapper::WriteBatch staged;
    grovedb::RocksDbWrapper::TransactionPrefixedContext ctx(
        &tx, grovedb::ColumnFamilyKind::kDefault, {root}, &staged);
    const std::vector<uint8_t> key_tx = {'b', 'p'};
    const std::vector<uint8_t> value_tx = {'b', 'v'};
    if (!ctx.Put(key_tx, value_tx, &error)) {
      Fail("prefixed tx staged put failed: " + error);
    }
    if (!ctx.Get(key_tx, &got, &found, &error)) {
      Fail("prefixed tx staged get failed: " + error);
    }
    if (found) {
      Fail("prefixed tx staged write should be hidden before batch commit");
    }
    if (!db.CommitBatch(staged, &tx, &error)) {
      Fail("prefixed tx staged batch commit failed: " + error);
    }
    if (!ctx.Get(key_tx, &got, &found, &error)) {
      Fail("prefixed tx staged get after batch commit failed: " + error);
    }
    if (!found || got != value_tx) {
      Fail("prefixed tx staged commit mismatch");
    }
    if (!db.Get(grovedb::ColumnFamilyKind::kDefault, {root}, key_tx, &got, &found, &error)) {
      Fail("prefixed tx staged external get before tx commit failed: " + error);
    }
    if (found) {
      Fail("external read should not observe staged tx write before tx commit");
    }
    if (!tx.Commit(&error)) {
      Fail("prefixed tx staged tx commit failed: " + error);
    }
    if (!db.Get(grovedb::ColumnFamilyKind::kDefault, {root}, key_tx, &got, &found, &error)) {
      Fail("prefixed tx staged external get after tx commit failed: " + error);
    }
    if (!found || got != value_tx) {
      Fail("prefixed tx staged persisted value mismatch");
    }
  }

  {
    grovedb::RocksDbWrapper::Transaction tx;
    if (!db.BeginTransaction(&tx, &error)) {
      Fail("begin transaction multi-batch merge failed: " + error);
    }
    grovedb::RocksDbWrapper::WriteBatch shared_batch;
    grovedb::RocksDbWrapper::WriteBatch default_part;
    grovedb::RocksDbWrapper::WriteBatch aux_part;
    grovedb::RocksDbWrapper::TransactionPrefixedContext default_part_ctx(
        &tx, grovedb::ColumnFamilyKind::kDefault, {root}, &default_part);
    grovedb::RocksDbWrapper::TransactionPrefixedContext aux_part_ctx(
        &tx, grovedb::ColumnFamilyKind::kAux, {root}, &aux_part);
    grovedb::RocksDbWrapper::TransactionPrefixedContext merge_ctx(
        &tx, grovedb::ColumnFamilyKind::kDefault, {root}, &shared_batch);
    grovedb::RocksDbWrapper::TransactionPrefixedContext read_default_ctx(
        &tx, grovedb::ColumnFamilyKind::kDefault, {root});
    grovedb::RocksDbWrapper::TransactionPrefixedContext read_aux_ctx(
        &tx, grovedb::ColumnFamilyKind::kAux, {root});
    const std::vector<uint8_t> default_key = {'m', 'b', 'd'};
    const std::vector<uint8_t> default_value = {'m', 'b', 'v'};
    const std::vector<uint8_t> aux_key = {'m', 'b', 'a'};
    const std::vector<uint8_t> aux_value = {'m', 'b', 'x'};

    if (!default_part_ctx.Put(default_key, default_value, &error)) {
      Fail("staged default part put failed: " + error);
    }
    if (!aux_part_ctx.Put(aux_key, aux_value, &error)) {
      Fail("staged aux part put failed: " + error);
    }

    if (!read_default_ctx.Get(default_key, &got, &found, &error)) {
      Fail("read default before part merge failed: " + error);
    }
    if (found) {
      Fail("default part should be hidden before merge");
    }
    if (!read_aux_ctx.Get(aux_key, &got, &found, &error)) {
      Fail("read aux before part merge failed: " + error);
    }
    if (found) {
      Fail("aux part should be hidden before merge");
    }

    if (!merge_ctx.CommitBatchPart(default_part, &error)) {
      Fail("merge default part into shared batch failed: " + error);
    }
    if (!merge_ctx.CommitBatchPart(aux_part, &error)) {
      Fail("merge aux part into shared batch failed: " + error);
    }

    if (!read_default_ctx.Get(default_key, &got, &found, &error)) {
      Fail("read default after part merge failed: " + error);
    }
    if (found) {
      Fail("merged parts should stay hidden before shared batch commit");
    }
    if (!read_aux_ctx.Get(aux_key, &got, &found, &error)) {
      Fail("read aux after part merge failed: " + error);
    }
    if (found) {
      Fail("merged aux part should stay hidden before shared batch commit");
    }

    if (!db.CommitBatch(shared_batch, &tx, &error)) {
      Fail("commit shared merged batch failed: " + error);
    }
    if (!read_default_ctx.Get(default_key, &got, &found, &error)) {
      Fail("read default after shared batch commit failed: " + error);
    }
    if (!found || got != default_value) {
      Fail("default merged part should be visible after shared batch commit");
    }
    if (!read_aux_ctx.Get(aux_key, &got, &found, &error)) {
      Fail("read aux after shared batch commit failed: " + error);
    }
    if (!found || got != aux_value) {
      Fail("aux merged part should be visible after shared batch commit");
    }

    if (!db.Get(grovedb::ColumnFamilyKind::kDefault, {root}, default_key, &got, &found, &error)) {
      Fail("external default read before tx commit failed: " + error);
    }
    if (found) {
      Fail("external read should not see merged default part before tx commit");
    }
    if (!db.Get(grovedb::ColumnFamilyKind::kAux, {root}, aux_key, &got, &found, &error)) {
      Fail("external aux read before tx commit failed: " + error);
    }
    if (found) {
      Fail("external read should not see merged aux part before tx commit");
    }

    if (!tx.Commit(&error)) {
      Fail("commit multi-batch merge tx failed: " + error);
    }
    if (!db.Get(grovedb::ColumnFamilyKind::kDefault, {root}, default_key, &got, &found, &error)) {
      Fail("external default read after tx commit failed: " + error);
    }
    if (!found || got != default_value) {
      Fail("external default merged part mismatch");
    }
    if (!db.Get(grovedb::ColumnFamilyKind::kAux, {root}, aux_key, &got, &found, &error)) {
      Fail("external aux read after tx commit failed: " + error);
    }
    if (!found || got != aux_value) {
      Fail("external aux merged part mismatch");
    }
  }

  {
    grovedb::RocksDbWrapper::Transaction tx;
    if (!db.BeginTransaction(&tx, &error)) {
      Fail("begin transaction 2 failed: " + error);
    }
    if (!tx.Delete(grovedb::ColumnFamilyKind::kDefault, {root}, key, &error)) {
      Fail("tx delete failed: " + error);
    }
    if (!tx.Rollback(&error)) {
      Fail("tx rollback failed: " + error);
    }
    if (!db.Get(grovedb::ColumnFamilyKind::kDefault, {root}, key, &got, &found, &error)) {
      Fail("get after rollback failed: " + error);
    }
    if (!found || got != value_a) {
      Fail("rollback should preserve value");
    }
  }

  {
    const std::vector<uint8_t> key_tx1 = {'s', '1'};
    const std::vector<uint8_t> key_tx2 = {'s', '2'};
    const std::vector<uint8_t> value_tx1 = {'v', '1'};
    const std::vector<uint8_t> value_tx2 = {'v', '2'};
    if (!db.Put(grovedb::ColumnFamilyKind::kDefault, {root}, key_tx1, value_tx1, &error)) {
      Fail("put scan seed 1 failed: " + error);
    }
    if (!db.Put(grovedb::ColumnFamilyKind::kDefault, {root}, key_tx2, value_tx2, &error)) {
      Fail("put scan seed 2 failed: " + error);
    }
    grovedb::RocksDbWrapper::Transaction tx;
    if (!db.BeginTransaction(&tx, &error)) {
      Fail("begin transaction scan failed: " + error);
    }
    grovedb::RocksDbWrapper::TransactionPrefixedContext ctx(&tx,
                                                         grovedb::ColumnFamilyKind::kDefault,
                                                         {root});
    const std::vector<uint8_t> key_tx3 = {'s', '3'};
    const std::vector<uint8_t> value_tx3 = {'v', '3'};
    if (!ctx.Put(key_tx3, value_tx3, &error)) {
      Fail("tx scan put failed: " + error);
    }
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> entries;
    if (!ctx.Scan(&entries, &error)) {
      Fail("tx scan failed: " + error);
    }
    bool saw_tx3 = false;
    for (const auto& kv : entries) {
      if (kv.first == key_tx3 && kv.second == value_tx3) {
        saw_tx3 = true;
      }
    }
    if (saw_tx3) {
      Fail("tx scan should not see no-batch prefixed write");
    }
    if (!tx.Put(grovedb::ColumnFamilyKind::kDefault, {root}, key_tx3, value_tx3, &error)) {
      Fail("tx scan direct put failed: " + error);
    }
    entries.clear();
    if (!ctx.Scan(&entries, &error)) {
      Fail("tx scan after direct put failed: " + error);
    }
    saw_tx3 = false;
    for (const auto& kv : entries) {
      if (kv.first == key_tx3 && kv.second == value_tx3) {
        saw_tx3 = true;
      }
    }
    if (!saw_tx3) {
      Fail("tx scan did not see uncommitted direct tx write");
    }
    if (!tx.Commit(&error)) {
      Fail("tx scan commit failed: " + error);
    }
    if (!db.Get(grovedb::ColumnFamilyKind::kDefault, {root}, key_tx3, &got, &found, &error)) {
      Fail("tx scan get after commit failed: " + error);
    }
    if (!found || got != value_tx3) {
      Fail("tx scan commit mismatch");
    }
  }

  {
    grovedb::RocksDbWrapper::Transaction tx;
    if (!db.BeginTransaction(&tx, &error)) {
      Fail("begin transaction delete prefix failed: " + error);
    }
    if (!tx.DeletePrefix(grovedb::ColumnFamilyKind::kDefault, {root}, &error)) {
      Fail("tx delete prefix failed: " + error);
    }
    if (!tx.Rollback(&error)) {
      Fail("tx delete prefix rollback failed: " + error);
    }
    if (!db.Get(grovedb::ColumnFamilyKind::kDefault, {root}, key, &got, &found, &error)) {
      Fail("get after delete prefix rollback failed: " + error);
    }
    if (!found) {
      Fail("delete prefix rollback should preserve data");
    }
  }

  {
    grovedb::RocksDbWrapper::Transaction tx;
    if (!db.BeginTransaction(&tx, &error)) {
      Fail("begin transaction 3 failed: " + error);
    }
    const std::vector<uint8_t> aux_key = {'a', 'x'};
    const std::vector<uint8_t> aux_val = {'a', 'v'};
    const std::vector<uint8_t> root_key = {'r', 'x'};
    const std::vector<uint8_t> root_val = {'r', 'v'};
    const std::vector<uint8_t> meta_key = {'m', 'x'};
    const std::vector<uint8_t> meta_val = {'m', 'v'};
    if (!tx.Put(grovedb::ColumnFamilyKind::kAux, {root}, aux_key, aux_val, &error)) {
      Fail("tx aux put failed: " + error);
    }
    if (!tx.Put(grovedb::ColumnFamilyKind::kRoots, {root}, root_key, root_val, &error)) {
      Fail("tx roots put failed: " + error);
    }
    if (!tx.Put(grovedb::ColumnFamilyKind::kMeta, {root}, meta_key, meta_val, &error)) {
      Fail("tx meta put failed: " + error);
    }
    if (!tx.Commit(&error)) {
      Fail("tx commit 3 failed: " + error);
    }
    if (!db.Get(grovedb::ColumnFamilyKind::kAux, {root}, aux_key, &got, &found, &error)) {
      Fail("get aux after tx failed: " + error);
    }
    if (!found || got != aux_val) {
      Fail("tx aux mismatch");
    }
    if (!db.Get(grovedb::ColumnFamilyKind::kRoots, {root}, root_key, &got, &found, &error)) {
      Fail("get roots after tx failed: " + error);
    }
    if (!found || got != root_val) {
      Fail("tx roots mismatch");
    }
    if (!db.Get(grovedb::ColumnFamilyKind::kMeta, {root}, meta_key, &got, &found, &error)) {
      Fail("get meta after tx failed: " + error);
    }
    if (!found || got != meta_val) {
      Fail("tx meta mismatch");
    }
  }

  {
    grovedb::RocksDbWrapper::WriteBatch batch;
    const std::vector<uint8_t> key2 = {'k', '2'};
    const std::vector<uint8_t> value2 = {'v', '2'};
    batch.Put(grovedb::ColumnFamilyKind::kDefault, {root}, key2, value2);
    batch.Delete(grovedb::ColumnFamilyKind::kDefault, {root}, key);
    if (!db.CommitBatch(batch, &error)) {
      Fail("commit batch failed: " + error);
    }
    if (!db.Get(grovedb::ColumnFamilyKind::kDefault, {root}, key2, &got, &found, &error)) {
      Fail("get batch key failed: " + error);
    }
    if (!found || got != value2) {
      Fail("get batch key mismatch");
    }
    if (!db.Get(grovedb::ColumnFamilyKind::kDefault, {root}, key, &got, &found, &error)) {
      Fail("get deleted key failed: " + error);
    }
    if (found) {
      Fail("expected batch delete to remove key");
    }
  }

  {
    grovedb::RocksDbWrapper::WriteBatch batch;
    const std::vector<uint8_t> key2 = {'c', '1'};
    const std::vector<uint8_t> key3 = {'c', '2'};
    const std::vector<uint8_t> key4 = {'c', '3'};
    const std::vector<uint8_t> value2 = {'c', 'v', '1'};
    const std::vector<uint8_t> value3 = {'c', 'v', '2'};
    if (!db.Put(grovedb::ColumnFamilyKind::kDefault, {root}, key2, value2, &error)) {
      Fail("seed for cost batch failed: " + error);
    }
    if (!db.Put(grovedb::ColumnFamilyKind::kDefault, {root}, key4, value2, &error)) {
      Fail("seed for cost batch delete failed: " + error);
    }
    batch.Put(grovedb::ColumnFamilyKind::kDefault, {root}, key2, value3);
    batch.Put(grovedb::ColumnFamilyKind::kDefault, {root}, key3, value2);
    batch.Delete(grovedb::ColumnFamilyKind::kDefault, {root}, key4);
    grovedb::BatchCost cost{};
    if (!db.CommitBatchWithCost(batch, &cost, &error)) {
      Fail("commit batch with cost failed: " + error);
    }
    uint64_t expected_added = PaidLen(key3.size()) + PaidLen(value2.size());
    uint64_t expected_replaced = PaidLen(value3.size());
    uint64_t expected_removed = PaidLen(key4.size()) + PaidLen(value2.size());
    if (cost.seek_count == 0 || cost.storage_loaded_bytes == 0) {
      Fail("batch cost should track reads");
    }
    if (cost.storage_cost.added_bytes == 0 || cost.storage_cost.replaced_bytes == 0 ||
        cost.storage_cost.removed_bytes.TotalRemovedBytes() == 0) {
      Fail("batch cost should track add/replace/remove");
    }
    if (cost.storage_cost.added_bytes < expected_added ||
        cost.storage_cost.replaced_bytes < expected_replaced ||
        cost.storage_cost.removed_bytes.TotalRemovedBytes() < expected_removed) {
      Fail("batch cost storage bytes too small");
    }
    if (cost.hash_node_calls == 0) {
      Fail("batch cost should track hash node calls");
    }
  }

  {
    // Exact cost invariants for same-key overlay in one batch:
    // put(k1,v1), put(k1,v2), delete(k1) should avoid extra db lookups after first op.
    grovedb::RocksDbWrapper::WriteBatch batch;
    const std::vector<uint8_t> key5 = {'o', '1'};
    const std::vector<uint8_t> value5a = {'o', 'v', '1'};
    const std::vector<uint8_t> value5b = {'o', 'v', '2'};
    batch.Put(grovedb::ColumnFamilyKind::kDefault, {root}, key5, value5a);
    batch.Put(grovedb::ColumnFamilyKind::kDefault, {root}, key5, value5b);
    batch.Delete(grovedb::ColumnFamilyKind::kDefault, {root}, key5);

    grovedb::BatchCost one_op_cost{};
    {
      grovedb::RocksDbWrapper::WriteBatch one_op_batch;
      one_op_batch.Put(grovedb::ColumnFamilyKind::kDefault, {root}, {'h'}, {'1'});
      if (!db.CommitBatchWithCost(one_op_batch, &one_op_cost, &error)) {
        Fail("one-op cost baseline failed: " + error);
      }
    }

    grovedb::BatchCost cost{};
    if (!db.CommitBatchWithCost(batch, &cost, &error)) {
      Fail("overlay batch commit with cost failed: " + error);
    }
    if (cost.seek_count != 4) {
      Fail("overlay batch seek_count mismatch");
    }
    if (cost.storage_loaded_bytes != 0) {
      Fail("overlay batch should not load storage bytes");
    }
    const uint64_t expected_added = PaidLen(key5.size()) + PaidLen(value5a.size());
    const uint64_t expected_replaced = PaidLen(value5b.size());
    const uint64_t expected_removed = PaidLen(key5.size()) + PaidLen(value5b.size());
    if (cost.storage_cost.added_bytes != expected_added) {
      Fail("overlay batch added bytes mismatch");
    }
    if (cost.storage_cost.replaced_bytes != expected_replaced) {
      Fail("overlay batch replaced bytes mismatch");
    }
    if (cost.storage_cost.removed_bytes.TotalRemovedBytes() != expected_removed) {
      Fail("overlay batch removed bytes mismatch");
    }
    if (one_op_cost.hash_node_calls == 0) {
      Fail("one-op hash baseline should be non-zero");
    }
    if (cost.hash_node_calls != one_op_cost.hash_node_calls * 3) {
      Fail("overlay batch hash_node_calls mismatch");
    }

    bool found_overlay = false;
    std::vector<uint8_t> got_overlay;
    if (!db.Get(grovedb::ColumnFamilyKind::kDefault,
                {root},
                key5,
                &got_overlay,
                &found_overlay,
                &error)) {
      Fail("get overlay key failed: " + error);
    }
    if (found_overlay) {
      Fail("overlay key should be deleted");
    }
  }

  {
    // Exact invariants for mixed-key overlay in one batch.
    // put(k1,a), put(k1,b), put(k2,c), delete(k1), put(k2,d)
    grovedb::RocksDbWrapper::WriteBatch batch;
    const std::vector<uint8_t> key7 = {'m', '1'};
    const std::vector<uint8_t> key8 = {'m', '2'};
    const std::vector<uint8_t> value7a = {'v', 'a'};
    const std::vector<uint8_t> value7b = {'v', 'b'};
    const std::vector<uint8_t> value8c = {'v', 'c'};
    const std::vector<uint8_t> value8d = {'v', 'd'};
    batch.Put(grovedb::ColumnFamilyKind::kDefault, {root}, key7, value7a);
    batch.Put(grovedb::ColumnFamilyKind::kDefault, {root}, key7, value7b);
    batch.Put(grovedb::ColumnFamilyKind::kDefault, {root}, key8, value8c);
    batch.Delete(grovedb::ColumnFamilyKind::kDefault, {root}, key7);
    batch.Put(grovedb::ColumnFamilyKind::kDefault, {root}, key8, value8d);

    grovedb::BatchCost one_op_cost{};
    {
      grovedb::RocksDbWrapper::WriteBatch one_op_batch;
      one_op_batch.Put(grovedb::ColumnFamilyKind::kDefault, {root}, {'z'}, {'1'});
      if (!db.CommitBatchWithCost(one_op_batch, &one_op_cost, &error)) {
        Fail("one-op mixed baseline failed: " + error);
      }
    }

    grovedb::BatchCost cost{};
    if (!db.CommitBatchWithCost(batch, &cost, &error)) {
      Fail("mixed overlay batch commit with cost failed: " + error);
    }
    if (cost.seek_count != 7) {
      Fail("mixed overlay batch seek_count mismatch");
    }
    if (cost.storage_loaded_bytes != 0) {
      Fail("mixed overlay batch should not load storage bytes");
    }
    const uint64_t expected_added =
        PaidLen(key7.size()) + PaidLen(value7a.size()) + PaidLen(key8.size()) + PaidLen(value8c.size());
    const uint64_t expected_replaced = PaidLen(value7b.size()) + PaidLen(value8d.size());
    const uint64_t expected_removed = PaidLen(key7.size()) + PaidLen(value7b.size());
    if (cost.storage_cost.added_bytes != expected_added ||
        cost.storage_cost.replaced_bytes != expected_replaced ||
        cost.storage_cost.removed_bytes.TotalRemovedBytes() != expected_removed) {
      Fail("mixed overlay batch storage bytes mismatch");
    }
    if (one_op_cost.hash_node_calls == 0 ||
        cost.hash_node_calls != one_op_cost.hash_node_calls * 5) {
      Fail("mixed overlay batch hash_node_calls mismatch");
    }

    bool found_k1 = false;
    bool found_k2 = false;
    std::vector<uint8_t> got_k1;
    std::vector<uint8_t> got_k2;
    if (!db.Get(grovedb::ColumnFamilyKind::kDefault, {root}, key7, &got_k1, &found_k1, &error)) {
      Fail("get mixed k1 failed: " + error);
    }
    if (!db.Get(grovedb::ColumnFamilyKind::kDefault, {root}, key8, &got_k2, &found_k2, &error)) {
      Fail("get mixed k2 failed: " + error);
    }
    if (found_k1 || !found_k2 || got_k2 != value8d) {
      Fail("mixed overlay final state mismatch");
    }
  }

  {
    grovedb::RocksDbWrapper::WriteBatch batch;
    const std::vector<uint8_t> key2 = {'b', '2'};
    const std::vector<uint8_t> value2 = {'b', 'v'};
    batch.Put(grovedb::ColumnFamilyKind::kDefault, {root}, key2, value2);
    grovedb::RocksDbWrapper::Transaction tx;
    if (!db.BeginTransaction(&tx, &error)) {
      Fail("begin transaction batch failed: " + error);
    }
    if (!db.CommitBatch(batch, &tx, &error)) {
      Fail("tx batch commit failed: " + error);
    }
    if (!tx.Rollback(&error)) {
      Fail("tx batch rollback failed: " + error);
    }
    if (!db.Get(grovedb::ColumnFamilyKind::kDefault, {root}, key2, &got, &found, &error)) {
      Fail("get after tx batch rollback failed: " + error);
    }
    if (found) {
      Fail("tx batch rollback should not persist data");
    }
  }

  {
    // Tx path should report the same cost shape for the same-key overlay case.
    grovedb::RocksDbWrapper::WriteBatch batch;
    const std::vector<uint8_t> key6 = {'o', '2'};
    const std::vector<uint8_t> value6a = {'o', 'w', '1'};
    const std::vector<uint8_t> value6b = {'o', 'w', '2'};
    batch.Put(grovedb::ColumnFamilyKind::kDefault, {root}, key6, value6a);
    batch.Put(grovedb::ColumnFamilyKind::kDefault, {root}, key6, value6b);
    batch.Delete(grovedb::ColumnFamilyKind::kDefault, {root}, key6);

    grovedb::RocksDbWrapper::Transaction tx;
    if (!db.BeginTransaction(&tx, &error)) {
      Fail("begin transaction overlay batch cost failed: " + error);
    }
    grovedb::BatchCost cost{};
    if (!db.CommitBatchWithCost(batch, &tx, &cost, &error)) {
      Fail("tx overlay batch commit with cost failed: " + error);
    }
    if (cost.seek_count != 4 || cost.storage_loaded_bytes != 0) {
      Fail("tx overlay batch cost shape mismatch");
    }
    const uint64_t expected_added = PaidLen(key6.size()) + PaidLen(value6a.size());
    const uint64_t expected_replaced = PaidLen(value6b.size());
    const uint64_t expected_removed = PaidLen(key6.size()) + PaidLen(value6b.size());
    if (cost.storage_cost.added_bytes != expected_added ||
        cost.storage_cost.replaced_bytes != expected_replaced ||
        cost.storage_cost.removed_bytes.TotalRemovedBytes() != expected_removed ||
        cost.hash_node_calls == 0) {
      Fail("tx overlay batch storage cost mismatch");
    }
    if (!tx.Rollback(&error)) {
      Fail("tx overlay batch rollback failed: " + error);
    }
    bool found_overlay = false;
    std::vector<uint8_t> got_overlay;
    if (!db.Get(grovedb::ColumnFamilyKind::kDefault,
                {root},
                key6,
                &got_overlay,
                &found_overlay,
                &error)) {
      Fail("get tx overlay key failed: " + error);
    }
    if (found_overlay) {
      Fail("tx overlay key should be absent after rollback");
    }
  }

  {
    grovedb::RocksDbWrapper::WriteBatch batch;
    const std::vector<uint8_t> key2 = {'t', '2'};
    const std::vector<uint8_t> value2 = {'t', 'v'};
    batch.Put(grovedb::ColumnFamilyKind::kDefault, {root}, key2, value2);
    grovedb::RocksDbWrapper::Transaction tx;
    if (!db.BeginTransaction(&tx, &error)) {
      Fail("begin transaction batch cost failed: " + error);
    }
    grovedb::BatchCost cost{};
    if (!db.CommitBatchWithCost(batch, &tx, &cost, &error)) {
      Fail("tx batch commit with cost failed: " + error);
    }
    if (cost.seek_count == 0 || cost.storage_cost.added_bytes == 0) {
      Fail("tx batch cost missing");
    }
    if (!tx.Rollback(&error)) {
      Fail("tx batch rollback failed: " + error);
    }
    if (!db.Get(grovedb::ColumnFamilyKind::kDefault, {root}, key2, &got, &found, &error)) {
      Fail("get after tx batch rollback failed: " + error);
    }
    if (found) {
      Fail("tx batch rollback should not persist data");
    }
  }

  if (!db.Put(grovedb::ColumnFamilyKind::kRoots, {root}, key, value_b, &error)) {
    Fail("put roots failed: " + error);
  }
  if (!db.Get(grovedb::ColumnFamilyKind::kRoots, {root}, key, &got, &found, &error)) {
    Fail("get roots failed: " + error);
  }
  if (!found || got != value_b) {
    Fail("get roots mismatch");
  }

  {
    // Repeated open/destroy on the same path should not leak column-family
    // handles or make later opens fail.
    auto reopen_now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::string reopen_dir = MakeTempDir("storage_reopen_" + std::to_string(reopen_now));
    {
      grovedb::RocksDbWrapper seed_db;
      if (!seed_db.Open(reopen_dir, &error)) {
        Fail("reopen loop seed open failed: " + error);
      }
      if (!seed_db.Put(grovedb::ColumnFamilyKind::kDefault, {root, child}, key, value_b, &error)) {
        Fail("reopen loop seed put failed: " + error);
      }
    }
    for (int i = 0; i < 3; ++i) {
      grovedb::RocksDbWrapper reopen_db;
      if (!reopen_db.Open(reopen_dir, &error)) {
        Fail("reopen loop open failed: " + error);
      }
      std::vector<uint8_t> reopen_got;
      bool reopen_found = false;
      if (!reopen_db.Get(grovedb::ColumnFamilyKind::kDefault,
                         {root, child},
                         key,
                         &reopen_got,
                         &reopen_found,
                         &error)) {
        Fail("reopen loop get failed: " + error);
      }
      if (!reopen_found || reopen_got != value_b) {
        Fail("reopen loop value mismatch");
      }
    }
    std::error_code reopen_ec;
    std::filesystem::remove_all(reopen_dir, reopen_ec);
  }

  {
    // Destroying a wrapper with a live transaction should not abort during
    // RocksDB teardown.
    auto tx_now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::string tx_dir = MakeTempDir("storage_tx_teardown_" + std::to_string(tx_now));
    {
      grovedb::RocksDbWrapper tx_teardown_db;
      if (!tx_teardown_db.Open(tx_dir, &error)) {
        Fail("tx teardown open failed: " + error);
      }
      grovedb::RocksDbWrapper::Transaction tx;
      if (!tx_teardown_db.BeginTransaction(&tx, &error)) {
        Fail("tx teardown begin failed: " + error);
      }
      if (!tx.Put(grovedb::ColumnFamilyKind::kDefault,
                  {root},
                  {'l', 'i', 'v', 'e'},
                  {'t', 'x'},
                  &error)) {
        Fail("tx teardown put failed: " + error);
      }
    }
    std::error_code tx_ec;
    std::filesystem::remove_all(tx_dir, tx_ec);
  }

  {
    // Touch every column family, flush, checkpoint, and destroy to exercise
    // full handle lifecycle on teardown.
    auto life_now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::string lifecycle_dir = MakeTempDir("storage_lifecycle_" + std::to_string(life_now));
    std::string lifecycle_checkpoint_dir = lifecycle_dir + "_checkpoint";
    std::error_code lifecycle_ec;
    {
      grovedb::RocksDbWrapper lifecycle_db;
      if (!lifecycle_db.Open(lifecycle_dir, &error)) {
        Fail("lifecycle open failed: " + error);
      }
      if (!lifecycle_db.Put(grovedb::ColumnFamilyKind::kDefault, {root}, {'d'}, {'1'}, &error)) {
        Fail("lifecycle put default failed: " + error);
      }
      if (!lifecycle_db.Put(grovedb::ColumnFamilyKind::kAux, {root}, {'a'}, {'2'}, &error)) {
        Fail("lifecycle put aux failed: " + error);
      }
      if (!lifecycle_db.Put(grovedb::ColumnFamilyKind::kRoots, {root}, {'r'}, {'3'}, &error)) {
        Fail("lifecycle put roots failed: " + error);
      }
      if (!lifecycle_db.Put(grovedb::ColumnFamilyKind::kMeta, {root}, {'m'}, {'4'}, &error)) {
        Fail("lifecycle put meta failed: " + error);
      }
      if (!lifecycle_db.Flush(&error)) {
        Fail("lifecycle flush failed: " + error);
      }
      if (!lifecycle_db.CreateCheckpoint(lifecycle_checkpoint_dir, &error)) {
        Fail("lifecycle checkpoint failed: " + error);
      }
    }
    {
      grovedb::RocksDbWrapper reopened_lifecycle_db;
      if (!reopened_lifecycle_db.Open(lifecycle_dir, &error)) {
        Fail("lifecycle reopen failed: " + error);
      }
      bool lifecycle_found = false;
      std::vector<uint8_t> lifecycle_got;
      if (!reopened_lifecycle_db.Get(grovedb::ColumnFamilyKind::kMeta,
                                     {root},
                                     {'m'},
                                     &lifecycle_got,
                                     &lifecycle_found,
                                     &error)) {
        Fail("lifecycle reopen meta get failed: " + error);
      }
      if (!lifecycle_found || lifecycle_got != std::vector<uint8_t>({'4'})) {
        Fail("lifecycle reopen meta value mismatch");
      }
    }
    std::filesystem::remove_all(lifecycle_dir, lifecycle_ec);
    std::filesystem::remove_all(lifecycle_checkpoint_dir, lifecycle_ec);
  }

  bool deleted = false;
  if (!db.Delete(grovedb::ColumnFamilyKind::kDefault, {root}, key, &deleted, &error)) {
    Fail("delete failed: " + error);
  }
  if (!deleted) {
    Fail("expected delete to report true");
  }
  if (!db.Get(grovedb::ColumnFamilyKind::kDefault, {root}, key, &got, &found, &error)) {
    Fail("get after delete failed: " + error);
  }
  if (found) {
    Fail("expected key to be deleted");
  }

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::remove_all(dir + "_checkpoint", ec);
  std::filesystem::remove_all(dir + "_checkpoint_2", ec);
  return 0;
}
