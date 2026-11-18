#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>

// Low-latency size-class allocator.
//
// - Requests memory from the system exactly once (in the constructor).
// - Supports allocation sizes in [8, 1024] bytes.
// - Sizes are rounded up to the next power of two, giving 8 size classes:
//   8, 16, 32, 64, 128, 256, 512, 1024.
// - Each size class keeps its own intrusive free list (a singly linked stack)
//   of freed blocks, so both allocate() and deallocate() run in O(1).
//
// Block layout in the buffer:
//
//   allocated:  [ header (8 B): size-class index ][ user data (class size) ]
//                ^ block                           ^ block + HEADER (returned)
//
//   free:       [ next free block of same class   ][ unused                 ]
//                ^ block (stored in heads[index])
//
// Trade-offs: no coalescing of adjacent free blocks, internal fragmentation
// from power-of-two rounding, and 8 bytes of header per allocation.
// In exchange, every operation is a handful of instructions with no loops.
class Allocator {
private:
    std::size_t capacity;
    std::size_t offset{0};                  // bytes already carved from the buffer
    std::byte* memory;                      // the single buffer requested from the system
    std::array<std::byte*, 8> heads{};      // heads[i] = top of the free list for class i

    static constexpr std::size_t HEADER{8};  // keeps returned pointers 8-byte aligned
    static constexpr std::size_t LOWER_LIMIT{8}, UPPER_LIMIT{1024};

    // A free block's first 8 bytes hold a pointer to the next free block.
    // Returns a reference to that slot, so it can be both read and written:
    //   heads[i] = nextOf(block);   // read  (pop)
    //   nextOf(block) = heads[i];   // write (push)
    static std::byte*& nextOf(std::byte* block) {
        return *reinterpret_cast<std::byte**>(block);
    }

public:
    Allocator(std::size_t capacity) : capacity(capacity) {
        memory = new std::byte[capacity]{};
    }

    ~Allocator() {
        delete[] memory;
    }

    // Returns a pointer to at least `size` usable bytes, or nullptr if the
    // size is out of range or there is no space left.
    // 8 <= size <= 1024
    std::byte* allocate(std::size_t size) {
        if (size < LOWER_LIMIT || size > UPPER_LIMIT) return nullptr;

        // Size-class index: exponent of the next power of two, minus 3 (8 = 2^3).
        // e.g. size 100 -> 128 = 2^7 -> index 4.
        std::size_t index = std::bit_width(size - 1) - 3;

        // Space the block takes in the buffer: header + rounded class size.
        std::size_t total_size = HEADER + (8 << index);

        std::byte* block = heads[index];
        if (block != nullptr) {
            // Reuse a freed block: pop it from the class's free list.
            heads[index] = nextOf(block);
        }
        else {
            // Free list is empty: carve a new block from the untouched part of the buffer.
            if (offset + total_size > capacity) {
                return nullptr;
            }
            block = memory + offset;
            offset += total_size;
        }

        // Record the class in the header so deallocate() knows where to return the block.
        // (Must be rewritten every time: while the block was free, these bytes held `next`.)
        block[0] = static_cast<std::byte>(index);

        // The user's memory starts right after the header.
        return block + HEADER;
    }

