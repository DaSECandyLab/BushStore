# BushStore: Efficient B+Tree Group Indexing for LSM-Tree in Non-Volatile Memory.

&#160; &#160; &#160; &#160; BushStore is an adaptive and horizontally scalable Log-Structured Merge-Tree for NVM storage that mitigates write and read amplifications. This web page hosts the code and data used in the paper titled "BushStore: Efficient B+Tree Group Indexing for LSM-Tree in Non-Volatile Memory".

## Environment, Workloads and Evaluation

### 1. Environment

**Our** evaluation is based on the following configurations and versions, you can try others.

* Hardware configuration

```
[CPU] 18-core 2.60GHz Intel(R) Xeon(R) Gold 6240C CPUs with 24.75 MB cache * 2
[MEM] 2666MHz DDR4 DRAM (32GB * 12)
[NVM] 1.5TB Intel Optane DC PMMs (128GB * 12)
[SSD] INTEL SSDPEDME016T4F.
```

* Operating environment

```
[os] Ubuntu 20.04.4 LTS (GNU/Linux 5.4.0-155-generic x86_64)
[gcc] 9.4.0
[cmake] 3.22.0(>=3.9)
[snappy] 1.2.0
[ndctl]
```

### 2. Workloads

```
db_bench (BushStore/benchmarks/db_bench)
ycsb (BushStore/ycsbc)
```

### 3. Compilation

You can compile as follows, or just use the `run_db_bench.sh` script, which packages the complete compilation and testing process

1. Compiling the BushStore and `db_bench` tools

```
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release .. && make
```

2. Compiling YCSB

```
cd ycsbc
make
```

### 4. Running

It is recommended to run the test directly using `run_db_bench.sh`, which presets the parameters used in our experiments. Or you can use the following way to test manually

* Microbenchmark

For example, to test the random read/write of 80G 1KB value size data, we use the following parameters

```
./db_bench --benchmarks=fillrandom,readrandom,readseq,stats --num=83886080 --value_size=1024 --write_buffer_size=67108864 --max_file_size=134217728 --open_files=10000 --reads=-1 --pm_size=193273528320 --pm_path=/mnt/pmem0.1/pm_test --db=/mnt/pmem0.1/pm_test --bucket_nums=33554432 --use_pm=1 --threads=1 --flush_ssd=0 --throughput=0 --dynamic_tree=1 --write_batch=1 --gc_ratio=0.5
```

* YCSB

For example, to test YCSBC with 1KB value size, you can use the following approach.

```
cd ycsbc
./ycsbc ./input/1KB_ALL
```

For specific YCSB runtime parameters, you can read and modify ycsbc/workloads, ycsbc/inputs, ycsbc/db/leveldb_db.cc

* Full Test

You can run `run_db_bench.sh` directly and it will generate the output directory in the BushStore directory, which contains the results of running all the tests for the paper

```
./run_db_bench.sh
```

# 测试须知

 - 记得修改CMakeLists.txt里的CMAKE_C_COMPILER和CMAKE_CXX_COMPILER，需要支持C++17

 - 记得修改ycsbc文件夹里所有makefile的CC，需要与CMakeLists.txt里用的g++位置一样

 - 修改options.h和global.h以调整选项

 - ycsbc/input/里的23指的是下一行有23个参数

# 改成100B Key的问题

 - non-leaf node里需要与cache line对齐，一个cache line 64B，108B (key + pointer)就不可以对齐cache line，对内存不友好。

 - leaf node里一个internal key 108B，无法与cache对齐，原来是16B，可以与cache对齐

 - 108B在NVM上存在严重写放大，原来是256B放一堆16B，可以放满，现在256B放两个key，剩下空的放不了。

 - kpage里256B metadata的finger设计在大key场景下效率极低。其metadata设计不适配于大key。