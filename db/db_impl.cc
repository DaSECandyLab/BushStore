// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include <cmath>
#include <iostream>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>
#include <climits>

#include "db/db_impl.h"
#include "db/builder.h"
#include "db/db_iter.h"
#include "db/dbformat.h"
#include "db/filename.h"
#include "db/log_reader.h"
#include "db/log_writer.h"
#include "db/memtable.h"
#include "db/table_cache.h"
#include "db/version_set.h"
#include "db/write_batch_internal.h"
#include "leveldb/db.h"
#include "leveldb/env.h"
#include "leveldb/status.h"
#include "leveldb/table.h"
#include "leveldb/table_builder.h"
#include "port/port.h"
#include "table/block.h"
#include "table/merger.h"
#include "table/two_level_iterator.h"
#include "table/pm_table_builder.h"
#include "util/bloomfilter.h"
#include "util/coding.h"
#include "util/logging.h"
#include "util/mutexlock.h"
#include "util/global.h"
#include "util/env_pm.h"
#include "bplustree/bp_iterator.h"
#include "bplustree/bp_merge_iterator.h"

// #define CUCKOO_FILTER
// #define MY_DEBUG
// #define DEBUG_PRINT
// #define CAL_TIME
// #define LOG_SYS

namespace leveldb {

const int kNumNonTableCacheFiles = 10;

static WriteMetric writeMetric_;

// Information kept for every waiting writer
struct DBImpl::Writer {
  explicit Writer(port::Mutex* mu)
      : batch(nullptr), sync(false), done(false), cv(mu) {}

  Status status;
  WriteBatch* batch;
  bool sync;
  bool done;
  port::CondVar cv;
};

struct DBImpl::CompactionState {
  // Files produced by compaction
  struct Output {
    uint64_t number;
    uint64_t file_size;
    InternalKey smallest, largest;
  };

  Output* current_output() { return &outputs[outputs.size() - 1]; }

  explicit CompactionState(Compaction* c)
      : compaction(c),
        smallest_snapshot(0),
        outfile(nullptr),
        builder(nullptr),
        total_bytes(0) {}

  Compaction* const compaction;

  // Sequence numbers < smallest_snapshot are not significant since we
  // will never have to service a snapshot below smallest_snapshot.
  // Therefore if we have seen a sequence number S <= smallest_snapshot,
  // we can drop all entries for the same key with sequence numbers < S.
  SequenceNumber smallest_snapshot;

  std::vector<Output> outputs;

  // State kept for output being generated
  WritableFile* outfile;
  TableBuilder* builder;
  uint64_t out_file_number;

