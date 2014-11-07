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

static bool is_forwarded(void *p) {
    return !((*(unsigned char *)p) & 0x40);
}

static void* forward_obj(void *p) {
    assert(!is_forwarded(p));
    size_t len = get_allocation_len(p);
    void* newp = allocAdult(len);
    memcpy(newp, p, len);
    *((void**)p) = newp;
    return newp;
}

static void* get_forward(void *p) {
    assert(is_forwarded(p));
    return *((void **)p);
}

static bool is_marked(void *p) {
    GCAllocation* allocation = p;
    return !!(p->flags & GC_FLAGS_MARKED);
}

static bool is_pinned(void *p) {
    GCAllocation* allocation = p;
    return !!(p->flags & GC_FLAGS_PINNED);
}

static bool mark(void *p) {
    GCAllocation* allocation = p;
    p->flags |= GC_FLAGS_MARKED;
}

class MinorGCVisitor : GCVisitor {
    virtual void handleNursery(void** ref, void* p) {
        // Move object to the older generation
        if (is_forwarded(p)) {
            p = get_forward(p);
            *ref = p;
        } else {
            if (!is_pinned(p)) {
                p = forward_obj(p);
                *ref = p;
            }
        }

        if (!is_marked(p)) {
            mark(p);
            this->visitNeighbors(p);
        }
    }

    virtual void handleOld(void** ref, void* p) {
        // do nothing
        return;
    }
};

static void movePhase() {
    TraceStack stack(roots);
    collectStackRoots(&stack);

    GCVisitor visitor(&stack);
}

static int nMinorCollections = 0;
void runMinorCollection() {
    // TODO mark pinned objects

    static StatCounter sc("gc_collections");
    sc.log();

    nMinorCollections++;

    if (VERBOSITY("gc") >= 2)
        printf("Collection #%d\n", ncollections);

    Timer _t("collecting", /*min_usec=*/10000);

    markPins();
    movePhase();

    if (VERBOSITY("gc") >= 2)
        printf("Collection #%d done\n\n", ncollections);

    long us = _t.end();
    static StatCounter sc_us("gc_collections_us");
    sc_us.log(us);
}

}
}
