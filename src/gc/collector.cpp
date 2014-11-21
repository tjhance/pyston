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

#include "core/types.h"
#include "gc/heap.h"

namespace pyston {
namespace gc {

void GCVisitor::_visit(void** ref, void* p) {
    if (isNonheapRoot(p)) {
        return;
    } else {
        assert(global_heap.getAllocationFromInteriorPoint(p)->user_data == p);

        GCAllocation* al = GCAllocation::fromUserData(p);
        assert(al->user_data == p);

        if (!isMarked(al)) {
            setMark(al);
            v.push_back(p);
        }
    }
}

void GCVisitor::visit(void** ref) {
    _visit(ref, *ref);
}

void GCVisitor::visitRange(void* const* start, void* const* end) {
    while (start < end) {
        visit(start);
        start++;
    }
}

void GCVisitor::visitPotential(void** ref) {
    GCAllocation* a = global_heap.getAllocationFromInteriorPointer(ref);
    if (a) {
        _visit(ref, a->user_data);
    }
}

void GCVisitor::visitPotentialRange(void* const* start, void* const* end) {
    while (start < end) {
        visitPotential(start);
        start++;
    }
}

static std::vector<void*> permanent_roots;
void registerPermanentRoot(void* obj) {
    assert(global_heap.getAllocationFromInteriorPointer(obj));
    permanent_roots.push_back(obj);

    #ifndef NDEBUG
    // Check for double-registers.  Wouldn't cause any problems, but we probably shouldn't be doing them.
    static std::unordered_set<void*> root_set;
    ASSERT(root_set.count(obj) == 0, "Please only register roots once");
    root_set.insert(obj);
    #endif
}

void GCVisitor::run_trace() {
    assert(v.size() == 0);

    // Add roots
    for (void* p : permanent_roots) {
        assert(!isMarked(GCAllocation::fromUserData(p)));
    }
    v.extend(permanent_roots);

    // Conservatively scan stack, inserting roots into v
    collectStackRoots();
}

}
}