  uint64_t total_bytes;
};

// Fix user-supplied options to be reasonable
template <class T, class V>
static void ClipToRange(T* ptr, V minvalue, V maxvalue) {
  if (static_cast<V>(*ptr) > maxvalue) *ptr = maxvalue;
  if (static_cast<V>(*ptr) < minvalue) *ptr = minvalue;
}
Options SanitizeOptions(const std::string& dbname,
                        const InternalKeyComparator* icmp,
                        const InternalFilterPolicy* ipolicy,
                        const Options& src) {
  Options result = src;
  result.comparator = icmp;
  result.filter_policy = (src.filter_policy != nullptr) ? ipolicy : nullptr;
  ClipToRange(&result.max_open_files, 64 + kNumNonTableCacheFiles, 50000);
  ClipToRange(&result.write_buffer_size, 64UL << 10, 1UL << 36);
  ClipToRange(&result.max_file_size, 1 << 20, 1 << 30);
  ClipToRange(&result.block_size, 1 << 10, 4 << 20);
  if (result.info_log == nullptr) {
    // Open a log file in the same directory as the db
    src.env->CreateDir(dbname);  // In case it does not exist
    src.env->RenameFile(InfoLogFileName(dbname), OldInfoLogFileName(dbname));
    Status s = src.env->NewLogger(InfoLogFileName(dbname), &result.info_log);
    if (!s.ok()) {
      // No place suitable for logging
      result.info_log = nullptr;
    }
  }
  if (result.block_cache == nullptr) {
    result.block_cache = NewLRUCache(8 << 20);
  }
  return result;
}

static int TableCacheSize(const Options& sanitized_options) {
  // Reserve ten files or so for other uses and give the rest to TableCache.
  return sanitized_options.max_open_files - kNumNonTableCacheFiles;
}

DBImpl::DBImpl(const Options& raw_options, const std::string& dbname)
    : env_(raw_options.env),
      internal_comparator_(raw_options.comparator),
      internal_filter_policy_(raw_options.filter_policy),
      options_(SanitizeOptions(dbname, &internal_comparator_,
                               &internal_filter_policy_, raw_options)),
      owns_info_log_(options_.info_log != raw_options.info_log),
      owns_cache_(options_.block_cache != raw_options.block_cache),
      dbname_(dbname),
      table_cache_(new TableCache(dbname_, options_, TableCacheSize(options_))),
      db_lock_(nullptr),
      shutting_down_(false),
      background_work_finished_signal_(&mutex_),
      space_signal_(&mutex_),
      pmAlloc_(new PMMemAllocator(options_)),
      mem_(nullptr),
      imm_(nullptr),
      has_imm_(false),
      logfile_(nullptr),
      logfile_number_(0),
      log_(nullptr),
      seed_(0),
      tmp_batch_(new WriteBatch),
      background_compaction_scheduled_(false),
      manual_compaction_(nullptr),
      versions_(new VersionSet(dbname_, &options_, table_cache_,
                               &internal_comparator_)),
      cuckoo_filter_(raw_options.bucket_nums == 0 ? nullptr : new CuckooFilter(raw_options.bucket_nums)),
      usage_(0),
      compactionThread_(&DBImpl::MergeL1, this) {
        maxMemtableSize_ = raw_options.write_buffer_size;
        if(raw_options.dynamic_tree){
          curMemtableSize_ = initMemtableSize;
        }else{
          curMemtableSize_ = maxMemtableSize_;
        }
        compactionThread_.detach();
      }

DBImpl::~DBImpl() {
  // Wait for background work to finish.
  mutex_.Lock();
  shutting_down_.store(true);
  conVar_.notify_all();
  while (background_compaction_scheduled_) {
    background_work_finished_signal_.WaitFor(1);
  }
  mutex_.Unlock();

  if (db_lock_ != nullptr) {
    env_->UnlockFile(db_lock_);
  }

  delete versions_;
  if (mem_ != nullptr) mem_->Unref();
  if (imm_ != nullptr) imm_->Unref();
  delete tmp_batch_;
  delete log_;
  delete logfile_;
  delete table_cache_;
  delete cuckoo_filter_;
  delete pmAlloc_;

  if (owns_info_log_) {
    delete options_.info_log;
  }
  if (owns_cache_) {
    delete options_.block_cache;
  }
  if (EVALUATE_METRIC) {
    std::cout << "Final Write Metric Result: " << std::endl;
    writeMetric_.printMetric();
    // std::cout << "Final Read Metric Result: " << std::endl;
    // readMetric_.printMetric();
  }
}

Status DBImpl::NewDB() {
  VersionEdit new_db;
  new_db.SetComparatorName(user_comparator()->Name());
  new_db.SetLogNumber(0);
  new_db.SetNextFile(10);
  new_db.SetLastSequence(0);
  const std::string manifest = DescriptorFileName(dbname_, 1);
  WritableFile* file;
  Status s;
  // if(options_.has_pm){
  //   file = new PMWritableFileMMap(manifest.c_str());
  // }else{
    s = env_->NewWritableFile(manifest, &file);
  // }
  if (!s.ok()) {
    return s;
  }
  {
    log::Writer log(file);
    std::string record;
    new_db.EncodeTo(&record);
    s = log.AddRecord(record);
    if (s.ok()) {
      s = file->Sync();
    }
    if (s.ok()) {
      s = file->Close();
    }
  }
  delete file;
  if (s.ok()) {
    // Make "CURRENT" file that points to the new manifest file.
    s = SetCurrentFile(env_, dbname_, 1);
  } else {
    env_->RemoveFile(manifest);
  }
  return s;
}

void DBImpl::MaybeIgnoreError(Status* s) const {
  if (s->ok() || options_.paranoid_checks) {
    // No change needed
  } else {
    Log(options_.info_log, "Ignoring error %s", s->ToString().c_str());
    *s = Status::OK();
  }
}

void DBImpl::RemoveObsoleteFiles() {
  mutex_.AssertHeld();

  if (!bg_error_.ok()) {
    // After a background error, we don't know whether a new version may
    // or may not have been committed, so we cannot safely garbage collect.
    return;
  }

  // Make a set of all of the live files
  std::set<uint64_t> live = pending_outputs_;
  versions_->AddLiveFiles(&live);

  std::vector<std::string> filenames;
  env_->GetChildren(dbname_, &filenames);  // Ignoring errors on purpose
  uint64_t number;
  FileType type;
  std::vector<std::string> files_to_delete;
  for (std::string& filename : filenames) {
    if (ParseFileName(filename, &number, &type)) {
      bool keep = true;
      switch (type) {
        case kLogFile:
          keep = ((number >= versions_->LogNumber()) ||
                  (number == versions_->PrevLogNumber()));
          break;
        case kDescriptorFile:
          // Keep my manifest file, and any newer incarnations'
          // (in case there is a race that allows other incarnations)
          keep = (number >= versions_->ManifestFileNumber());
          break;
        case kTableFile:
          keep = (live.find(number) != live.end());
          break;
        case kTempFile:
          // Any temp files that are currently being written to must
          // be recorded in pending_outputs_, which is inserted into "live"
          keep = (live.find(number) != live.end());
          break;
        case kCurrentFile:
        case kDBLockFile:
        case kInfoLogFile:
          keep = true;
          break;
      }

      if (!keep) {
        files_to_delete.push_back(std::move(filename));
        if (type == kTableFile) {
          table_cache_->Evict(number);
        }
        Log(options_.info_log, "Delete type=%d #%lld\n", static_cast<int>(type),
            static_cast<unsigned long long>(number));
      }
    }
  }

  // While deleting all files unblock other threads. All files being deleted
  // have unique names which will not collide with newly created files and
  // are therefore safe to delete while allowing other threads to proceed.
  mutex_.Unlock();
  for (const std::string& filename : files_to_delete) {
    env_->RemoveFile(dbname_ + "/" + filename);
  }
  mutex_.Lock();
}

Status DBImpl::Recover(VersionEdit* edit, bool* save_manifest) {
  mutex_.AssertHeld();

  // Ignore error from CreateDir since the creation of the DB is
  // committed only when the descriptor is created, and this directory
  // may already exist from a previous failed creation attempt.
  env_->CreateDir(dbname_);
assert(db_lock_ == nullptr);
  Status s = env_->LockFile(LockFileName(dbname_), &db_lock_);
  if (!s.ok()) {
    return s;
  }
  if (!env_->FileExists(CurrentFileName(dbname_))) {
    if (options_.create_if_missing) {
      Log(options_.info_log, "Creating (debugTag) DB %s since it was missing.",
          dbname_.c_str());
      s = NewDB();
      if (!s.ok()) {
        return s;
      }
    } else {
      return Status::InvalidArgument(
          dbname_, "does not exist (create_if_missing is false)");
    }
  } else {
    if (options_.error_if_exists) {
      return Status::InvalidArgument(dbname_,
                                     "exists (error_if_exists is true)");
    }
  }

  s = versions_->Recover(save_manifest);
  if (!s.ok()) {
    return s;
  }
  SequenceNumber max_sequence(0);

  // Recover from all newer log files than the ones named in the
  // descriptor (new log files may have been added by the previous
  // incarnation without registering them in the descriptor).
  //
  // Note that PrevLogNumber() is no longer used, but we pay
  // attention to it in case we are recovering a database
  // produced by an older version of leveldb.
  const uint64_t min_log = versions_->LogNumber();
  const uint64_t prev_log = versions_->PrevLogNumber();
  std::vector<std::string> filenames;
  s = env_->GetChildren(dbname_, &filenames);
  if (!s.ok()) {
    return s;
  }
  std::set<uint64_t> expected;
  versions_->AddLiveFiles(&expected);
  uint64_t number;
  FileType type;
  std::vector<uint64_t> logs;
  for (size_t i = 0; i < filenames.size(); i++) {
    if (ParseFileName(filenames[i], &number, &type)) {
      expected.erase(number);
      if (type == kLogFile && ((number >= min_log) || (number == prev_log)))
        logs.push_back(number);
    }
  }
  if (!expected.empty()) {
    char buf[50];
    std::snprintf(buf, sizeof(buf), "%d missing files; e.g.",
                  static_cast<int>(expected.size()));
    return Status::Corruption(buf, TableFileName(dbname_, *(expected.begin())));
  }

  // Recover in the order in which the logs were generated
  std::sort(logs.begin(), logs.end());
  for (size_t i = 0; i < logs.size(); i++) {
    s = RecoverLogFile(logs[i], (i == logs.size() - 1), save_manifest, edit,
                       &max_sequence);
    if (!s.ok()) {
      return s;
    }

    // The previous incarnation may not have written any MANIFEST
    // records after allocating this log number.  So we manually
    // update the file number allocation counter in VersionSet.
    versions_->MarkFileNumberUsed(logs[i]);
  }

  if (versions_->LastSequence() < max_sequence) {
    versions_->SetLastSequence(max_sequence);
  }

  return Status::OK();
}

Status DBImpl::RecoverLogFile(uint64_t log_number, bool last_log,
                              bool* save_manifest, VersionEdit* edit,
                              SequenceNumber* max_sequence) {
  struct LogReporter : public log::Reader::Reporter {
    Env* env;
    Logger* info_log;
    const char* fname;
    Status* status;  // null if options_.paranoid_checks==false
    void Corruption(size_t bytes, const Status& s) override {
      Log(info_log, "%s%s: dropping %d bytes; %s",
          (this->status == nullptr ? "(ignoring error) " : ""), fname,
          static_cast<int>(bytes), s.ToString().c_str());
      if (this->status != nullptr && this->status->ok()) *this->status = s;
    }
  };

  mutex_.AssertHeld();

  // Open the log file
  std::string fname = LogFileName(dbname_, log_number);
  SequentialFile* file;
  Status status;
  // if(LOG_PM){
  //   file = new PMSequentialFile(fname.c_str());
  // }else{
    status = env_->NewSequentialFile(fname, &file);
  // }
  if (!status.ok()) {
    MaybeIgnoreError(&status);
    return status;
  }

  // Create the log reader.
  LogReporter reporter;
  reporter.env = env_;
  reporter.info_log = options_.info_log;
  reporter.fname = fname.c_str();
  reporter.status = (options_.paranoid_checks ? &status : nullptr);
  // We intentionally make log::Reader do checksumming even if
  // paranoid_checks==false so that corruptions cause entire commits
  // to be skipped instead of propagating bad information (like overly
  // large sequence numbers).
  log::Reader reader(file, &reporter, true /*checksum*/, 0 /*initial_offset*/);
  Log(options_.info_log, "Recovering log #%llu",
      (unsigned long long)log_number);

  // Read all the records and add to a memtable
  std::string scratch;
  Slice record;
  WriteBatch batch;
  int compactions = 0;
  MemTable* mem = nullptr;
  while (reader.ReadRecord(&record, &scratch) && status.ok()) {
    if (record.size() < 12) {
      reporter.Corruption(record.size(),
                          Status::Corruption("log record too small"));
      continue;
    }
    WriteBatchInternal::SetContents(&batch, record);

    if (mem == nullptr) {
      mem = new MemTable(internal_comparator_);
      mem->setPMAllocator(pmAlloc_,kv_total_);
      mem->Ref();
    }
    status = WriteBatchInternal::InsertInto(&batch, mem);
    MaybeIgnoreError(&status);
    if (!status.ok()) {
      break;
    }
    const SequenceNumber last_seq = WriteBatchInternal::Sequence(&batch) +
                                    WriteBatchInternal::Count(&batch) - 1;
    if (last_seq > *max_sequence) {
      *max_sequence = last_seq;
    }

    if (mem->ApproximateMemoryUsage() > curMemtableSize_) {
      compactions++;
      *save_manifest = true;
      status = WriteLevel0Table(mem, edit, nullptr);
      mem->Unref();
      mem = nullptr;
      if (!status.ok()) {
        // Reflect errors immediately so that conditions like full
        // file-systems cause the DB::Open() to fail.
        break;
      }
    }
  }

  delete file;

  // See if we should keep reusing the last log file.
  if (status.ok() && options_.reuse_logs && last_log && compactions == 0) {
  assert(logfile_ == nullptr);
  assert(log_ == nullptr);
  assert(mem_ == nullptr);
    uint64_t lfile_size;
    if (env_->GetFileSize(fname, &lfile_size).ok() &&
        env_->NewAppendableFile(fname, &logfile_).ok()) {
      Log(options_.info_log, "Reusing old log %s \n", fname.c_str());
      log_ = new log::Writer(logfile_, lfile_size);
      logfile_number_ = log_number;
      if (mem != nullptr) {
        mem_ = mem;
        mem = nullptr;
      } else {
        // mem can be nullptr if lognum exists but was empty.
        mem_ = new MemTable(internal_comparator_);
        mem_->setPMAllocator(pmAlloc_,kv_total_);
        mem_->Ref();
      }
    }
  }

  if (mem != nullptr) {
    // mem did not get reused; compact it.
    if (status.ok()) {
      *save_manifest = true;
      status = WriteLevel0Table(mem, edit, nullptr);
    }
    mem->Unref();
  }

  return status;
}
Status DBImpl::WriteLevel0TableToPM(MemTable* mem){
  mutex_.AssertHeld();
  uint64_t imm_micros = 0;  // Micros spent doing imm_ compactions
  uint64_t start_micros = 0;
  int level = 0;
  if (TIME_ANALYSIS){
    start_micros = env_->NowMicros();
  }
  MemTableIterator* iter = (MemTableIterator*)mem->NewIterator();
  uint32_t currentFileNumber = -1;
  if(CUCKOO_FILTER){
    mutex_l0_.lock();
    currentFileNumber = cuckoo_filter_->minFileNumber + Table_L0_.size();
    mutex_l0_.unlock();
  }
  mutex_.Unlock();
  // TODO
  uint64_t kvCount = mem->kvCount();
  int page_count = ((kvCount / LEAF_KEY_NUM) + 1) / (NON_LEAF_KEY_NUM - 4) + 7;
  char* node_mem;
  if(TEST_BPTREE_NVM){
    node_mem = (char*)pmAlloc_->PmAlloc(256 * page_count);
  }else{
    node_mem = (char*)calloc(1, 256 * page_count);
  }

  std::shared_ptr<BloomFilterPolicy> filter;
  if(BLOOM_FILTER){
    filter = std::make_shared<BloomFilterPolicy>();
    filter->CreateFilter(kvCount);
  };

  PMTableBuilder builder(pmAlloc_, node_mem, kvCount);
  iter->SeekToFirst();
  int count = 0;
  if(!KV_SEPERATE){
    iter->setAlloc(pmAlloc_);
  }
  bool filter_valid = cuckoo_filter_->isValid();
  if(iter->Valid()){
    Slice key;
    builder.setMinKey(iter->key());
    for (; iter->Valid(); iter->Next()) {
      key = iter->key();
      uint64_t curKey = DecodeDBBenchFixed64(key.data());
      //assert(curKey == count);
      count++;
      if(KV_SEPERATE){
        auto [pointer, index] = iter->valuePointer();
        assert(index >= 4);
        // auto check = [&](uint16_t index, uint32_t pointer){
        //     vPage* addr = (vPage*)getAbsoluteAddr(((uint64_t)pointer) << 12);
        //   assert(addr->v(index).size() == 4096);
        // };
        // check(index, pointer);
        builder.add(key, pointer, index);
        // pb->add(key, iter->value());
      }else{
        builder.add(key, iter->value());
        iter->ClearValue();
      }
      kv_total_.fetch_add(KEY_SIZE, std::memory_order_relaxed);
      if (CUCKOO_FILTER && filter_valid) {
        // cuckoo_filter_->Put(ExtractUserKey(key), currentFileNumber);
        cuckoo_filter_->PutFirst0AndMin(ExtractUserKey(key), currentFileNumber);
      }
      if (BLOOM_FILTER){
        filter->insertKey(key.ToString(8));
      }
      //cuckoo_filter->Put(ExtractUserKey(key), meta->number);
    }
  assert(builder.cur_node_index_ <= page_count);
    builder.setMaxKey(key);
    std::shared_ptr<lbtree> tree = nullptr;
    builder.finish(tree);
    if (CUCKOO_FILTER) {
      tree->fileNumber = currentFileNumber;
    }
    mutex_l0_.lock();
    Table_L0_.push_back(tree);
    if(BLOOM_FILTER){
      bloom_filters_.push_back(filter);
    }
    mutex_l0_.unlock();

    if(DEBUG_CHECK){
      tree->checkIterator();
    }
  }
  mutex_.Lock();

  delete iter;
  if (TIME_ANALYSIS) {
    imm_micros = env_->NowMicros() - start_micros;
    Log(options_.info_log, "flush at level %d, used time : %ld", level,
        imm_micros / 1000);
    lastCompactL0Time_ = imm_micros / 1000;
  }
  CompactionStats stats;
  stats.micros = imm_micros;
  stats.bytes_written += builder.getWriteByte();
  stats_[level].Add(stats);
  if(WRITE_TIME_ANALYSIS){
    writeStats_.writeL0Count++;
    writeStats_.writeL0Time+=imm_micros;
  }
  if (DEBUG_PRINT) {
    double usage1 = pmAlloc_->getMemoryUsabe();
    std::cout << "flush at level 0, cost time : " << imm_micros / 1000;
    std::cout << "ms memoryUsage: " << usage1 << " flush count: " << count << std::endl;
  }
  return Status::OK();
}

Status DBImpl::WriteLevel0Table(MemTable* mem, VersionEdit* edit,
                                Version* base) {
  mutex_.AssertHeld();
  const uint64_t start_micros = env_->NowMicros();
  FileMetaData meta;
  meta.number = versions_->NewFileNumber();
  pending_outputs_.insert(meta.number);
  Iterator* iter = mem->NewIterator();
  Log(options_.info_log, "Level-0 table #%llu: started",
      (unsigned long long)meta.number);

  Status s;
  {
    mutex_.Unlock();
    s = BuildTable(dbname_, env_, options_, table_cache_, iter, &meta, cuckoo_filter_);
    mutex_.Lock();
  }

  Log(options_.info_log, "Level-0 table #%llu: %lld bytes %s",
      (unsigned long long)meta.number, (unsigned long long)meta.file_size,
      s.ToString().c_str());
  delete iter;
  pending_outputs_.erase(meta.number);

  // Note that if file_size is zero, the file has been deleted and
  // should not be added to the manifest.
  int level = 0;
  if (s.ok() && meta.file_size > 0) {
    const Slice min_user_key = meta.smallest.user_key();
    const Slice max_user_key = meta.largest.user_key();
    if (base != nullptr) {
      //level = base->PickLevelForMemTableOutput(min_user_key, max_user_key);
      level = 0;
    }
    edit->AddFile(level, meta.number, meta.file_size, meta.smallest,
                  meta.largest);
  }

  CompactionStats stats;
  stats.micros = env_->NowMicros() - start_micros;
  stats.bytes_written = meta.file_size;
  stats_[level].Add(stats);
  return s;
}

void DBImpl::CompactMemTable() {
  mutex_.AssertHeld();
assert(imm_ != nullptr);

  // Save the contents of the memtable as a new Table
  VersionEdit edit;
  Version* base = versions_->current();
  base->Ref();
  Status s;
  // if(options_.use_pm_){

  if(!TEST_FLUSH_SSD){
    s = WriteLevel0TableToPM(imm_);
  }else{
    // std::cout<<"flush ssd"<<std::endl;
    s = WriteLevel0Table(imm_, &edit, base);
  }
    // std::cout << "flush imm" << std::endl;
  // }else{
  //   s = WriteLevel0Table(imm_, &edit, base);
  // }
  base->Unref();

  if (s.ok() && shutting_down_.load(std::memory_order_acquire)) {
    s = Status::IOError("Deleting DB during memtable compaction");
  }

  // Replace immutable memtable with the generated Table
  if (s.ok()) {
    edit.SetPrevLogNumber(0);
    edit.SetLogNumber(logfile_number_);  // Earlier logs no longer needed
    s = versions_->LogAndApply(&edit, &mutex_);
  }

  if (s.ok()) {
    // Commit to the new state
    imm_->Unref();
    imm_ = nullptr;
    has_imm_.store(false, std::memory_order_release);
    RemoveObsoleteFiles();
  } else {
    RecordBackgroundError(s);
  }
}


void DBImpl::CompactRange(const Slice* begin, const Slice* end) {
  int max_level_with_files = 1;
  {
    MutexLock l(&mutex_);
    Version* base = versions_->current();
    for (int level = 1; level < config::kNumLevels; level++) {
      if (base->OverlapInLevel(level, begin, end)) {
        max_level_with_files = level;
      }
    }
  }
  TEST_CompactMemTable();  // TODO(sanjay): Skip if memtable does not overlap
  for (int level = 0; level < max_level_with_files; level++) {
    TEST_CompactRange(level, begin, end);
  }
}

void DBImpl::TEST_CompactRange(int level, const Slice* begin,
                               const Slice* end) {
assert(level >= 0);
assert(level + 1 < config::kNumLevels);

  InternalKey begin_storage, end_storage;

  ManualCompaction manual;
  manual.level = level;
  manual.done = false;
  if (begin == nullptr) {
    manual.begin = nullptr;
  } else {
    begin_storage = InternalKey(*begin, kMaxSequenceNumber, kValueTypeForSeek);
    manual.begin = &begin_storage;
  }
  if (end == nullptr) {
    manual.end = nullptr;
  } else {
    end_storage = InternalKey(*end, 0, static_cast<ValueType>(0));
    manual.end = &end_storage;
  }

  MutexLock l(&mutex_);
  while (!manual.done && !shutting_down_.load(std::memory_order_acquire) &&
         bg_error_.ok()) {
    if (manual_compaction_ == nullptr) {  // Idle
      manual_compaction_ = &manual;
      MaybeScheduleCompaction();
    } else {  // Running either my compaction or another compaction.
      background_work_finished_signal_.Wait();
    }
  }
  if (manual_compaction_ == &manual) {
    // Cancel my manual compaction since we aborted early for some reason.
    manual_compaction_ = nullptr;
  }
}

Status DBImpl::TEST_CompactMemTable() {
  // nullptr batch means just wait for earlier writes to be done
  Status s = Write(WriteOptions(), nullptr);
  if (s.ok()) {
    // Wait until the compaction completes
    MutexLock l(&mutex_);
    while (imm_ != nullptr && bg_error_.ok()) {
      background_work_finished_signal_.Wait();
    }
    if (imm_ != nullptr) {
      s = bg_error_;
    }
  }
  return s;
}

void DBImpl::RecordBackgroundError(const Status& s) {
  mutex_.AssertHeld();
  if (bg_error_.ok()) {
    bg_error_ = s;
    background_work_finished_signal_.SignalAll();
  }
}

void DBImpl::MaybeScheduleCompaction() {
  mutex_.AssertHeld();
  if (background_compaction_scheduled_) {
    // Already scheduled
  } else if (shutting_down_.load(std::memory_order_acquire)) {
    // DB is being deleted; no more background compactions
  } else if (!bg_error_.ok()) {
    // Already got an error; no more changes
  } else if (imm_ == nullptr
             && manual_compaction_ == nullptr &&
             !versions_->NeedsCompaction()) {
    // No work to be done
  } else {
    background_compaction_scheduled_ = true;
    env_->Schedule(&DBImpl::BGWork, this);
  }
}

void DBImpl::BGWork(void* db) {
  reinterpret_cast<DBImpl*>(db)->BackgroundCall();
}

void DBImpl::BackgroundCall() {
  std::chrono::milliseconds duration(1);    std::this_thread::sleep_for(duration);    // if (shutting_down_.load(std::memory_order_acquire)) {
  //   // No more background work when shutting down.
  //   background_compaction_scheduled_ = false;
  //   background_work_finished_signal_.SignalAll();
  //   // Previous compaction may have produced too many files in a level,
  //   // so reschedule another compaction if needed.
  //   return;
  // }
  usage_ = pmAlloc_->getMemoryUsabe() * 100;
  if(usage_ < 95){
    space_signal_.SignalAll();
  }
  MutexLock l(&mutex_);
assert(background_compaction_scheduled_);
  if (shutting_down_.load(std::memory_order_acquire)) {
    // No more background work when shutting down.
  } else if (!bg_error_.ok()) {
    // No more background work after a background error.
  } else {
    BackgroundCompaction();
  }

  background_compaction_scheduled_ = false;

  // Previous compaction may have produced too many files in a level,
  // so reschedule another compaction if needed.
  MaybeScheduleCompaction();
}

void DBImpl::checkAndSetGC(){
  // double usage = pmAlloc_->getMemoryUsabe();
  // if(needGC && usage > 0.7){
  //   flushSSD = true;
  // }
  // if(usage > 0.85) {
  //   needGC = true;
  // }else if(usage < 0.75){
  //   needGC = false;
  // }

// #ifdef DEBUG_PRINT
//   printf("memoryUsage : %.2lf\n", usage);
// #endif
  // needGC = true;
}

bool DBImpl::isNeedGC(){
  return needGC || !KV_SEPERATE;
}

void DBImpl::Flush(){
  // std::cout<<"flush"<<std::endl;;
  Status s = Write(WriteOptions(), nullptr);
  //   std::cout<<"flush1"<<std::endl;;
  // CompactMemTable();
  // std::cout<<"flushed"<<std::endl;;
  sleep(5);
}

int DBImpl::PickCompactionPM(){
  double usage_rate = pmAlloc_->getMemoryUsabe();
  usage_ = usage_rate * 100;
  valid_rate_ = 1.0 * kv_total_ / (usage_rate * options_.pm_size_);
  int level = -1;
  
  mutex_l0_.lock();
  l0_num_ = Table_L0_.size();
  mutex_l0_.unlock();

  
  
  if(l0_num_ >= MinCompactionL0Count){
    level = 0;
    if(valid_rate_ < options_.gc_ratio){
        needGC = true;
    }
    if(stop_gc_count_ > 0){
      stop_gc_count_--;
        // printf("stop_gc_count_:%d\n", stop_gc_count_);
      needGC = false;
    }
  }
  // else if(!CUCKOO_FILTER && l0_num_ >= MinCompactionL0Count){
  //   level = 0;
  //   if(valid_rate_ < options_.gc_ratio){
  //     needGC = true;
  //   }
  // }
  if(usage_ > memory_rate * 100){
    level = 1;
    flushSSD = true;
  }
  // if(options_.flush_ssd){
  //   if(usage_ > memory_rate){
  //     level = 1;
  //     flushSSD = true;
  //   }
  // }else if(usage_ > memory_rate){
  //   level = 0;
  //   needGC = true;
  //   // printf("need GC!!\n");
  // }

  return level;
}

// void tryDeleteTable(std::vector<lbtree*>& table){
//   std::vector<lbtree*> newTable;
//   for(int i = 0; i < table.size(); i++){
//     //     int expected = 0;
//     if(table[i]->reader_count.compare_exchange_weak(expected, INT_MIN)){
//       free(table[i]->tree_meta->addr);
//       delete table[i];
//     }else{
//       newTable.push_back(table[i]);
//     }
//   }
//   std::swap(table, newTable);
// }

Status DBImpl::CompactionLevel0(){
          // finished
  uint64_t imm_micros = 0;  // Micros spent doing imm_ compactions
  uint64_t start_micros = 0;
  int64_t KeyIn = 0;
  int64_t KeyDrop = 0;
  int level = 1;
  // checkAndSetGC();
    int mergeSize;
  // if(compactL0Count_.load() < Table_L0_.size()){
  //   mergeSize = compactL0Count_.load();
  // }else{
    mergeSize = Table_L0_.size();
  // }
  if(TEST_CUCKOOFILTER){
    mergeSize -= 7;
  }
  mutex_l0_.lock();
  std::vector<std::shared_ptr<lbtree>> Table_L0_Merge(Table_L0_.begin(), Table_L0_.begin() + mergeSize);
  mutex_l0_.unlock();

  // std::sort(Table_L0_Sorted.begin(), Table_L0_Sorted.end(), [](lbtree* tree1,
  // lbtree* tree2){
  //   if(tree1->tree_meta->min_key == tree2->tree_meta->min_key){
  //     return tree1->tree_meta->max_key < tree2->tree_meta->max_key;
  //   }
  //   return tree1->tree_meta->min_key < tree2->tree_meta->min_key;
  // });

  // if(Table_L0_Sorted.empty()){

  // }
  // std::vector<lbtree*> Table_L0_Merge = Table_L0_Sorted;
  std::reverse(Table_L0_Merge.begin(), Table_L0_Merge.end());
  // if(Table_L0_Sorted.empty()){
  // assert(false);
  //   return Status::OK();
  // }

      // Table_L0_Merge.push_back(Table_L0_Sorted[0]);
  // key_type maxKey0 = Table_L0_Sorted.front()->tree_meta->max_key;
  // for(int i = 1; i < Table_L0_Sorted.size(); i++){
  //   lbtree* tree1 = Table_L0_Merge.back();
  //   maxKey0 = std::max(maxKey0, tree1->tree_meta->max_key);
  //   lbtree* tree2 = Table_L0_Sorted[i];
  //   if(maxKey0 < tree2->tree_meta->min_key){
  //     break;
  //   }else{
  //     Table_L0_Merge.push_back(tree2);
  //   }
  // }
  int max_count = Table_L0_Merge.size();
  assert(max_count != 0);
  if (max_count == 0) {
    return Status::OK();
  }
  key_type maxKey = Table_L0_Merge[0]->tree_meta->max_key;
  std::vector<IteratorBTree*> its;  // need delete
  key_type minKey = Table_L0_Merge[0]->tree_meta->min_key;
  key_type tree2_start;
  key_type tree2_end;
  kPage* kBegin = nullptr;
  kPage* kEnd = nullptr;
  for (int i = 0; i < max_count; i++) {
    maxKey = std::max(maxKey, Table_L0_Merge[i]->tree_meta->max_key);
    minKey = std::min(minKey, Table_L0_Merge[i]->tree_meta->min_key);
    its.push_back(new BP_Iterator_Read(Table_L0_Merge[i], pmAlloc_, false));
    // its.push_back(new BP_Iterator(pmAlloc_, Table_L0_Merge[i].get(),
    //                               Table_L0_Merge[i]->tree_meta->pages, 1,
    //                               Table_L0_Merge[i]->tree_meta->kPage_count));
  }
  std::shared_ptr<lbtree> tree2 = nullptr;
  std::vector<std::vector<void*>> pages2;
    if (!Table_LN_.empty()) {
    tree2 = Table_LN_[0];
    int index_start_pos2 = 0;
    int output_page_count = 0;
        pages2 =
        tree2->getOverlapping(minKey, maxKey, &index_start_pos2,
                              &output_page_count, &tree2_start, &tree2_end, kBegin, kEnd);
    if(kEnd != nullptr && kEnd->maxRawKey() < maxKey){
      kEnd = kEnd->nextPage();
    }
    // std::vector<std::vector<bnode*>> pages3;
    // BP_Iterator* it2 = new BP_Iterator(pmAlloc_, tree2.get(), pages2[1],
    //                                    index_start_pos2, output_page_count);
    BP_Iterator_Trim *it2 = new BP_Iterator_Trim(pmAlloc_, tree2.get(), pages2[1], 0);
    it2->setKeyStartAndEnd(minKey, maxKey + 1, user_comparator());
    if(maxKey == UINT64_MAX){
      it2->setEndEmpty();
    }
    its.push_back(it2);
  } else {
  }
  BP_Merge_Iterator* input = new BP_Merge_Iterator(its, user_comparator());
  input->SeekToFirst();

  if (TIME_ANALYSIS){
    start_micros = env_->NowMicros();
  }
  PMTableBuilder builder(pmAlloc_);
  int new_rewrite_gc_page_count = MAX_GC_VPAGE;
  uint64_t max_rewrite_key = 0;
  ParsedInternalKey ikey;
  std::string current_user_key;
  bool has_current_user_key = false;
  SequenceNumber last_sequence_for_key = kMaxSequenceNumber;
  if (input->Valid()) {
    builder.setMinKey(input->key());
  }
  while (input->Valid() && !shutting_down_.load(std::memory_order_acquire)) {
    Slice key = input->key();
    bool drop = false;
    KeyIn++;

    if (!ParseInternalKey(key, &ikey)) {
      // Do not hide error keys
      current_user_key.clear();
      has_current_user_key = false;
      last_sequence_for_key = kMaxSequenceNumber;
    } else {
      if (!has_current_user_key ||
          user_comparator()->Compare(ikey.user_key, Slice(current_user_key)) !=
              0) {
        // First occurrence of this user key
        current_user_key.assign(ikey.user_key.data(), ikey.user_key.size());
        has_current_user_key = true;
        last_sequence_for_key = kMaxSequenceNumber;
      } else {
        drop = true;
      }
      // if (last_sequence_for_key <= compact->smallest_snapshot) {
      //   // Hidden by an newer entry for same user key
      //   drop = true;  // (A)
      // }
      last_sequence_for_key = ikey.sequence;
    }

    if (TEST_CUCKOO_DELETE) {
        // cuckoo_filter_->Put(ExtractUserKey(key), currentFileNumber);
        cuckoo_filter_->Delete(ExtractUserKey(key));
    }
    if (!drop) {
      key_type key64 = DecodeDBBenchFixed64(key.data());
      if (!KV_SEPERATE || (needGC && key64 >= gc_start_key_ && new_rewrite_gc_page_count > 0)) {
          max_rewrite_key = std::max(key64, max_rewrite_key);

      // if (input->getCapacityUsage() < usage_ / 2) {
      // if (isNeedGC() || input->getCapacityUsage() < usage_ / 2) {
        if(builder.add(key, input->value(), input->finger())){
          new_rewrite_gc_page_count--;
        }
        input->clrValue();
      } else {
                // auto check = [&](uint16_t index, uint32_t pointer, Slice& key){
        //   vPage* addr = (vPage*)getAbsoluteAddr(((uint64_t)pointer) << 12);
        // assert(addr->v(index).size() == 1000);
        // };
        // check(input->index(), input->pointer(), key);
        builder.add(key, input->finger(), input->pointer(), input->index());
      }
    } else {
      kv_total_.fetch_sub(input->value().size() + KEY_SIZE, std::memory_order_relaxed);
      input->clrValue();
      KeyDrop++;
    }
    input->Next();
  }
  if(KV_SEPERATE && needGC){
    if(new_rewrite_gc_page_count > 0){
      gc_start_key_ = 0;
    }else{
      gc_start_key_ = max_rewrite_key + 1;
    }
  }
  if (TIME_ANALYSIS) {
    imm_micros = env_->NowMicros() - start_micros;
  }
  std::shared_ptr<lbtree> tree = nullptr;
    auto [pages3, firstPage, lastPage] = builder.finish(tree);

      {
    std::lock_guard<std::mutex> lock(mutex_l1_);
        if (tree2 != nullptr &&
        tree->tree_meta->min_key <= tree2->tree_meta->min_key &&
        tree2->tree_meta->max_key <= tree->tree_meta->max_key) {
      Table_LN_[0]->needFreeNodePages = pages2;
      Table_LN_[0] = tree;
    } else if (tree2 != nullptr) {
      tree2->tree_meta->kPage_count += (pages3[0].size() - pages2[0].size());
      tree2->rangeReplace(pages2, pages3, tree->tree_meta->min_key,
                          tree->tree_meta->max_key);
      // tree2->rangeReplace(pages2, pages3, tree2_start, tree2_end);
      tree2->tree_meta->min_key = std::min(tree2->tree_meta->min_key, minKey);
      tree2->tree_meta->max_key = std::max(tree2->tree_meta->max_key, maxKey);
    } else {
      Table_LN_.push_back(tree);
    }
    if(kBegin != nullptr){
      assert(kBegin->maxRawKey() < firstPage->minRawKey());
      kBegin->setNext(firstPage);
    }
    if(kEnd != nullptr){
      assert(lastPage->maxRawKey() < kEnd->minRawKey());
      lastPage->setNext(kEnd);
    }
  }
    if (tree2 != nullptr) {
    ((BP_Iterator_Trim*)its.back())->releaseKVpage();
    delete its.back();
    its.pop_back();
  }
  for (int i = 0; i < its.size(); i++) {
    its[i]->movePage();
    delete its[i];
  }
  delete input;

  if (DEBUG_CHECK) {
    Table_LN_[0]->reverseAndCheck();
    Table_LN_[0]->checkIterator();
  }
    // 1. delete L0 table
  mutex_l0_.lock();
  if(CUCKOO_FILTER){
    cuckoo_filter_->minFileNumber += Table_L0_Merge.size();
  }
  Table_L0_.erase(Table_L0_.begin(), Table_L0_.begin() + Table_L0_Merge.size());
  if(BLOOM_FILTER){
    bloom_filters_.erase(bloom_filters_.begin(), bloom_filters_.begin() + Table_L0_Merge.size());
  }
  mutex_l0_.unlock();
  int mergeCount = 0;
  if (TIME_ANALYSIS) {
    // imm_micros = env_->NowMicros() - start_micros;
    Log(options_.info_log, "compaction at level %d, used time : %ld, mergeCount : %zu", level,
        imm_micros / 1000, Table_L0_Merge.size());
    lastCompactL1Time_ = imm_micros / 1000;
    if(lastCompactL0Time_ != 0 && lastCompactL1Time_ != 0 && options_.dynamic_tree){
      mergeCount = lastCompactL1Time_ / lastCompactL0Time_;
      if(mergeCount >= minMergeCount && mergeCount <= maxMergeCount){
        compactL0Count_ = mergeCount;
      }if(mergeCount > maxMergeCount && curMemtableSize_ < maxMemtableSize_){
        compactL0Count_ = maxMergeCount;
        if(options_.flush_ssd){
          curMemtableSize_ += addMemtableSize / 16;
        }else{
          curMemtableSize_ += addMemtableSize * (mergeCount / maxMergeCount);
        }
        if(curMemtableSize_ > maxMemtableSize_){
          curMemtableSize_ = maxMemtableSize_;
        }
      } else if (mergeCount >L0BufferCount && curMemtableSize_ >= maxMemtableSize_) {
        ratio_L0_ = std::max(1, mergeCount / mergeSize / 6);
      }
    }
  }
  CompactionStats stats;
  stats.micros = imm_micros;
  stats.bytes_written += builder.getWriteByte();
  stats.KeyIn = KeyIn;
  stats.KeyDrop = KeyDrop;
  stats_[level].Add(stats);
  if(WRITE_TIME_ANALYSIS){
    writeStats_.writeL1Count++;
    writeStats_.writeL1Time+=imm_micros;
  }
  double usage1 = pmAlloc_->getMemoryUsabe();
  usage_ = usage1 * 100;
  double new_valid_rate_ = 1.0 * kv_total_ / (usage1 * options_.pm_size_);
  if (DEBUG_PRINT) {
    std::cout<< "compaction at level 0, cost time : "<< imm_micros / 1000;
    std::cout<< " gc_start_key: "<< gc_start_key_;
    std::cout<< " rewrite vpage: "<< MAX_GC_VPAGE - new_rewrite_gc_page_count;
    std::cout<< " memoryUsage: "<<usage1;
    std::cout<< " memoryUsedData: "<<usage1 * options_.pm_size_ / 1024 / 1024;
    std::cout<< "MB validData : "<<kv_total_ / 1024 / 1024;
    std::cout<< "MB validRate : "<<valid_rate_;
    std::cout<< " mergeCount : "<<Table_L0_Merge.size();
    std::cout<< " nextMergeCount : "<<mergeCount;
    std::cout<< " curMemtableSize : "<<curMemtableSize_ / 1024 / 1024;
    std::cout<< "MB ratioCount : " << ratio_L0_;
    std::cout<< " min key : " << tree->tree_meta->min_key << " max key : "<< tree->tree_meta->max_key<< std::endl;
  }
  
  if(needGC && kv_total_ < STOP_THRESHOLD && (new_valid_rate_ - valid_rate_ < STOP_GC_RATE)){
    stop_gc_count_ = STOP_GC_COUNT;
    if(DEBUG_PRINT){
      printf("stop gc before valid_rate %lf, new valid rate %lf\n", valid_rate_, new_valid_rate_);
    }
  }
  // // 2. release tree
  // for(int i = 0; i < max_count; i++){
  //     //   int expected = 0;
  //   if(Table_L0_Merge[i]->reader_count.compare_exchange_weak(expected,
  //   INT_MIN)){
  //     delete Table_L0_Merge[i];
  //   }else{
  //   assert(expected > 0);
  //     Table_Delete_.push_back(Table_L0_Merge[i]);
  //   }
  // }
  
  //   int index_start_pos1 = 0;
  //   int index_start_pos2 = 0;
  //   key_type tree1_start;
  //   key_type tree1_end;
  //   key_type tree2_start;
  //   key_type tree2_end;
  //   lbtree* tree1 = Table_LN_[level - 1];
  //   lbtree* tree2 = Table_LN_[level];
  //   int input_page_count = 20;   //   int output_page_count = 0;
  //     //   std::vector<std::vector<void *>> pages1 =
  //   tree1->pickInput(input_page_count, &index_start_pos1, &tree1_start,
  //   &tree1_end);
  //   //
    //   std::vector<std::vector<void *>> pages2 =
  //   tree2->getOverlapping(tree1_start, tree1_end, &index_start_pos2,
  //   &output_page_count, &tree2_start, &tree2_end); BP_Iterator* it1 = new
  //   BP_Iterator(pmAlloc_, tree1, pages1[1], index_start_pos1,
  //   input_page_count); BP_Iterator* it2 = new BP_Iterator(pmAlloc_, tree2,
  //   pages2[1], index_start_pos2, output_page_count);
  //   std::vector<BP_Iterator*> its = {it1, it2};
  //   BP_Merge_Iterator* input = new BP_Merge_Iterator(its, user_comparator());

  //   PMTableBuilder builder(pmAlloc_);
  //   ParsedInternalKey ikey;
  //   std::string current_user_key;
  //   bool has_current_user_key = false;
  //   SequenceNumber last_sequence_for_key = kMaxSequenceNumber;
  //   while(input->Valid() && !shutting_down_.load(std::memory_order_acquire)){
  //     Slice key = input->key();
  //     bool drop = false;

  //     if (!ParseInternalKey(key, &ikey)) {
  //       // Do not hide error keys
  //       current_user_key.clear();
  //       has_current_user_key = false;
  //       last_sequence_for_key = kMaxSequenceNumber;
  //     } else {
  //       if (!has_current_user_key ||
  //           user_comparator()->Compare(ikey.user_key,
  //           Slice(current_user_key)) !=
  //               0) {
  //         // First occurrence of this user key
  //         current_user_key.assign(ikey.user_key.data(),
  //         ikey.user_key.size()); has_current_user_key = true;
  //         last_sequence_for_key = kMaxSequenceNumber;
  //       }else{
  //         drop = true;
  //       }
  //       // if (last_sequence_for_key <= compact->smallest_snapshot) {
  //       //   // Hidden by an newer entry for same user key
  //       //   drop = true;  // (A)
  //       // }
  //       last_sequence_for_key = ikey.sequence;
  //     }
  //     if(!drop){
  //         //       builder.add(key, input->finger(), input->pointer(), input->index());
  //       if(isNeedGC()){
  //           //         input->clrValue();
  //         builder.add(key, input->value(), input->finger());
  //       }

  //     }else{
  //         //       input->clrValue();
  //     }

  //   }
  //   lbtree *tree = nullptr;
  //   //
    //   std::vector<std::vector<void*>> pages3 = builder.finish(tree);

  //     //   tree1->rangeDelete(pages1, tree1_start, tree1_end);

  //       //   pages3, tree2_start, tree2_end);

  //   for(int i = 0; i < its.size(); i++){
  //     delete its[i];
  //   }
  //   delete input;
  return Status::OK();
}

Status DBImpl::CompactionLevel1Concurrency(){
  int64_t imm_micros = 0;
  uint64_t start_micros = 0;
  if(TIME_ANALYSIS){
    start_micros = env_->NowMicros();
  }
  Compaction* c = versions_->PickCompaction();
  // CompactionState* compact = new CompactionState(c);
  std::vector<FileMetaData*> &files = c->inputs_[1];
  std::vector<std::vector<void*>> pages2;
  struct Task{
    Iterator* it = nullptr;
    BP_Iterator_Trim* it2 = nullptr;
    // Iterator* it3 = nullptr;
    key_type begin;
    key_type end;
    int bnode_begin;
    std::vector<FileMetaData*> files;
    CompactionState* compaction;
    void set(key_type begin, key_type end, int b){
      this->begin = begin;
      this->end = end;
      this->bnode_begin = b;
    }
  };
  std::vector<Task> tasks;
  tasks.reserve(TASK_COUNT + 2);
  uint64_t flushKVcount = 0;
  uint64_t flushKeySize = 8; // always 8B key
  uint64_t flushValSize = 0;
  bool writeSeq = files.empty() ? true : false;

    std::shared_ptr<lbtree> tree;
  {
    std::lock_guard<std::mutex> lock(mutex_l1_);
    tree = Table_LN_[0];
  }

    key_type start_key = c->smallest.empty()
                           ? tree->tree_meta->min_key
                           : DecodeDBBenchFixed64(c->smallest.data()) + 1;
  if(writeSeq){
    start_key = new_start_key_;
  }
  key_type end_key = c->largest.empty()
                         ? tree->tree_meta->max_key + 1
                         : DecodeDBBenchFixed64(c->largest.data()) - 1;

    key_type sst_start = 0;
  key_type sst_end = 0;
  if(!writeSeq){
    sst_start = DecodeDBBenchFixed64(files.front()->smallest.Encode().data());
    sst_end = DecodeDBBenchFixed64(files.back()->largest.Encode().data()) + 1;
  assert(sst_start < sst_end);
  }
  // key_type sst_start = DecodeDBBenchFixed64(c->sst_smallest.data());
  // key_type sst_end = DecodeDBBenchFixed64(c->sst_largets.data());
  // new_start_key_ = -1;
  int sst_page_index;
  int sst_page_end_index;
  kPage* kBegin;
  kPage* kEnd;
  mutex_.Unlock();

    int sst_count = (files.size() + MAX_FILE_NUM - 1) / MAX_FILE_NUM;
  pages2 = tree->getOverlappingMulTask(start_key, end_key, sst_count, sst_start,
                                       sst_end, new_start_key_, sst_page_index,
                                       sst_page_end_index, kBegin, kEnd);

  if(new_start_key_ > tree->tree_meta->max_key){
    versions_->setCompactionPointer(0, 2, c);
  }else{
    versions_->setCompactionPointer(new_start_key_, 2, c);
  }

    std::vector<void*>& bnodes = pages2[1];
  int new_start_index = -1;
  key_type new_start = 0;

    key_type rangeBegin = writeSeq ? start_key : std::min(sst_start, start_key);
    key_type rangeEnd = writeSeq ? new_start_key_ : std::max(sst_end, new_start_key_);

  if(new_start_key_ > tree->tree_meta->max_key){
    new_start_key_ = 0;
  }

  constexpr bool multiThread = true;

  auto fillTaskOverSST = [&](int start_index, int end_index, bool first, bool isend) {
    for (int i = start_index; i < end_index; i += MAX_BNODE_NUM) {
      // if(end_index - i < MAX_BNODE_NUM / 5){
      //   break;
      // }
      key_type start = ((bnode*)bnodes[i])->kBegin();
      if(first && i == start_index){
        start = rangeBegin;
      }
      if(!first && i == start_index){
        start = new_start;
      }
      key_type end;
      int bnode_count;
      int next = i + MAX_BNODE_NUM;
      // if(i + MAX_BNODE_NUM < end_index || next < end_index && end_index -
      // next < MAX_BNODE_NUM / 5){
      if (i + MAX_BNODE_NUM < end_index) {
        end = ((bnode*)bnodes[i + MAX_BNODE_NUM])->kBegin();
        bnode_count = MAX_BNODE_NUM;
      } else {
        new_start_index = i;
        // it is temp end, it is wrong.
        if (!isend) {
          break;
        } else {
          end = std::min((((bnode*)bnodes.back())->kRealEnd() + 1), rangeEnd);
          bnode_count = end_index - i;
        }
      }
      BP_Iterator_Trim* it =
          new BP_Iterator_Trim(pmAlloc_, tree.get(), bnodes, i, true);
      // BP_Iterator *it = new BP_Iterator(pmAlloc_, tree.get(), bnodes, 1,
      // INT_MAX, false, i);
      it->setKeyStartAndEnd(start, end, user_comparator());
      tasks.push_back({.it = it});
      tasks.back().set(start, end, i);
      tasks.back().it2 = it;
    }
  };
  auto fillTaskBySST = [&]() {
        std::vector<key_type> starts;
    for (int i = 0; i < files.size(); i += MAX_FILE_NUM) {
      starts.push_back(
          DecodeDBBenchFixed64(files[i]->smallest.Encode().data()));
    }
    // if (files.size() % MAX_FILE_NUM <= MAX_FILE_NUM / 4 && starts.size() > 1) {
    //   starts.pop_back();
    // }
    starts.push_back(
        DecodeDBBenchFixed64(files.back()->largest.Encode().data()) + 1);

        int cur = 0;
    int sst_index;
    int last_index;
  assert(starts.size() >= 2);
    std::vector<int> start_index(starts.size(), -1);
    // int last = sst_page_end_index == -1 ? bnodes.size() :
    // sst_page_end_index;
    int first = sst_page_index == -1 ? 0 : sst_page_index;
    for (int i = first; i < bnodes.size(); i++) {
      auto& b = ((bnode*)bnodes[i])->kBegin();
      while (cur < starts.size() && starts[cur] < b) {
        cur++;
      }
      if (cur >= starts.size()) {
        break;
      }
      if (b <= starts[cur]) {
        start_index[cur] = i;
      }
    }
    while (cur != 0 && cur < starts.size()) {
      start_index[cur] = bnodes.size() - 1;
      cur++;
    }
        int removeFirst = -1;
    int removeLast = -1;
    for (int i = 0; i < start_index.size() - 1; i++) {
      int begin_index = start_index[i] == -1 ? 0 : start_index[i];
            if (starts[i + 1] < ((bnode*)bnodes[begin_index])->kBegin()) {
                removeFirst = std::min((int)files.size(), (i + 1) * MAX_FILE_NUM);
        continue;
      }
      if ((starts[i] > ((bnode*)bnodes.back())->kRealEnd())) {
        removeLast = i * MAX_FILE_NUM;
        sst_page_end_index = -1;
        break;
      }
      if(new_start_index != -1){
        begin_index = new_start_index;
      }
      BP_Iterator_Trim* it =
          new BP_Iterator_Trim(pmAlloc_, tree.get(), bnodes, begin_index, true);
      // BP_Iterator *it = new BP_Iterator(pmAlloc_, tree.get(), bnodes, 1,
      // INT_MAX, false, begin_index);
      key_type startK = starts[i];
      key_type endK = starts[i + 1];
      if(i == 0 && new_start_index != -1){
        startK = std::max(((bnode*)bnodes[new_start_index])->kBegin(), rangeBegin);
      }
      if(i == start_index.size() - 2 && sst_page_end_index != -1 && sst_page_end_index - start_index[i + 1] < 5){
        endK = rangeEnd;
        sst_page_end_index = -1;
      }
      it->setKeyStartAndEnd(startK, endK, user_comparator());
      tasks.push_back({});
      std::vector<FileMetaData*>::iterator endIt =
          (i + 1) * MAX_FILE_NUM < files.size()
              ? files.begin() + (i + 1) * MAX_FILE_NUM
              : files.end();
      tasks.back().files.assign(files.begin() + i * MAX_FILE_NUM, endIt);
      Iterator* it2 = versions_->getTwoLevelIterator(tasks.back().files);
      Iterator* its[2] = {it, it2};
      tasks.back().it = NewMergingIterator(user_comparator(), its, 2);
      tasks.back().set(startK, endK, begin_index);
      tasks.back().it2 = it;
      // tasks.back().it3 = it2;
      if(new_start_index != -1){
        new_start_index = -1;
      }
    }
    if(removeFirst != -1){
      if(DEBUG_PRINT){
        printf("erase files : %d-%d\n",0, removeFirst);
      }
      files.erase(files.begin(), files.begin() + removeFirst);
    }
    if(removeLast != -1){
      if(DEBUG_PRINT){
        printf("erase files : %d-%zu\n",removeLast, files.size());
      }
      files.erase(files.begin() + removeLast, files.end());
    }
  };

  if(multiThread){
    if (writeSeq) {
      fillTaskOverSST(0, bnodes.size(), true, true);
    } else {
      // add task before sst.
      fillTaskOverSST(0, sst_page_index + 1, true, false);
      assert(new_start_index != -1);
      // if (!tasks.empty()) {
      //   ((BP_Iterator*)tasks.back().it)->setEnd(sst_start);
      // }
      // add sst in task.
      if (sst_page_index != -1 || sst_page_end_index != -1) {
        // int cur_task_count = tasks.size();
        fillTaskBySST();
        // if (cur_task_count < tasks.size() && new_start_index != -1) {
        //   ((BP_Iterator*)tasks[cur_task_count].it)
        //       ->setStart(((bnode*)bnodes[new_start_index])->kBegin());
        // }
      }
      if (sst_page_end_index != -1) {
        new_start = sst_end;
        // int cur_task_count = tasks.size();
        fillTaskOverSST(sst_page_end_index, bnodes.size(), false, true);
        // if (cur_task_count < tasks.size()) {
        //   ((BP_Iterator*)tasks[cur_task_count].it)->setStart(sst_end);
        // }
      }
    }
  }else{
    if(writeSeq){
      BP_Iterator_Trim* it =
        new BP_Iterator_Trim(pmAlloc_, tree.get(), bnodes, 0, true);
      it->setKeyStartAndEnd(rangeBegin, rangeEnd, user_comparator());
      tasks.push_back({.it = it});
      tasks.back().set(rangeBegin, rangeEnd, 0);
      tasks.back().it2 = it;
    }else{
      BP_Iterator_Trim* it =
          new BP_Iterator_Trim(pmAlloc_, tree.get(), bnodes, 0, true);
      // BP_Iterator *it = new BP_Iterator(pmAlloc_, tree.get(), bnodes, 1,
      // INT_MAX, false, begin_index);
      it->setKeyStartAndEnd(rangeBegin, rangeEnd, user_comparator());
      tasks.push_back({});
      tasks.back().files = files;
      Iterator* it2 = versions_->getTwoLevelIterator(tasks.back().files);
      Iterator* its[2] = {it, it2};
      tasks.back().it = NewMergingIterator(user_comparator(), its, 2);
      tasks.back().set(rangeBegin, rangeEnd, 0);
      tasks.back().it2 = it;
      // tasks.back().it3 = it2;
    }
  }

  if(DEBUG_PRINT){
    key_type lastEnd = 0;
    int lastIndex = 0;
    for(int i = 0; i < tasks.size(); i++){
      printf("add task: [%ld, %ld), node count: %d, sst count: %zull\n", tasks[i].begin, tasks[i].end, tasks[i].bnode_begin, tasks[i].files.size());
    assert(lastEnd <= tasks[i].begin);
    assert(lastIndex <= tasks[i].bnode_begin);
    assert(tasks[i].begin < tasks[i].end);
      lastEnd = tasks[i].end;
      lastIndex = tasks[i].bnode_begin;
    }
  assert(rangeBegin <= tasks.front().begin && tasks.back().end <= rangeEnd);
  }
  auto writeToSSD = [&](Task &task, CompactionState *compact){
      Iterator *input = task.it;
      // Log(options_.info_log, "Compacting %d@%d + %d@%d files",
      //     compact->compaction->num_input_files(0), compact->compaction->level(),
      //     compact->compaction->num_input_files(1),
      //     compact->compaction->level() + 1);

      assert(versions_->NumLevelFiles(compact->compaction->level()) > 0);
    assert(compact->builder == nullptr);
    assert(compact->outfile == nullptr);
      compact->smallest_snapshot = versions_->LastSequence();


      // Release mutex while we're actually doing the compaction work
      input->SeekToFirst();
      Status status;
      ParsedInternalKey ikey;
      std::string current_user_key;
      bool has_current_user_key = false;
      SequenceNumber last_sequence_for_key = kMaxSequenceNumber;
      while (input->Valid() && !shutting_down_.load(std::memory_order_acquire)) {

        Slice key = input->key();
        // if (compact->compaction->ShouldStopBefore(key) &&
        //     compact->builder != nullptr) {
        //   status = FinishCompactionOutputFile(compact, input);
        //   if (!status.ok()) {
        //     break;
        //   }
        // }

        // Handle key/value, add to state, etc.
        bool drop = false;
        if (!ParseInternalKey(key, &ikey)) {
          // Do not hide error keys
          current_user_key.clear();
          has_current_user_key = false;
          last_sequence_for_key = kMaxSequenceNumber;
        } else {
          if (!has_current_user_key ||
              user_comparator()->Compare(ikey.user_key, Slice(current_user_key)) !=
                  0) {
            // First occurrence of this user key
            current_user_key.assign(ikey.user_key.data(), ikey.user_key.size());
            has_current_user_key = true;
            last_sequence_for_key = kMaxSequenceNumber;
          }

          if (last_sequence_for_key <= compact->smallest_snapshot) {
            // Hidden by an newer entry for same user key
            drop = true;  // (A)
          } else if (ikey.type == kTypeDeletion &&
                    ikey.sequence <= compact->smallest_snapshot
                    //  && compact->compaction->IsBaseLevelForKey(ikey.user_key)
                    ) {
            // For this user key:
            // (1) there is no data in higher levels
            // (2) data in lower levels will have larger sequence numbers
            // (3) data in layers that are being compacted here and have
            //     smaller sequence numbers will be dropped in the next
            //     few iterations of this loop (by rule (A) above).
            // Therefore this deletion marker is obsolete and can be dropped.
            drop = true;
          }

          last_sequence_for_key = ikey.sequence;
        }
    #if 0
        Log(options_.info_log,
            "  Compact: %s, seq %d, type: %d %d, drop: %d, is_base: %d, "
            "%d smallest_snapshot: %d",
            ikey.user_key.ToString().c_str(),
            (int)ikey.sequence, ikey.type, kTypeValue, drop,
            compact->compaction->IsBaseLevelForKey(ikey.user_key),
            (int)last_sequence_for_key, (int)compact->smallest_snapshot);
    #endif

        if (!drop) {
          // Open output file if necessary
          if (compact->builder == nullptr) {
            status = OpenCompactionOutputFile(compact);
            if (!status.ok()) {
              break;
            }
          }
          if (compact->builder->NumEntries() == 0) {
            compact->current_output()->smallest.DecodeFrom(key);
          }
          compact->current_output()->largest.DecodeFrom(key);
          assert(files.empty() || (DecodeDBBenchFixed64(key.data()) <= end_key && DecodeDBBenchFixed64(key.data()) >= start_key));
          compact->builder->Add(key, input->value());
          if (EVALUATE_METRIC) {
            flushValSize = std::max(flushValSize, input->value().size());
          }
          // cuckoo_filter_->Delete(key, mergeIterator->fileNum());
          // cuckoo_filter_->Put(key, compact->out_file_number);
          // if(mergeIterator->fileNum() != compact->compaction->level() + 1){
          //   cuckoo_filter_->Update(ikey.user_key, mergeIterator->fileNum(), compact->compaction->level() + 1);
          // }
          // Close output file if it is big enough
          if (compact->builder->FileSize() >=
              compact->compaction->MaxOutputFileSize()) {
            status = FinishCompactionOutputFile(compact, input);
            if (!status.ok()) {
              break;
            }
          }
        }else{
          // cuckoo_filter_->Delete(ikey.user_key, mergeIterator->fileNum());
        }

        input->Next();
      }

      if (status.ok() && shutting_down_.load(std::memory_order_acquire)) {
        status = Status::IOError("Deleting DB during compaction");
      }
      if (status.ok() && compact->builder != nullptr) {
        status = FinishCompactionOutputFile(compact, input);
      }
      if (status.ok()) {
        status = input->status();
      }
  };
  std::vector<std::thread> threads;
  std::vector<CompactionState*> states;
  for(int i = 0; i < tasks.size(); i++){
    states.push_back(new CompactionState(c));
    std::thread th(writeToSSD, std::ref(tasks[i]), states.back());
    threads.push_back(std::move(th));
  }
  for(int i = 0; i < tasks.size(); i++){
    threads[i].join();
  }



  mutex_.Lock();

  Status status = InstallCompactionResults(states);
  if (!status.ok()) {
    RecordBackgroundError(status);
  }
  VersionSet::LevelSummaryStorage tmp;
  Log(options_.info_log, "compacted to: %s", versions_->LevelSummary(&tmp));

      {
    std::lock_guard<std::mutex> lock(mutex_l1_);
    std::shared_ptr<lbtree> tree2 = Table_LN_[0];
    tree2->rangeDelete(pages2, rangeBegin, rangeEnd - 1);
    if(rangeEnd > tree2->tree_meta->max_key && rangeBegin <= tree2->tree_meta->min_key){
      Table_LN_.clear();
    }else{
      if(rangeEnd > tree2->tree_meta->max_key){
        tree2->tree_meta->max_key = rangeBegin - 1;
      }
      if(rangeBegin <= tree2->tree_meta->min_key){
        tree2->tree_meta->min_key = rangeEnd;
      }
    }

    for(int i = 0; i < tasks.size(); i++){
      if (EVALUATE_METRIC) {
        flushKVcount += tasks[i].it2->validKVcount();
      }
      tasks[i].it2->releaseKVpage();
      // delete tasks[i].it3;
      // delete tasks[i].it2;
      // if(tasks[i].it2 != tasks[i].it){
      delete tasks[i].it;
      // }
    }

    if(kBegin != nullptr){
      kBegin->setNext(kEnd);
    }
    if(DEBUG_CHECK){
      if(!Table_LN_.empty()){
        Table_LN_[0]->reverseAndCheck();
        Table_LN_[0]->checkIterator();
      }
    }
  }

  if (TIME_ANALYSIS) {
    imm_micros = env_->NowMicros() - start_micros;
    Log(options_.info_log, "compaction at level %d, used time : %ld", 2,
        imm_micros / 1000);
  }
  CompactionStats stats;
  stats.micros = imm_micros;
  for(const auto& c : states){
    for(const auto& file : c->outputs){
      stats.bytes_written += file.file_size;
      if (EVALUATE_METRIC) {
        writeMetric_.WriteSsdDataBytes += file.file_size;
      }
    }
  }
  stats_[2].Add(stats);
  usage_ = pmAlloc_->getMemoryUsabe() * 100;
  if(usage_ < 20){
    curMemtableSize_ = initMemtableSize;
  }
  if(DEBUG_PRINT){
    std::cout<< "compaction at level 2, cost time : "<< imm_micros / 1000;
    std::cout<< " memoryUsage: "<< usage_ * 1.0 / 100;
    std::cout<< " min key : " << rangeBegin << " max key : " << rangeEnd << std::endl;
  }
  if (!status.ok()) {
    RecordBackgroundError(status);
  }
  CleanupCompaction(states);
  c->ReleaseInputs();
  RemoveObsoleteFiles();
  delete c;

  if (EVALUATE_METRIC) {
    writeMetric_.WriteValueSize = flushValSize;
    writeMetric_.FlushSsdDataBytes += flushKVcount * (flushKeySize + flushValSize);

    // writeMetric_.printMetric();
  }
  
  return Status::OK();
}

Status DBImpl::CompactionLevel1(){
  return Status::OK();
//   int64_t imm_micros = 0;
// #ifdef CAL_TIME
//   const uint64_t start_micros = env_->NowMicros();
// #endif
//   //   //   //   //   // finished
//   //   // checkAndSetGC();
//   Compaction* c = versions_->PickCompaction();
//   CompactionState* compact = new CompactionState(c);
//   std::vector<FileMetaData*> &files = c->inputs_[1];
//   Iterator* input;
//   int index_start_pos2 = 0;
//   int output_page_count = 0;
//   key_type tree2_start;
//   key_type tree2_end;
//   key_type start_key;
//   key_type end_key;
//   kPage* kBegin;
//   kPage* kEnd;
//   std::vector<std::vector<void*>> pages2;
//   Iterator* it1 = nullptr;
//   if(files.empty()){
//     std::lock_guard<std::mutex> lock(mutex_l1_);
//     std::shared_ptr<lbtree> tree2 = Table_LN_[0];
//     //     pages2 =
//         tree2->getOverlapping(tree2->tree_meta->min_key, tree2->tree_meta->max_key, &index_start_pos2,
//                               &output_page_count, &tree2_start, &tree2_end, kBegin, kEnd);
//     // std::vector<std::vector<bnode*>> pages3;
//     it1 = new BP_Iterator(pmAlloc_, tree2.get(), pages2[1],
//                                        index_start_pos2, output_page_count, true);
//     input = NewMergingIterator(user_comparator(), &it1, 1);
//   }else{
//     std::lock_guard<std::mutex> lock(mutex_l1_);
//     Iterator* it2 = versions_->getTwoLevelIterator(files);
//     std::shared_ptr<lbtree> tree2 = Table_LN_[0];
//     // key_type input_start = DecodeDBBenchFixed64(c->smallest.data()) + 1;
//     // key_type input_end = DecodeDBBenchFixed64(c->largest.data()) - 1;
//     // key_type input_start = DecodeDBBenchFixed64(files.front()->smallest.user_key().data());
//     // key_type input_end = DecodeDBBenchFixed64(files.back()->largest.user_key().data());
//     //     start_key = c->smallest.empty() ? tree2->tree_meta->min_key : DecodeDBBenchFixed64(c->smallest.data()) + 1;
//     end_key = c->largest.empty() ? tree2->tree_meta->max_key : DecodeDBBenchFixed64(c->largest.data()) - 1;
//     pages2 =
//         tree2->getOverlapping(start_key, end_key, &index_start_pos2,
//                               &output_page_count, &tree2_start, &tree2_end, kBegin, kEnd);
//     // std::vector<std::vector<bnode*>> pages3;
//     it1 = new BP_Iterator(pmAlloc_, tree2.get(), pages2[1],
//                                        index_start_pos2, output_page_count, true);
//     ((BP_Iterator*)it1)->setKeyStartAndEnd(c->smallest, c->largest, user_comparator());
//     //     Iterator* its[2] = {it1, it2};
//     input = NewMergingIterator(user_comparator(), its, 2);
//   }

//   // Log(options_.info_log, "Compacting %d@%d + %d@%d files",
//   //     compact->compaction->num_input_files(0), compact->compaction->level(),
//   //     compact->compaction->num_input_files(1),
//   //     compact->compaction->level() + 1);

//   assert(versions_->NumLevelFiles(compact->compaction->level()) > 0);
// assert(compact->builder == nullptr);
// assert(compact->outfile == nullptr);
//   if (snapshots_.empty()) {
//     compact->smallest_snapshot = versions_->LastSequence();
//   } else { 
//     compact->smallest_snapshot = snapshots_.oldest()->sequence_number();
//   }

//   // Release mutex while we're actually doing the compaction work
//   mutex_.Unlock();

//   input->SeekToFirst();
//   Status status;
//   ParsedInternalKey ikey;
//   std::string current_user_key;
//   bool has_current_user_key = false;
//   SequenceNumber last_sequence_for_key = kMaxSequenceNumber;
//   while (input->Valid() && !shutting_down_.load(std::memory_order_acquire)) {

//     Slice key = input->key();
//     // if (compact->compaction->ShouldStopBefore(key) &&
//     //     compact->builder != nullptr) {
//     //   status = FinishCompactionOutputFile(compact, input);
//     //   if (!status.ok()) {
//     //     break;
//     //   }
//     // }

//     // Handle key/value, add to state, etc.
//     bool drop = false;
//     if (!ParseInternalKey(key, &ikey)) {
//       // Do not hide error keys
//       current_user_key.clear();
//       has_current_user_key = false;
//       last_sequence_for_key = kMaxSequenceNumber;
//     } else {
//       if (!has_current_user_key ||
//           user_comparator()->Compare(ikey.user_key, Slice(current_user_key)) !=
//               0) {
//         // First occurrence of this user key
//         current_user_key.assign(ikey.user_key.data(), ikey.user_key.size());
//         has_current_user_key = true;
//         last_sequence_for_key = kMaxSequenceNumber;
//       }

//       if (last_sequence_for_key <= compact->smallest_snapshot) {
//         // Hidden by an newer entry for same user key
//         drop = true;  // (A)
//       } else if (ikey.type == kTypeDeletion &&
//                  ikey.sequence <= compact->smallest_snapshot
//                 //  && compact->compaction->IsBaseLevelForKey(ikey.user_key)
//                  ) {
//         // For this user key:
//         // (1) there is no data in higher levels
//         // (2) data in lower levels will have larger sequence numbers
//         // (3) data in layers that are being compacted here and have
//         //     smaller sequence numbers will be dropped in the next
//         //     few iterations of this loop (by rule (A) above).
//         // Therefore this deletion marker is obsolete and can be dropped.
//         drop = true;
//       }

//       last_sequence_for_key = ikey.sequence;
//     }
// #if 0
//     Log(options_.info_log,
//         "  Compact: %s, seq %d, type: %d %d, drop: %d, is_base: %d, "
//         "%d smallest_snapshot: %d",
//         ikey.user_key.ToString().c_str(),
//         (int)ikey.sequence, ikey.type, kTypeValue, drop,
//         compact->compaction->IsBaseLevelForKey(ikey.user_key),
//         (int)last_sequence_for_key, (int)compact->smallest_snapshot);
// #endif

//     if (!drop) {
//       // Open output file if necessary
//       if (compact->builder == nullptr) {
//         status = OpenCompactionOutputFile(compact);
//         if (!status.ok()) {
//           break;
//         }
//       }
//       if (compact->builder->NumEntries() == 0) {
//         compact->current_output()->smallest.DecodeFrom(key);
//       }
//       compact->current_output()->largest.DecodeFrom(key);
//       assert(files.empty() || (DecodeDBBenchFixed64(key.data()) <= end_key && DecodeDBBenchFixed64(key.data()) >= start_key));
//       compact->builder->Add(key, input->value());
//       // cuckoo_filter_->Delete(key, mergeIterator->fileNum());
//       // cuckoo_filter_->Put(key, compact->out_file_number);
//       // if(mergeIterator->fileNum() != compact->compaction->level() + 1){
//       //   cuckoo_filter_->Update(ikey.user_key, mergeIterator->fileNum(), compact->compaction->level() + 1);
//       // }
//       // Close output file if it is big enough
//       if (compact->builder->FileSize() >=
//           compact->compaction->MaxOutputFileSize()) {
//         status = FinishCompactionOutputFile(compact, input);
//         if (!status.ok()) {
//           break;
//         }
//       }
//     }else{
//       // cuckoo_filter_->Delete(ikey.user_key, mergeIterator->fileNum());
//     }

//     input->Next();
//   }

//   if (status.ok() && shutting_down_.load(std::memory_order_acquire)) {
//     status = Status::IOError("Deleting DB during compaction");
//   }
//   if (status.ok() && compact->builder != nullptr) {
//     status = FinishCompactionOutputFile(compact, input);
//   }
//   if (status.ok()) {
//     status = input->status();
//   }

//   mutex_.Lock();

//   if (status.ok()) {
//     status = InstallCompactionResults(compact);
//   }
//   if (!status.ok()) {
//     RecordBackgroundError(status);
//   }
//   VersionSet::LevelSummaryStorage tmp;
//   Log(options_.info_log, "compacted to: %s", versions_->LevelSummary(&tmp));
//   //   //   {
//     std::lock_guard<std::mutex> lock(mutex_l1_);
//     std::shared_ptr<lbtree> tree2 = Table_LN_[0];
//     // tree2->needFreeNodePages = pages2;
//     if(files.empty()){
//       tree2->rangeDelete(pages2, tree2->tree_meta->min_key, tree2->tree_meta->max_key);
//       Table_LN_.clear();
// #ifdef DEBUG_PRINT
//       start_key = tree2->tree_meta->min_key;
//       end_key = tree2->tree_meta->max_key;
// #endif
//     }else{
//       tree2->rangeDelete(pages2, start_key, end_key);
//       if(end_key == tree2->tree_meta->max_key){
//         tree2->tree_meta->max_key = start_key - 1;
//       }
//       if(start_key == tree2->tree_meta->min_key){
//         tree2->tree_meta->min_key = end_key + 1;
//       }

//     }
//     ((BP_Iterator*)it1)->releaseKVpage();
//     if(kBegin != nullptr){
//       kBegin->setNext(kEnd);
//     }
// #ifdef MY_DEBUG
//   if(!Table_LN_.empty()){
//     Table_LN_[0]->reverseAndCheck();
//     Table_LN_[0]->checkIterator();
//   }
// #endif
//   }
//   delete input;

// #ifdef CAL_TIME
//   imm_micros = env_->NowMicros() - start_micros;
// #endif
// #ifdef DEBUG_PRINT
//       double usage1 = pmAlloc_->getMemoryUsabe();
//       std::cout<< "compaction at level 1, cost time : "<< imm_micros / 1000;
//       std::cout<< " memoryUsage: "<<usage1;
//       std::cout<< " min key : " << start_key << " max key : " << end_key << std::endl;
// #endif
//   if (!status.ok()) {
//     RecordBackgroundError(status);
//   }
//   CleanupCompaction(compact);
//   c->ReleaseInputs();
//   RemoveObsoleteFiles();
//   delete c;
//   return Status::OK();
}

void DBImpl::MergeL1(){
  std::unique_lock<std::mutex> lk(mergeMutex_);
  Status status;
  while(!shutting_down_){
    int level = PickCompactionPM();
    if(level == 0){
      status = CompactionLevel0();
      needGC = false;
    }else if(level == 1){
      mutex_.Lock();
      // status = CompactionLevel1();
      status = CompactionLevel1Concurrency();
      mutex_.Unlock();
      flushSSD = false;
    }

    conVar_.wait_for(lk, std::chrono::milliseconds(10000));
  }
}



void DBImpl::BackgroundCompaction() {
  mutex_.AssertHeld();
  // mutex_.Unlock();

  mutex_l0_.lock();
  l0_num_ = Table_L0_.size();
  mutex_l0_.unlock();

  // mutex_.Lock();
  usage_ = pmAlloc_->getMemoryUsabe() * 100;

  if(usage_ < 95){
    space_signal_.SignalAll();
  }
  bool right = false;
  if (imm_ != nullptr && l0_num_ < L0BufferCount * ratio_L0_ && !needGC && !flushSSD && usage_ <= memory_rate * 100) {
    CompactMemTable();
    conVar_.notify_one();
    //CompactMemTableToPM();
    background_work_finished_signal_.SignalAll();
    return;
  }else{
    right = true;
    // std::cout<<"not flush"<<std::endl;
  }
  background_work_finished_signal_.SignalAll();
  // std::chrono::milliseconds duration(2);    // std::this_thread::sleep_for(duration);    // Compaction* c;
  // bool is_manual = (manual_compaction_ != nullptr);
  // InternalKey manual_end;
  // if (is_manual) {
  //   ManualCompaction* m = manual_compaction_;
  //   c = versions_->CompactRange(m->level, m->begin, m->end);
  //   m->done = (c == nullptr);
  //   if (c != nullptr) {
  //     manual_end = c->input(0, c->num_input_files(0) - 1)->largest;
  //   }
  //   Log(options_.info_log,
  //       "Manual compaction at level-%d from %s .. %s; will stop at %s\n",
  //       m->level, (m->begin ? m->begin->DebugString().c_str() : "(begin)"),
  //       (m->end ? m->end->DebugString().c_str() : "(end)"),
  //       (m->done ? "(end)" : manual_end.DebugString().c_str()));
  // } else {
  //   c = versions_->PickCompaction();
  // }

  // if (c == nullptr) {
  //   // Nothing to do
  // } else if (!is_manual && c->IsTrivialMove()) {
  //   // Move file to next level
  // assert(c->num_input_files(0) == 1);
  //   FileMetaData* f = c->input(0, 0);
  //   c->edit()->RemoveFile(c->level(), f->number);
  //   c->edit()->AddFile(c->level() + 1, f->number, f->file_size, f->smallest,
  //                      f->largest);
  //   status = versions_->LogAndApply(c->edit(), &mutex_);
  //   if (!status.ok()) {
  //     RecordBackgroundError(status);
  //   }
  //   VersionSet::LevelSummaryStorage tmp;
  //   Log(options_.info_log, "Moved #%lld to level-%d %lld bytes %s: %s\n",
  //       static_cast<unsigned long long>(f->number), c->level() + 1,
  //       static_cast<unsigned long long>(f->file_size),
  //       status.ToString().c_str(), versions_->LevelSummary(&tmp));
  // } else {
  //   CompactionState* compact = new CompactionState(c);
  //   status = DoCompactionWork(compact);
  //   if (!status.ok()) {
  //     RecordBackgroundError(status);
  //   }
  //   CleanupCompaction(compact);
  //   c->ReleaseInputs();
  //   RemoveObsoleteFiles();
  // }
  // delete c;

  // if (status.ok()) {
  //   // Done
  // } else if (shutting_down_.load(std::memory_order_acquire)) {
  //   // Ignore compaction errors found during shutting down
  // } else {
  //   Log(options_.info_log, "Compaction error: %s", status.ToString().c_str());
  // }

  // if (is_manual) {
  //   ManualCompaction* m = manual_compaction_;
  //   if (!status.ok()) {
  //     m->done = true;
  //   }
  //   if (!m->done) {
  //     // We only compacted part of the requested range.  Update *m
  //     // to the range that is left to be compacted.
  //     m->tmp_storage = manual_end;
  //     m->begin = &m->tmp_storage;
  //   }
  //   manual_compaction_ = nullptr;
  // }
}

void DBImpl::CleanupCompaction(std::vector<CompactionState*> compacts) {
  for(int i = 0; i < compacts.size(); i++){
    CleanupCompaction(compacts[i]);
  }
}

void DBImpl::CleanupCompaction(CompactionState* compact) {
  mutex_.AssertHeld();
  if (compact->builder != nullptr) {
    // May happen if we get a shutdown call in the middle of compaction
    compact->builder->Abandon();
    delete compact->builder;
  } else {
  assert(compact->outfile == nullptr);
  }
  delete compact->outfile;
  for (size_t i = 0; i < compact->outputs.size(); i++) {
    const CompactionState::Output& out = compact->outputs[i];
    pending_outputs_.erase(out.number);
  }
  delete compact;
}

Status DBImpl::OpenCompactionOutputFile(CompactionState* compact) {
assert(compact != nullptr);
assert(compact->builder == nullptr);
  uint64_t file_number;
  {
    mutex_.Lock();
    file_number = versions_->NewFileNumber();
    pending_outputs_.insert(file_number);
    CompactionState::Output out;
    out.number = file_number;
    out.smallest.Clear();
    out.largest.Clear();
    compact->outputs.push_back(out);
    mutex_.Unlock();
  }

  // Make the output file
  std::string fname = TableFileName(dbname_, file_number);
  compact->out_file_number = file_number;
  Status s = env_->NewWritableFile(fname, &compact->outfile);
  if (s.ok()) {
    compact->builder = new TableBuilder(options_, compact->outfile);
  }
  return s;
}

Status DBImpl::FinishCompactionOutputFile(CompactionState* compact,
                                          Iterator* input) {
assert(compact != nullptr);
assert(compact->outfile != nullptr);
assert(compact->builder != nullptr);

  const uint64_t output_number = compact->current_output()->number;
assert(output_number != 0);

  // Check for iterator errors
  Status s = input->status();
  const uint64_t current_entries = compact->builder->NumEntries();
  if (s.ok()) {
    s = compact->builder->Finish();
  } else {
    compact->builder->Abandon();
  }
  const uint64_t current_bytes = compact->builder->FileSize();
  compact->current_output()->file_size = current_bytes;
  compact->total_bytes += current_bytes;
  delete compact->builder;
  compact->builder = nullptr;

  // Finish and check for file errors
  if (s.ok()) {
    s = compact->outfile->Sync();
  }
  if (s.ok()) {
    s = compact->outfile->Close();
  }
  delete compact->outfile;
  compact->outfile = nullptr;

  if (s.ok() && current_entries > 0) {
    // Verify that the table is usable
    Iterator* iter =
        table_cache_->NewIterator(ReadOptions(), output_number, current_bytes);
    s = iter->status();
    delete iter;
    if (s.ok()) {
      Log(options_.info_log, "Generated table #%llu@%d: %lld keys, %lld bytes",
          (unsigned long long)output_number, compact->compaction->level(),
          (unsigned long long)current_entries,
          (unsigned long long)current_bytes);
    }
  }
  return s;
}
Status DBImpl::InstallCompactionResults(std::vector<CompactionState*>& compacts){
  mutex_.AssertHeld();

  // Add compaction outputs
  for(auto &compact : compacts){
    compact->compaction->AddInputDeletions(compact->compaction->edit());
    const int level = compact->compaction->level();
    for (size_t i = 0; i < compact->outputs.size(); i++) {
      const CompactionState::Output& out = compact->outputs[i];
      compact->compaction->edit()->AddFile(level + 1, out.number, out.file_size,
                                          out.smallest, out.largest);
    }
  }
  compacts.front()->compaction->edit()->sortNewFiles(internal_comparator_);
  return versions_->LogAndApply(compacts.front()->compaction->edit(), &mutex_);
}

Status DBImpl::InstallCompactionResults(CompactionState* compact) {
  mutex_.AssertHeld();
  Log(options_.info_log, "Compacted %d@%d + %d@%d files => %lld bytes",
      compact->compaction->num_input_files(0), compact->compaction->level(),
      compact->compaction->num_input_files(1), compact->compaction->level() + 1,
      static_cast<long long>(compact->total_bytes));

  // Add compaction outputs
  compact->compaction->AddInputDeletions(compact->compaction->edit());
  const int level = compact->compaction->level();
  for (size_t i = 0; i < compact->outputs.size(); i++) {
    const CompactionState::Output& out = compact->outputs[i];
    compact->compaction->edit()->AddFile(level + 1, out.number, out.file_size,
                                         out.smallest, out.largest);
  }
  return versions_->LogAndApply(compact->compaction->edit(), &mutex_);
}

Status DBImpl:: DoCompactionWork(CompactionState* compact) {
  const uint64_t start_micros = env_->NowMicros();
  int64_t imm_micros = 0;  // Micros spent doing imm_ compactions

  Log(options_.info_log, "Compacting %d@%d + %d@%d files",
      compact->compaction->num_input_files(0), compact->compaction->level(),
      compact->compaction->num_input_files(1),
      compact->compaction->level() + 1);

assert(versions_->NumLevelFiles(compact->compaction->level()) > 0);
assert(compact->builder == nullptr);
assert(compact->outfile == nullptr);
  if (snapshots_.empty()) {
    compact->smallest_snapshot = versions_->LastSequence();
  } else {
    compact->smallest_snapshot = snapshots_.oldest()->sequence_number();
  }

  Iterator* input = versions_->MakeInputIterator(compact->compaction);
  // MergingIterator *mergeIterator = static_cast<MergingIterator*>(input);

  // Release mutex while we're actually doing the compaction work
  mutex_.Unlock();

  input->SeekToFirst();
  Status status;
  ParsedInternalKey ikey;
  std::string current_user_key;
  bool has_current_user_key = false;
  SequenceNumber last_sequence_for_key = kMaxSequenceNumber;
  while (input->Valid() && !shutting_down_.load(std::memory_order_acquire)) {
    // Prioritize immutable compaction work
    if (has_imm_.load(std::memory_order_relaxed)) {
      const uint64_t imm_start = env_->NowMicros();
      mutex_.Lock();
      if (imm_ != nullptr) {
        CompactMemTable();
        // Wake up MakeRoomForWrite() if necessary.
        background_work_finished_signal_.SignalAll();
      }
      mutex_.Unlock();
      imm_micros += (env_->NowMicros() - imm_start);
    }

    Slice key = input->key();
    if (compact->compaction->ShouldStopBefore(key) &&
        compact->builder != nullptr) {
      status = FinishCompactionOutputFile(compact, input);
      if (!status.ok()) {
        break;
      }
    }

    // Handle key/value, add to state, etc.
    bool drop = false;
    if (!ParseInternalKey(key, &ikey)) {
      // Do not hide error keys
      current_user_key.clear();
      has_current_user_key = false;
      last_sequence_for_key = kMaxSequenceNumber;
    } else {
      if (!has_current_user_key ||
          user_comparator()->Compare(ikey.user_key, Slice(current_user_key)) !=
              0) {
        // First occurrence of this user key
        current_user_key.assign(ikey.user_key.data(), ikey.user_key.size());
        has_current_user_key = true;
        last_sequence_for_key = kMaxSequenceNumber;
      }

      if (last_sequence_for_key <= compact->smallest_snapshot) {
        // Hidden by an newer entry for same user key
        drop = true;  // (A)
      } else if (ikey.type == kTypeDeletion &&
                 ikey.sequence <= compact->smallest_snapshot &&
                 compact->compaction->IsBaseLevelForKey(ikey.user_key)) {
        // For this user key:
        // (1) there is no data in higher levels
        // (2) data in lower levels will have larger sequence numbers
        // (3) data in layers that are being compacted here and have
        //     smaller sequence numbers will be dropped in the next
        //     few iterations of this loop (by rule (A) above).
        // Therefore this deletion marker is obsolete and can be dropped.
        drop = true;
      }

      last_sequence_for_key = ikey.sequence;
    }
#if 0
    Log(options_.info_log,
        "  Compact: %s, seq %d, type: %d %d, drop: %d, is_base: %d, "
        "%d smallest_snapshot: %d",
        ikey.user_key.ToString().c_str(),
        (int)ikey.sequence, ikey.type, kTypeValue, drop,
        compact->compaction->IsBaseLevelForKey(ikey.user_key),
        (int)last_sequence_for_key, (int)compact->smallest_snapshot);
#endif

    if (!drop) {
      // Open output file if necessary
      if (compact->builder == nullptr) {
        status = OpenCompactionOutputFile(compact);
        if (!status.ok()) {
          break;
        }
      }
      if (compact->builder->NumEntries() == 0) {
        compact->current_output()->smallest.DecodeFrom(key);
      }
      compact->current_output()->largest.DecodeFrom(key);
      compact->builder->Add(key, input->value());
      // cuckoo_filter_->Delete(key, mergeIterator->fileNum());
      // cuckoo_filter_->Put(key, compact->out_file_number);
      // if(mergeIterator->fileNum() != compact->compaction->level() + 1){
      //   cuckoo_filter_->Update(ikey.user_key, mergeIterator->fileNum(), compact->compaction->level() + 1);
      // }
      // Close output file if it is big enough
      if (compact->builder->FileSize() >=
          compact->compaction->MaxOutputFileSize()) {
        status = FinishCompactionOutputFile(compact, input);
        if (!status.ok()) {
          break;
        }
      }
    }else{
      // cuckoo_filter_->Delete(ikey.user_key, mergeIterator->fileNum());
    }

    input->Next();
  }

  if (status.ok() && shutting_down_.load(std::memory_order_acquire)) {
    status = Status::IOError("Deleting DB during compaction");
  }
  if (status.ok() && compact->builder != nullptr) {
    status = FinishCompactionOutputFile(compact, input);
  }
  if (status.ok()) {
    status = input->status();
  }
  delete input;
  input = nullptr;

  CompactionStats stats;
  stats.micros = env_->NowMicros() - start_micros - imm_micros;
  for (int which = 0; which < 2; which++) {
    for (int i = 0; i < compact->compaction->num_input_files(which); i++) {
      stats.bytes_read += compact->compaction->input(which, i)->file_size;
    }
  }
  for (size_t i = 0; i < compact->outputs.size(); i++) {
    stats.bytes_written += compact->outputs[i].file_size;
  }

  mutex_.Lock();
  stats_[compact->compaction->level() + 1].Add(stats);

  if (status.ok()) {
    status = InstallCompactionResults(compact);
  }
  if (!status.ok()) {
    RecordBackgroundError(status);
  }
  VersionSet::LevelSummaryStorage tmp;
  Log(options_.info_log, "compacted to: %s", versions_->LevelSummary(&tmp));
  return status;
}

namespace {

struct IterState {
  port::Mutex* const mu;
  Version* const version GUARDED_BY(mu);
  MemTable* const mem GUARDED_BY(mu);
  MemTable* const imm GUARDED_BY(mu);

