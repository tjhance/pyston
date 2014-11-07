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

Arena gen1_arena((void*)0x1270000000L);
Arena gen2_arena((void*)0x2270000000L);
Arena large_arena((void*)0x3270000000L);

Heap global_heap;

#define ALLOCBYTES_PER_COLLECTION 10000000
static unsigned bytesAllocatedSinceCollection;
static __thread unsigned thread_bytesAllocatedSinceCollection;
void _majorCollectIfNeeded(size_t bytes) {
    if (bytesAllocatedSinceCollection >= ALLOCBYTES_PER_COLLECTION) {
        threading::GLPromoteRegion _lock;
        if (bytesAllocatedSinceCollection >= ALLOCBYTES_PER_COLLECTION) {
            runMajorCollection();
            bytesAllocatedSinceCollection = 0;
        }
    }

    thread_bytesAllocatedSinceCollection += bytes;
    if (thread_bytesAllocatedSinceCollection > ALLOCBYTES_PER_COLLECTION / 4) {
        bytesAllocatedSinceCollection += thread_bytesAllocatedSinceCollection;
        thread_bytesAllocatedSinceCollection = 0;
    }
}

GCAllocation* alloc(size_t bytes) {
    if (bytes <= sizes[NUM_BUCKETS - 1]) {
        return allocSmall(size);
    } else {
        return allocLarge(size);
    }
}

void* allocSmall(size_t size) {
    size_t full_size = size + sizeof(NurseryAllocation);

    _majorCollectIfNeeded(full_size);

    // Get the NurseryFragment, and check if we need to allocate one.
    NurseryFragment* fragment = *global_heap.threadFragments.get();
    if (fragment == NULL || !fragment->canAllocate(full_size)) {
        if (nursery.shouldCollect()) {
            runMinorColletion();
        }

        fragment = global_heap.nursery.allocateFragment();
        *global_heap.threadFragments.get() = fragment;
    }

    // Allocate the space.
    assert(fragment->canAllocate(full_size));
    NurseryAllocation *allocation = fragment->allocate(full_size);

    allocation->gc_flags = GC_FLAGS_EXISTS;
    allocation->len = full_size;

    return &allocation->user_data;
}

void* allocLarge(size_t size) {
    _majorCollectIfNeeded(size);

    LOCK_REGION(lock);

    size_t total_size = size + sizeof(LargeObj);
    total_size = (total_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    LargeObj* rtn = (LargeObj*)large_arena.doMmap(total_size);
    rtn->obj_size = size;

    rtn->next = large_head;
    if (rtn->next)
        rtn->next->prev = &rtn->next;
    rtn->prev = &large_head;
    large_head = rtn;

    return rtn->data;
}

}
}
