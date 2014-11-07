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

#include "heap.h"

namespace pyston {
namespace gc {

#define STANDARD_FRAGMENT_SIZE 8192
#define MIN_FRAGMENT_SIZE 512

class NurseryRoom {
    public:
        NurseryRoom(Arena &arena);

        NurseryFragment* allocate_fragment();

        void release_minor();
        void release_major();

        // Pins an object for the minor collection.
        void minor_pin_object(void* addr, size_t len);

        // For pinning an object that should remain pinned until the next *major* collection.
        void major_pin_object(void* addr, size_t len);

    private:
        Arena& arena;
        size_t arena_allocated_size;

        NurseryFragment fragments[NURSERY_ROOM_SIZE / MIN_FRAGMENT_SIZE];
        int nFragments;
        int nextFreeFragment;

        struct PinnedObject {
            void* addr;
            size_t len;
            PinnedObject(void* addr, size_t len) : addr(addr), len(len) { }
            bool operator<(PinnedObject const& other) const {
                return addr < other.addr;
            }
        };

        vector<PinnedObject> minor_pins;
        vector<PinnedObject> major_pins;

        void release();
};

}
}