  IterState(port::Mutex* mutex, MemTable* mem, MemTable* imm, Version* version)
      : mu(mutex), version(version), mem(mem), imm(imm) {}
};

static void CleanupIteratorState(void* arg1, void* arg2) {
  IterState* state = reinterpret_cast<IterState*>(arg1);
  state->mu->Lock();
  state->mem->Unref();
  if (state->imm != nullptr) state->imm->Unref();
  state->version->Unref();
  state->mu->Unlock();
  delete state;
}

}  // anonymous namespace

Iterator* DBImpl::NewInternalIterator(const ReadOptions& options,
                                      SequenceNumber* latest_snapshot,
                                      uint32_t* seed) {
  mutex_.Lock();
  *latest_snapshot = versions_->LastSequence();

  // Collect together all needed child iterators
  std::vector<Iterator*> list;
  list.push_back(mem_->NewIterator());
  mem_->Ref();
  if (imm_ != nullptr) {
    list.push_back(imm_->NewIterator());
    imm_->Ref();
  }
  mutex_l0_.lock();
  for(int i = Table_L0_.size() - 1; i >=0 ;i--){
    list.push_back(new BP_Iterator_Read(Table_L0_[i]));
  }
  mutex_l0_.unlock();
  mutex_l1_.lock();
  assert(Table_LN_.size() == 1 || Table_LN_.size() == 0);
    if(!Table_LN_.empty()){
      list.push_back(new BP_Iterator_Read(Table_LN_.back(), nullptr, true, &mutex_l1_));
    }
  mutex_l1_.unlock();
  versions_->current()->AddIterators(options, &list);
  Iterator* internal_iter =
      NewMergingIterator(&internal_comparator_, &list[0], list.size());
  versions_->current()->Ref();

  IterState* cleanup = new IterState(&mutex_, mem_, imm_, versions_->current());
  internal_iter->RegisterCleanup(CleanupIteratorState, cleanup, nullptr);

  *seed = ++seed_;
  mutex_.Unlock();
  return internal_iter;
}

Iterator* DBImpl::TEST_NewInternalIterator() {
  SequenceNumber ignored;
  uint32_t ignored_seed;
  return NewInternalIterator(ReadOptions(), &ignored, &ignored_seed);
}

int64_t DBImpl::TEST_MaxNextLevelOverlappingBytes() {
  MutexLock l(&mutex_);
  return versions_->MaxNextLevelOverlappingBytes();
}

Status DBImpl::GetValueFromTree(const ReadOptions& options, const Slice& key,
                                std::string* value) {
    mutex_l0_.lock();
  uint32_t firstFileNumber =
      cuckoo_filter_ == nullptr
          ? -1
          : cuckoo_filter_->minFileNumber.load(std::memory_order_relaxed);
  std::vector<std::shared_ptr<lbtree>> trees = Table_L0_;
  std::vector<std::shared_ptr<BloomFilterPolicy>> bloom_filters;
  if(BLOOM_FILTER){
    bloom_filters = bloom_filters_;
  }
  mutex_l0_.unlock();
  int pos;
  char* value_addr;
  uint32_t value_length;
  key_type rawKey = DecodeDBBenchFixed64(key.data());
  uint64_t startTime;
  uint64_t endTime;
  uint64_t startTime1;
  uint64_t endTime1;
  uint64_t startTime2;
  uint64_t endTime2;

  uint32_t fileNumber = 0;
  bool isOnL0 = true;
  // bool isOnL0Again = false;
  if (CUCKOO_FILTER && cuckoo_filter_->isValid()) {

      if (READ_TIME_ANALYSIS) {
        startTime1 = env_->NowMicros();
      }
      cuckoo_filter_->GetMax(key, &fileNumber);
      if (READ_TIME_ANALYSIS) {
        endTime1 = env_->NowMicros();
        readStats_.readCuckooTime += (endTime1 - startTime1);
        readStats_.readCuckooCount++;
      }
      readStats_.readCount++;
            if (fileNumber != 0 && fileNumber >= firstFileNumber) {
        fileNumber -= firstFileNumber;
        if (fileNumber < trees.size()) {

          if (READ_TIME_ANALYSIS) {
            startTime = env_->NowMicros();
          }
          value_addr = (char*)trees[fileNumber]->lookup(rawKey, &pos);
          if (READ_TIME_ANALYSIS) {
            endTime = env_->NowMicros();
            readStats_.readL0Time += (endTime - startTime);
            readStats_.readL0Count++;
          }

          if (value_addr != nullptr) {
            value_length = leveldb::DecodeFixed32(value_addr + VPAGE_KEY_SIZE);
            assert(value_length = 1000);
            *value = std::string(value_addr + 4 + VPAGE_KEY_SIZE, value_length);
            readStats_.readRight++;
            readStats_.readL0Found++;
            return Status::OK();
          }
                } else {
          readStats_.readWrong++;
          fileNumber = -1;
        }
                  } else if (fileNumber == 0) {
        readStats_.readNotFound++;
        // fileNumber = -1;
        isOnL0 = true;
            } else if (fileNumber < firstFileNumber) {
        readStats_.readExpire++;
        isOnL0 = false;
        // isOnL0Again = true;
      }
      // else{
      //   std::cout << "cuckoo_filter got wrong fileNumber : " << fileNumber <<
      //   std::endl;
      // }
  }

  if (READ_TIME_ANALYSIS) {
    startTime = env_->NowMicros();
  }
  if (isOnL0) {
      for (int i = 0; i < trees.size(); i++) {
        if (CUCKOO_FILTER) {
          if (i == fileNumber) {
            continue;
          }
        }
        if (BLOOM_FILTER) {
          if (READ_TIME_ANALYSIS) {
            startTime2 = env_->NowMicros();
          }
          bool result = bloom_filters[i]->KeyMayMatch(key.ToString(8));
          if (READ_TIME_ANALYSIS) {
            endTime2 = env_->NowMicros();
            readStats_.readBloomTime += (endTime2 - startTime2);
            readStats_.readBloomCount++;
          }
          if(!result){
            readStats_.bloomfilter++;
            continue;
          }else{
            readStats_.bloomNofilter++;
          }
        }
        value_addr = (char*)trees[i]->lookup(rawKey, &pos);
        if (READ_TIME_ANALYSIS) {
          // endTime = env_->NowMicros();
          // readStats_.readL0Time += (endTime - startTime);
          readStats_.readL0Count++;
        }

        if (value_addr != nullptr) {
          value_length = leveldb::DecodeFixed32(value_addr + VPAGE_KEY_SIZE);
          assert(value_length = 1000);
          *value = std::string(value_addr + 4 + VPAGE_KEY_SIZE, value_length);
          readStats_.readL0Found++;
          endTime = env_->NowMicros();
          readStats_.readL0Time += (endTime - startTime);
          return Status::OK();
        }
      }
  }
  if (READ_TIME_ANALYSIS) {
    endTime = env_->NowMicros();
    readStats_.readL0Time += (endTime - startTime);\
  }
    {
      std::lock_guard<std::mutex> lock(mutex_l1_);
      if (Table_LN_.empty() || Table_LN_[0] == nullptr) {
      return Status::NotFound("");
      }
      std::shared_ptr<lbtree> tree = Table_LN_[0];

      if (READ_TIME_ANALYSIS) {
        startTime = env_->NowMicros();
      }
      value_addr = (char*)Table_LN_[0]->lookup(rawKey, &pos);
      if (READ_TIME_ANALYSIS) {
        endTime = env_->NowMicros();
        readStats_.readL1Time += (endTime - startTime);
        readStats_.readL1Count++;
      }

      if (value_addr != nullptr) {
        value_length = leveldb::DecodeFixed32(value_addr + VPAGE_KEY_SIZE);
        assert(value_length = 1000);
        *value = std::string(value_addr + 4 + VPAGE_KEY_SIZE, value_length);
        readStats_.readL1Found++;
        return Status::OK();
      } else {
      // value_addr = (char*)Table_LN_[0]->lookup(rawKey, &pos);
      }
  }
  // if (isOnL0Again) {
  //     for (int i = 0; i < trees.size(); i++) {
  //     if (CUCKOO_FILTER) {
  //       if (i == fileNumber) {
  //         continue;
  //       }
  //     }
  //     value_addr = (char*)trees[i]->lookup(rawKey, &pos);
  //     if (value_addr != nullptr) {
  //       value_length = leveldb::DecodeFixed32(value_addr + VPAGE_KEY_SIZE);
  //       assert(value_length = 1000);
  //       *value = std::string(value_addr + 4 + VPAGE_KEY_SIZE, value_length);
  //       readStats_.readL0Found++;
  //       return Status::OK();
  //     }
  //     }
  // }
  return Status::NotFound("");
}

Status DBImpl::Get(const ReadOptions& options, const Slice& key,
                   std::string* value) {
  Status s;
  MutexLock l(&mutex_);
  SequenceNumber snapshot;
  if (options.snapshot != nullptr) {
    snapshot =
        static_cast<const SnapshotImpl*>(options.snapshot)->sequence_number();
  } else {
    snapshot = versions_->LastSequence();
  }

  auto NowMicros = []() {
    static constexpr uint64_t kUsecondsPerSecond = 1000000;
    struct ::timeval tv;
    ::gettimeofday(&tv, nullptr);
    return static_cast<uint64_t>(tv.tv_sec) * kUsecondsPerSecond + tv.tv_usec;
  };

  MemTable* mem = mem_;
  MemTable* imm = imm_;
  Version* current = versions_->current();
  mem->Ref();
  if (imm != nullptr) imm->Ref();
  current->Ref();

  bool have_stat_update = false;
  Version::GetStats stats;

  // Unlock while reading from files and memtables
  {
    mutex_.Unlock();
    // First look in the memtable, then in the immutable memtable (if any).
    LookupKey lkey(key, snapshot);
    if (mem->Get(lkey, value, &s, readStats_)) {
      // Done
    } else if (imm != nullptr && imm->Get(lkey, value, &s, readStats_)) {
      // Done
    } else if (GetValueFromTree(options, key, value).ok()) {
      // Done 
    } else {
      uint64_t startTime;
      uint64_t endTime;
      if(READ_TIME_ANALYSIS){
        startTime = NowMicros();
      }
      s = current->Get(options, lkey, value, &stats, cuckoo_filter_);
      if(READ_TIME_ANALYSIS){
        endTime = NowMicros();
        readStats_.readL2Time += (endTime - startTime);
        readStats_.readL2Count ++;
      }
      if(s.ok()){
        readStats_.readL2Found++;
      }
      have_stat_update = true;
    }
    mutex_.Lock();
  }

  if (have_stat_update && current->UpdateStats(stats)) {
    MaybeScheduleCompaction();
  }
  mem->Unref();
  if (imm != nullptr) imm->Unref();
  current->Unref();
  return s;
}

Iterator* DBImpl::NewIterator(const ReadOptions& options) {
  SequenceNumber latest_snapshot;
  uint32_t seed;
  Iterator* iter = NewInternalIterator(options, &latest_snapshot, &seed);
  return NewDBIterator(this, user_comparator(), iter,
                       (options.snapshot != nullptr
                            ? static_cast<const SnapshotImpl*>(options.snapshot)
                                  ->sequence_number()
                            : latest_snapshot),
                       seed);
}

void DBImpl::RecordReadSample(Slice key) {
  // MutexLock l(&mutex_);
  // if (versions_->current()->RecordReadSample(key)) {
  //   MaybeScheduleCompaction();
  // }
}

const Snapshot* DBImpl::GetSnapshot() {
  MutexLock l(&mutex_);
  return snapshots_.New(versions_->LastSequence());
}

void DBImpl::ReleaseSnapshot(const Snapshot* snapshot) {
  MutexLock l(&mutex_);
  snapshots_.Delete(static_cast<const SnapshotImpl*>(snapshot));
}

// Convenience methods
Status DBImpl::Put(const WriteOptions& o, const Slice& key, const Slice& val) {
  return DB::Put(o, key, val);
}

Status DBImpl::Delete(const WriteOptions& options, const Slice& key) {
  return DB::Delete(options, key);
}

Status DBImpl::Write(const WriteOptions& options, WriteBatch* updates) {
  Writer w(&mutex_);
  w.batch = updates;
  w.sync = options.sync;
  w.done = false;

  MutexLock l(&mutex_);
  writers_.push_back(&w);
  while (!w.done && &w != writers_.front()) {
    w.cv.Wait();
  }
  if (w.done) {
    return w.status;
  }

  // May temporarily unlock and wait.
  Status status = MakeRoomForWrite(updates == nullptr);
  uint64_t last_sequence = versions_->LastSequence();
  Writer* last_writer = &w;
  if (status.ok() && updates != nullptr) {  // nullptr batch is for compactions
    WriteBatch* write_batch = BuildBatchGroup(&last_writer);
    WriteBatchInternal::SetSequence(write_batch, last_sequence + 1);
    last_sequence += WriteBatchInternal::Count(write_batch);

    // Add to log and apply to memtable.  We can release the lock
    // during this phase since &w is currently responsible for logging
    // and protects against concurrent loggers and concurrent writes
    // into mem_.
    {
      mutex_.Unlock();
      if (status.ok()) {
        status = WriteBatchInternal::InsertInto(write_batch, mem_);
      }
      mutex_.Lock();
    }
    if (write_batch == tmp_batch_) tmp_batch_->Clear();

    versions_->SetLastSequence(last_sequence);
  }

  while (true) {
    Writer* ready = writers_.front();
    writers_.pop_front();
    if (ready != &w) {
      ready->status = status;
      ready->done = true;
      ready->cv.Signal();
    }
    if (ready == last_writer) break;
  }

  // Notify new head of write queue
  if (!writers_.empty()) {
    writers_.front()->cv.Signal();
  }

  return status;
}

// REQUIRES: Writer list must be non-empty
// REQUIRES: First writer must have a non-null batch
WriteBatch* DBImpl::BuildBatchGroup(Writer** last_writer) {
  mutex_.AssertHeld();
assert(!writers_.empty());
  Writer* first = writers_.front();
  WriteBatch* result = first->batch;
assert(result != nullptr);

  size_t size = WriteBatchInternal::ByteSize(first->batch);

  // Allow the group to grow up to a maximum size, but if the
  // original write is small, limit the growth so we do not slow
  // down the small write too much.
  size_t max_size = 1 << 20;
  if (size <= (128 << 10)) {
    max_size = size + (128 << 10);
  }

  *last_writer = first;
  std::deque<Writer*>::iterator iter = writers_.begin();
  ++iter;  // Advance past "first"
  for (; iter != writers_.end(); ++iter) {
    Writer* w = *iter;
    if (w->sync && !first->sync) {
      // Do not include a sync write into a batch handled by a non-sync write.
      break;
    }

    if (w->batch != nullptr) {
      size += WriteBatchInternal::ByteSize(w->batch);
      if (size > max_size) {
        // Do not make batch too big
        break;
      }

      // Append to *result
      if (result == first->batch) {
        // Switch to temporary batch instead of disturbing caller's batch
        result = tmp_batch_;
      assert(WriteBatchInternal::Count(result) == 0);
        WriteBatchInternal::Append(result, first->batch);
      }
      WriteBatchInternal::Append(result, w->batch);
    }
    *last_writer = w;
  }
  return result;
}

// REQUIRES: mutex_ is held
// REQUIRES: this thread is currently at the front of the writer queue
Status DBImpl::MakeRoomForWrite(bool force) {
  mutex_.AssertHeld();
assert(!writers_.empty());
  bool allow_delay = !force;
  Status s;
  while (true) {
    if (!bg_error_.ok()) {
      // Yield previous error
      s = bg_error_;
      break;
    } else if (allow_delay && l0_num_ >=
                                  maxMergeCount * ratio_L0_) {
      // We are getting close to hitting a hard limit on the number of
      // L0 files.  Rather than delaying a single write by several
      // seconds when we hit the hard limit, start delaying each
      // individual write by 1ms to reduce latency variance.  Also,
      // this delay hands over some CPU to the compaction thread in
      // case it is sharing the same core as the writer.
      mutex_.Unlock();
      // env_->SleepForMicroseconds(50);
      allow_delay = false;  // Do not delay a single write more than once
      mutex_.Lock();
    } else if (allow_delay && l0_num_ >=
                                  (maxMergeCount + L0BufferCount) * ratio_L0_ / 2) {
      // We are getting close to hitting a hard limit on the number of
      // L0 files.  Rather than delaying a single write by several
      // seconds when we hit the hard limit, start delaying each
      // individual write by 1ms to reduce latency variance.  Also,
      // this delay hands over some CPU to the compaction thread in
      // case it is sharing the same core as the writer.
      mutex_.Unlock();
      env_->SleepForMicroseconds(1000);
      allow_delay = false;  // Do not delay a single write more than once
      mutex_.Lock();
    } else if (!force &&
               (mem_->ApproximateMemoryUsage() <= curMemtableSize_)) {
      // There is room in current memtable
      break;
    } else if (imm_ != nullptr) {
      // We have filled up the current memtable, but the previous
      // one is still being compacted, so we wait.
      Log(options_.info_log, "Current memtable full; waiting...\n");
      background_work_finished_signal_.Wait();
    } else if (l0_num_ >= L0BufferCountMax * ratio_L0_ || usage_ > 95) {
      // There are too many level-0 files.
      Log(options_.info_log, "Too many L0 files; waiting...\n");
      space_signal_.Wait();
    } else {
      // Attempt to switch to a new memtable and trigger compaction of old
    assert(versions_->PrevLogNumber() == 0);
      uint64_t new_log_number = versions_->NewFileNumber();
      WritableFile* lfile = nullptr;
      s = env_->NewWritableFile(LogFileName(dbname_, new_log_number), &lfile);
      if (!s.ok()) {
        // Avoid chewing through file number space in a tight loop.
        versions_->ReuseFileNumber(new_log_number);
        break;
      }
      delete log_;
      delete logfile_;
      logfile_ = lfile;
      logfile_number_ = new_log_number;
      log_ = new log::Writer(lfile);
      imm_ = mem_;
      has_imm_.store(true, std::memory_order_release);
      mem_ = new MemTable(internal_comparator_);
      mem_->setPMAllocator(pmAlloc_,kv_total_);
      mem_->Ref();
      force = false;  // Do not force another compaction if have room
      MaybeScheduleCompaction();
    }
  }
  return s;
}

bool DBImpl::GetProperty(const Slice& property, std::string* value) {
  value->clear();

  MutexLock l(&mutex_);
  Slice in = property;
  Slice prefix("leveldb.");
  if (!in.starts_with(prefix)) return false;
  in.remove_prefix(prefix.size());

  if (in.starts_with("num-files-at-level")) {
    in.remove_prefix(strlen("num-files-at-level"));
    uint64_t level;
    bool ok = ConsumeDecimalNumber(&in, &level) && in.empty();
    if (!ok || level >= config::kNumLevels) {
      return false;
    } else {
      char buf[100];
      std::snprintf(buf, sizeof(buf), "%d",
                    versions_->NumLevelFiles(static_cast<int>(level)));
      *value = buf;
      return true;
    }
  } else if (in == "stats") {
    char buf[200];
    std::snprintf(buf, sizeof(buf),
                  "                               Compactions\n"
                  "Level  Files Size(MB) Time(sec) Read(MB) Write(MB) KeyIn KeyDrop\n"
                  "--------------------------------------------------\n");
    value->append(buf);
    for (int level = 0; level < config::kNumLevels; level++) {
      int files = versions_->NumLevelFiles(level);
      if(level == 0){
        files = Table_L0_.size();
      }else if(level == 1){
        files = Table_LN_.size();
      }
      if (stats_[level].micros > 0 || files > 0) {
        std::snprintf(buf, sizeof(buf), "%3d %8d %8.0f %9.0f %8.0f %9.0f %ld %ld\n",
                      level, files, versions_->NumLevelBytes(level) / 1048576.0,
                      stats_[level].micros / 1e6,
                      stats_[level].bytes_read / 1048576.0,
                      stats_[level].bytes_written / 1048576.0,
                      stats_[level].KeyIn,
                      stats_[level].KeyDrop);
        value->append(buf);
      }
    }
    return true;
  } else if (in == "sstables") {
    *value = versions_->current()->DebugString();
    return true;
  } else if (in == "approximate-memory-usage") {
    size_t total_usage = options_.block_cache->TotalCharge();
    if (mem_) {
      total_usage += mem_->ApproximateMemoryUsage();
    }
    if (imm_) {
      total_usage += imm_->ApproximateMemoryUsage();
    }
    char buf[50];
    std::snprintf(buf, sizeof(buf), "%llu",
                  static_cast<unsigned long long>(total_usage));
    value->append(buf);
    return true;
  } else if (in == "flush"){
    mutex_.Unlock();
    Flush();
    mutex_.Lock();
  }

  return false;
}

void DBImpl::GetApproximateSizes(const Range* range, int n, uint64_t* sizes) {
  // TODO(opt): better implementation
  MutexLock l(&mutex_);
  Version* v = versions_->current();
  v->Ref();

  for (int i = 0; i < n; i++) {
    // Convert user_key into a corresponding internal key.
    InternalKey k1(range[i].start, kMaxSequenceNumber, kValueTypeForSeek);
    InternalKey k2(range[i].limit, kMaxSequenceNumber, kValueTypeForSeek);
    uint64_t start = versions_->ApproximateOffsetOf(v, k1);
    uint64_t limit = versions_->ApproximateOffsetOf(v, k2);
    sizes[i] = (limit >= start ? limit - start : 0);
  }

  v->Unref();
}

// Default implementations of convenience methods that subclasses of DB
// can call if they wish
Status DB::Put(const WriteOptions& opt, const Slice& key, const Slice& value) {
  WriteBatch batch;
  batch.Put(key, value);
  return Write(opt, &batch);
}

Status DB::Delete(const WriteOptions& opt, const Slice& key) {
  WriteBatch batch;
  batch.Delete(key);
  return Write(opt, &batch);
}

DB::~DB() = default;

Status DB::Open(const Options& options, const std::string& dbname, DB** dbptr) {
  *dbptr = nullptr;
  Log(options.info_log, "open database :%s !!!\n", dbname.c_str());

  DBImpl* impl = new DBImpl(options, dbname);
  impl->mutex_.Lock();
  VersionEdit edit;
  // Recover handles create_if_missing, error_if_exists
  bool save_manifest = false;
  Status s = impl->Recover(&edit, &save_manifest);
  if (s.ok() && impl->mem_ == nullptr) {
    // Create new log and a corresponding memtable.
    uint64_t new_log_number = impl->versions_->NewFileNumber();
    WritableFile* lfile;
    s = options.env->NewWritableFile(LogFileName(dbname, new_log_number),
                                     &lfile);
    if (s.ok()) {
      edit.SetLogNumber(new_log_number);
      impl->logfile_ = lfile;
      impl->logfile_number_ = new_log_number;
      impl->log_ = new log::Writer(lfile);
      impl->mem_ = new MemTable(impl->internal_comparator_);
      impl->mem_->setPMAllocator(impl->pmAlloc_, impl->kv_total_);
      impl->mem_->Ref();
    }
  }
  if (s.ok() && save_manifest) {
    edit.SetPrevLogNumber(0);  // No older logs needed after recovery.
    edit.SetLogNumber(impl->logfile_number_);
    s = impl->versions_->LogAndApply(&edit, &impl->mutex_);
  }
  if (s.ok()) {
    impl->RemoveObsoleteFiles();
    impl->MaybeScheduleCompaction();
  }
  impl->mutex_.Unlock();
  if (s.ok()) {
  assert(impl->mem_ != nullptr);
    *dbptr = impl;
  } else {
    delete impl;
  }
  return s;
}

Snapshot::~Snapshot() = default;

Status DestroyDB(const std::string& dbname, const Options& options) {
  Env* env = options.env;
  std::vector<std::string> filenames;
  Status result = env->GetChildren(dbname, &filenames);
  if (!result.ok()) {
    // Ignore error in case directory does not exist
    return Status::OK();
  }

  FileLock* lock;
  const std::string lockname = LockFileName(dbname);
  result = env->LockFile(lockname, &lock);
  if (result.ok()) {
    uint64_t number;
    FileType type;
    for (size_t i = 0; i < filenames.size(); i++) {
      if (ParseFileName(filenames[i], &number, &type) &&
          type != kDBLockFile) {  // Lock file will be deleted at end
        Status del = env->RemoveFile(dbname + "/" + filenames[i]);
        if (result.ok() && !del.ok()) {
          result = del;
        }
      }
    }
    env->UnlockFile(lock);  // Ignore error since state is already gone
    env->RemoveFile(lockname);
    env->RemoveDir(dbname);  // Ignore error in case dir contains other files
  }
  return result;
}

}  // namespace leveldb
