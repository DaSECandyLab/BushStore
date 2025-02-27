#ifndef STORAGE_LEVELDB_TABLE_BITMAP_H_
#define STORAGE_LEVELDB_TABLE_BITMAP_H_
#include<memory>
#include<iostream>
#include<cassert>
#include "util/global.h"

#include "bplustree/persist.h"

namespace leveldb{
class Bitmap{
public:
    uint64_t nums_;
    char bitmaps_[];

public:
    void set(size_t index){
        if(index > nums_) return ;
        int charIndex = (index >> 3);
        int innerIndex = (index & 7);
      // assert(!get(index));
        bitmaps_[charIndex] |= (1 << innerIndex);
        if(MALLO_CFLUSH){
            clflush(&bitmaps_[charIndex], 1);
        }
    };

    void clr(size_t index){
        if(index > nums_) return ;
        int charIndex = (index >> 3);
        int innerIndex = (index & 7);
      // assert(get(index));
        bitmaps_[charIndex] ^= (1 << innerIndex);
        if(MALLO_CFLUSH){
            clflush(&bitmaps_[charIndex], 1);
        }
    };

    bool get(size_t index){
        if(index > nums_) return 0;
        int charIndex = (index >> 3);
        int innerIndex = (index & 7);
        return (bitmaps_[charIndex] >> innerIndex) & 1;
    };

    void getEmpty(size_t &last_empty){
        while(get(last_empty)){
            last_empty = (last_empty + 1) % nums_;
            if(last_empty == 0){
                // std::cout<<"new allocator nums : "<< nums_ <<std::endl;
            }
        }
    }
};
}

#endif 