    // Returns a block obtained from allocate() to its size class's free list.
    void deallocate(std::byte* ptr) {
        if (ptr == nullptr) return;

        // Step back over the header to get to the start of the block.
        std::byte* block = ptr - HEADER;

        // Read the class BEFORE pushing: the push overwrites the same bytes with `next`.
        std::size_t index = static_cast<std::size_t>(block[0]);

        // Push the block onto the free list of its class.
        nextOf(block) = heads[index];
        heads[index] = block;
    }
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static int failures = 0;

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::cout << "  FAILED: " #cond " (line " << __LINE__ << ")\n"; \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

static bool isAligned(std::byte* p, std::size_t alignment) {
    return reinterpret_cast<std::uintptr_t>(p) % alignment == 0;
}

// Sizes outside [8, 1024] must be rejected.
static void testOutOfRangeSizes() {
    Allocator a(4096);
    CHECK(a.allocate(0) == nullptr);
    CHECK(a.allocate(7) == nullptr);
    CHECK(a.allocate(1025) == nullptr);
    CHECK(a.allocate(8) != nullptr);
    CHECK(a.allocate(1024) != nullptr);
}

// Every returned pointer must be 8-byte aligned, for every size class.
static void testAlignment() {
    Allocator a(1 << 16);
    for (std::size_t size : {8, 9, 15, 16, 33, 100, 255, 513, 1024}) {
        std::byte* p = a.allocate(size);
        CHECK(p != nullptr);
        CHECK(isAligned(p, 8));
    }
}

// Live allocations must never overlap: fill each one with its own pattern,
// then verify no other allocation overwrote it.
static void testNoOverlap() {
    Allocator a(1 << 16);
    std::byte* ptrs[20];
    std::size_t sizes[20];
    for (int i = 0; i < 20; ++i) {
        sizes[i] = 8 + i * 50;
        ptrs[i] = a.allocate(sizes[i]);
        CHECK(ptrs[i] != nullptr);
        std::memset(ptrs[i], i + 1, sizes[i]);
    }
    for (int i = 0; i < 20; ++i) {
        for (std::size_t j = 0; j < sizes[i]; ++j) {
            if (ptrs[i][j] != static_cast<std::byte>(i + 1)) {
                CHECK(false && "allocation was overwritten");
                break;
            }
        }
    }
}

// A freed block must be handed out again for the next request of the same class.
static void testReuseSameClass() {
    Allocator a(4096);
    std::byte* p = a.allocate(100);   // class 128
    a.deallocate(p);
    CHECK(a.allocate(120) == p);      // also class 128 -> same block
}

// Free lists behave like stacks: last freed is first reused.
static void testLifoOrder() {
    Allocator a(4096);
    std::byte* p1 = a.allocate(64);
    std::byte* p2 = a.allocate(64);
    std::byte* p3 = a.allocate(64);
    a.deallocate(p1);
    a.deallocate(p2);
    a.deallocate(p3);
    CHECK(a.allocate(64) == p3);
    CHECK(a.allocate(64) == p2);
    CHECK(a.allocate(64) == p1);
}

// A freed block of one class must not be reused for a different class.
static void testClassesAreSeparate() {
    Allocator a(4096);
    std::byte* small = a.allocate(16);
    a.deallocate(small);
    std::byte* big = a.allocate(512);
    CHECK(big != nullptr);
    CHECK(big != small);
    CHECK(a.allocate(16) == small);
}

// When the buffer is full, allocate() returns nullptr; after a free it works again.
static void testExhaustion() {
    Allocator a(1032);                 // room for exactly one 1024-byte block (+ header)
    std::byte* p = a.allocate(1024);
    CHECK(p != nullptr);
    CHECK(a.allocate(8) == nullptr);   // nothing left
    a.deallocate(p);
    CHECK(a.allocate(1024) == p);      // reused
}

// Writing the whole user region must not corrupt the header,
// so the block still goes back to the right free list.
static void testUserWritesDoNotCorruptHeader() {
    Allocator a(4096);
    std::byte* p = a.allocate(64);
    std::memset(p, 0xFF, 64);
    a.deallocate(p);
    CHECK(a.allocate(64) == p);
}

// Many allocate/free cycles must not leak buffer space.
static void testRepeatedCyclesDoNotLeak() {
    Allocator a(2048);
    for (int i = 0; i < 100000; ++i) {
        std::byte* p = a.allocate(200);
        CHECK(p != nullptr);
        if (p == nullptr) return;
        a.deallocate(p);
    }
}

// deallocate(nullptr) must be a no-op.
static void testDeallocateNull() {
    Allocator a(4096);
    a.deallocate(nullptr);
    CHECK(a.allocate(8) != nullptr);
}

int main() {
    struct Test { const char* name; void (*fn)(); };
    const Test tests[] = {
        {"out of range sizes", testOutOfRangeSizes},
        {"alignment", testAlignment},
        {"no overlap", testNoOverlap},
        {"reuse same class", testReuseSameClass},
        {"LIFO order", testLifoOrder},
        {"classes are separate", testClassesAreSeparate},
        {"exhaustion", testExhaustion},
        {"user writes do not corrupt header", testUserWritesDoNotCorruptHeader},
        {"repeated cycles do not leak", testRepeatedCyclesDoNotLeak},
        {"deallocate nullptr", testDeallocateNull},
    };

    for (const Test& t : tests) {
        int before = failures;
        t.fn();
        std::cout << (failures == before ? "[PASS] " : "[FAIL] ") << t.name << '\n';
    }

    std::cout << (failures == 0 ? "\nAll tests passed.\n" : "\nSome tests failed.\n");
    return failures == 0 ? 0 : 1;
}
