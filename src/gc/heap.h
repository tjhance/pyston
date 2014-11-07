// Copyright (c) 2014 Dropbox, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdint.h>
#include <sys/mman.h>

#include "core/common.h"
#include "core/util.h"
#include "gc/gc_alloc.h"

#ifndef NVALGRIND
#include "valgrind.h"
#endif

namespace pyston {
namespace gc {

#define PAGE_SIZE 4096
class Arena {
private:
    void* start;
    void* cur;

public:
    constexpr Arena(void* start) : start(start), cur(start) {}

    void* doMmap(size_t size) {
        assert(size % PAGE_SIZE == 0);

        void* mrtn = mmap(cur, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        assert((uintptr_t)mrtn != -1 && "failed to allocate memory from OS");
        ASSERT(mrtn == cur, "%p %p\n", mrtn, cur);
        cur = (uint8_t*)cur + size;
        return mrtn;
    }

    bool contains(void* addr) { return start <= addr && addr < cur; }

    void* getStart() {
        return start;
    }
};

extern Arena gen1_arena;
extern Arena gen2_arena;
extern Arena large_arena;

// Sizes for small objects
constexpr const size_t sizes[] = {
    16,  32,  48,  64,  80,  96,  112, 128,  160,  192,  224,  256,
    320, 384, 448, 512, 640, 768, 896, 1024, 1280, 1536, 1792, 2048,
    // 2560, 3072, 3584, // 4096,
};
#define NUM_BUCKETS (sizeof(sizes) / sizeof(sizes[0]))

struct LargeObj {
    LargeObj* next, **prev;
    size_t obj_size;
    GCAllocation data[0];

    int mmap_size() {
        size_t total_size = obj_size + sizeof(LargeObj);
        total_size = (total_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        return total_size;
    }

    int capacity() {
        return mmap_size() - sizeof(LargeObj);
    }

    static LargeObj* fromAllocation(GCAllocation* alloc) {
        char* rtn = (char*)alloc - offsetof(LargeObj, data);
        assert((uintptr_t)rtn % PAGE_SIZE == 0);
        return reinterpret_cast<LargeObj*>(rtn);
    }
};

// Segment of the nursery that can be allocated on
class NurseryFragment {
private:
    void* start, *end;

public:
    NurseryFragment() : start(NULL), end(NULL) { }
    NurseryFragment(void* start, void* end) : start(start), end(end) { }

    bool canAllocate(size_t len) {
        return start + len <= end;
    }

    void* allocate(size_t len) {
        assert(canAllocate(len));
        void* res = start;
        start += len;
        return res;
    }

    void* getStart() {
        return start;
    }
};

#define GC_FLAGS_MARKED 0x01
#define GC_FLAGS_PINNED 0x02

// MSB of the NurseryAllocation - distinguishes between 
#define GC_FLAGS_EXISTS 0x80

struct NurseryAllocation {
    union {
        void* fwd_ptr;
        struct {
            unsigned char gc_flags;
            unsigned short len;
        };
    };
    char user_data[0];
};
static_assert(sizeof(NurseryAllocation) == sizeof(void *), "bad NurseryAllocation size");
static_assert(sizes[NUM_BUCKETS - 1] <= (1 << sizeof(short)), "size may not fit in short 'len' field");


struct Heap {
    LargeObj* large_head = NULL;

    NurseryRoom nursery;
    PerThreadSet<NurseryFragment*> threadFragments;
};

GCAllocation* __attribute__((__malloc__)) alloc(size_t bytes);

GCAllocation* __attribute__((__malloc__)) allocSmall(size_t rounded_size, int bucket_idx);
GCAllocation* __attribute__((__malloc__)) allocLarge(size_t bytes);

extern Heap global_heap;

}
}
