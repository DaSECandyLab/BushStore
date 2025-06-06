
#include <iostream>

#include "leveldb_db.h"
#include "leveldb/filter_policy.h"
#include "lib/coding.h"
#include "core/utils.h"
#include "leveldb/cache.h"

using namespace std;

namespace ycsbc {
    LevelDB::LevelDB(const char *dbfilename, utils::Properties &props) :noResult(0){
        
        //set option
        leveldb::Options options;
        SetOptions(&options, props);

        leveldb::Status s = leveldb::DB::Open(options, dbfilename, &db_);

        if(!s.ok()){
            cerr<<"Can't open leveldb "<<dbfilename<<endl;
            exit(0);
        }
    }
    void LevelDB::SetOptions(leveldb::Options *options, utils::Properties &props) {
        options->create_if_missing = true;
        options->compression = leveldb::kNoCompression;

        std::string pm_path = props.GetProperty("pmpath","/mnt/pmem0.8/guoteng");
        options->pm_path_ = pm_path;
        options->flush_ssd = utils::StrToBool(props["flushssd"]);
        if(options->flush_ssd){
            options->pm_size_ = 8ULL * 1024 * 1024 * 1024; // pmem size 8GB
        }
        options->filter_policy = leveldb::NewBloomFilterPolicy(4);
        options->block_cache = leveldb::NewLRUCache(134217728); // 128MB
        // printf("set MioDB options!\n");
        // options->nvm_node = 0;
        // options->nvm_next_node = -1;
    }

    int LevelDB::Read(const std::string &table, const std::string &key, const std::vector<std::string> *fields,
                      std::vector<KVPair> &result) {
        string value;
        leveldb::Status s = db_->Get(leveldb::ReadOptions(),key,&value);
        //printf("read:key:%lu-%s [%lu]\n",key.size(),key.data(),value.size());
        if(s.ok()) {
            //printf("value:%lu\n",value.size());
            DeSerializeValues(value, result);
            /* printf("get:key:%lu-%s\n",key.size(),key.data());
            for( auto kv : result) {
                printf("get field:key:%lu-%s value:%lu-%s\n",kv.first.size(),kv.first.data(),kv.second.size(),kv.second.data());
            } */
            return DB::kOK;
        }
        if(s.IsNotFound()){
            noResult++;
            //cerr<<"read not found:"<<noResult<<endl;
            return DB::kOK;
        }else{
            cerr<<"read error"<<endl;
            exit(0);
        }
    }


    int LevelDB::Scan(const std::string &table, const std::string &key, int len, const std::vector<std::string> *fields,
                      std::vector<std::vector<KVPair>> &result) {
        auto it=db_->NewIterator(leveldb::ReadOptions());
        it->Seek(key);
        std::string val;
        std::string k;
        for(int i=0;i<len && it->Valid();i++){
            k = it->key().ToString();
            val = it->value().ToString();
            it->Next();
        }
        delete it;
        return DB::kOK;
    }

    int LevelDB::Insert(const std::string &table, const std::string &key,
               std::vector<KVPair> &values){
        leveldb::Status s;
        string value;
        SerializeValues(values,value);
        //printf("put:key:%lu-%s [%lu]\n",key.size(),key.data(),value.size());
        /* for( auto kv : values) {
            printf("put field:key:%lu-%s value:%lu-%s\n",kv.first.size(),kv.first.data(),kv.second.size(),kv.second.data());
        } */ 
        
        s = db_->Put(leveldb::WriteOptions(), key, value);
        if(!s.ok()){
            cerr<<"insert error\n"<<endl;
            exit(0);
        }
        
        return DB::kOK;
    }

    int LevelDB::Update(const std::string &table, const std::string &key, std::vector<KVPair> &values) {
        std::string data;
        leveldb::Status s = db_->Get(leveldb::ReadOptions(),key,&data);
        if (s.IsNotFound()) {
            noResult++;
            return DB::kOK;
        } else if (!s.ok()) {
            // throw utils::Exception(std::string("RocksDB Get: ") + s.ToString());
            cerr<<"update error"<<endl;
            exit(0);
        }

        std::vector<KVPair> current_kvs;
        DeSerializeValues(data, current_kvs);
        for (auto &new_kv : values) {
            bool found __attribute__((unused)) = false;
            for (auto &cur_kv : current_kvs) {
                if (cur_kv.first == new_kv.first) {
                    found = true;
                    cur_kv.second = new_kv.second;
                    break;
                }
            }
        }

        leveldb::WriteOptions wopt;
        data.clear();
        SerializeValues(current_kvs, data);
        s = db_->Put(wopt, key, data);
        if (!s.ok()) {
            cerr<<"update error"<<endl;
            exit(0);
        }
        return DB::kOK;
        // return Insert(table,key,values);
    }

    int LevelDB::Delete(const std::string &table, const std::string &key) {
        leveldb::Status s;
        s = db_->Delete(leveldb::WriteOptions(),key);
        if(!s.ok()){
            cerr<<"Delete error\n"<<endl;
            exit(0);
        }
        return DB::kOK;
    }

    void LevelDB::PrintStats() {
        cout<<"read not found:"<<noResult<<endl;
        string stats;
        db_->GetProperty("leveldb.stats",&stats);
        cout<<stats<<endl;
    }

    // bool LevelDB::HaveBalancedDistribution() {
    //     return db_->HaveBalancedDistribution();
    // }

    LevelDB::~LevelDB() {
        delete db_;
    }

    void LevelDB::SerializeValues(std::vector<KVPair> &kvs, std::string &value) {
        value.clear();
        PutFixed64(&value, kvs.size());
        for(unsigned int i=0; i < kvs.size(); i++){
            PutFixed64(&value, kvs[i].first.size());
            value.append(kvs[i].first);
            PutFixed64(&value, kvs[i].second.size());
            value.append(kvs[i].second);
        }
    }

    void LevelDB::DeSerializeValues(std::string &value, std::vector<KVPair> &kvs){
        uint64_t offset = 0;
        uint64_t kv_num = 0;
        uint64_t key_size = 0;
        uint64_t value_size = 0;

        kv_num = DecodeFixed64(value.c_str());
        offset += 8;
        for( unsigned int i = 0; i < kv_num; i++){
            ycsbc::DB::KVPair pair;
            key_size = DecodeFixed64(value.c_str() + offset);
            offset += 8;

            pair.first.assign(value.c_str() + offset, key_size);
            offset += key_size;

            value_size = DecodeFixed64(value.c_str() + offset);
            offset += 8;

            pair.second.assign(value.c_str() + offset, value_size);
            offset += value_size;
            kvs.push_back(pair);
        }
    }
}
