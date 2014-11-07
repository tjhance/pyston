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

namespace pyston {
namespace gc {

NurseryRoom::NurseryRoom(Arena& arena) :
            arena(arena),
            arena_allocated_size(0),
            nFragments(NURSERY_ROOM_SIZE / STANDARD_FRAGMENT_SIZE) {
    for (int i = 0; i < NURSERY_ROOM_SIZE / STANDARD_FRAGMENT_SIZE; i++) {
         fragments[i] = NurseryFragment(arena.getStart(), STANDARD_FRAGMENT_SIZE);
    }
}

void NurseryRoom::minor_pin_object(void* addr, size_t len) {
    minor_pins.push_back(PinnedObject(addr, len));
}

void NurseryRoom::major_pin_object(void* addr, size_t len) {
    major_pins.push_back(PinnedObject(addr, len));
}

NurseryFragment* NurseryRoom::allocate_fragment() {
    assert(nextFreeFragment < nFragments);

    NurseryFragment* res = &fragments[nextFreeFragment];
    nextFreeFragment++;

    while (arena.getStart() + arena_allocated_size < res.getEnd()) {
        arena.doMmap(PAGE_SIZE);
        arena_allocated_size += PAGE_SIZE;
    }

    return res;
}

void NurseryRoom::release_minor() {
    release();
    minor_pins.clear();
}

void NurseryRoom::release_major() {
    release();
    minor_pins.clear();
    major_pins.clear();
}

bool fragmentCmp(NurseryFragment const& a, NurseryFragment const& b) {
    return a.getStart() < b.getStart();
}

void NurseryRoom::release() {
    // Sort the pinned objects
    vector<PinnedObject> pins = minor_pins;
    pins.insert(pins.end(), major_pins.begin(), major_pins.end());
    sort(pins.begin(), pins.end());

    // Sort the fragments which were allocated.
    sort(fragments, fragments + nextFreeFragment, fragmentCmp);

    // Break up fragments by pins
    int pinIndex = 0;
    for (int fragIndex = 0; fragIndex < nFragments; fragIndex++) {
        while (pinIndex < pins.size() && pins[pinIndex].addr < fragments[fragIndex].getStart()) {
            pinIndex++;
        }

        while (pinIndex < pins.size() && pins[pinIndex].addr < fragments[fragIndex].getEnd()) {
            void* start1 = fragments[fragIndex].getStart();
            void* end1 = pins[pinIndex].addr;
            void* start2 = pins[pinIndex].addr + len;
            void* end2 = fragments[fragIndex].getEnd();

            if (end1 - start1 <= MIN_FRAGMENT_SIZE) {
                fragments[nFragments] = NurseryFragment(start1, end1);
                nFragments++;
            }

            fragments[fragIndex] = NurseryFragment(start2, end2);

            pinIndex++;
        }
    }

    // TODO meld the fragments back together for objects that were un-pinned...

    nextFreeFragment = 0;
}

}
}
