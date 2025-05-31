cd ../build
make clean
cmake -DCMAKE_BUILD_TYPE=Release .. && make -j32 
cd ../ycsbc
make clean
make -j4
sh clean.sh
./ycsbc ./input/EXAMPLE