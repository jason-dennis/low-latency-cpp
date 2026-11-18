# Size-Class Allocator

A low-latency memory allocator in C++20 with O(1) `allocate` and `deallocate`, no loops, and no system calls after construction.

## Problem

Implement an allocator with the interface below. It has to be low latency, and it may request memory from the system **only once**, in the constructor.

```cpp
class Allocator {
public:
    Allocator(std::size_t capacity);
    std::byte* allocate(std::size_t size);   // 8 <= size <= 1024
    void deallocate(std::byte* ptr);
};
```

- `allocate` returns a pointer to the start of the allocated block, or `nullptr` if the request can't be satisfied.
- `deallocate` receives a pointer previously returned by `allocate`. It does **not** receive the size.

## Design

### Size classes

Every request is rounded up to the next power of two. With sizes in `[8, 1024]`, that gives 8 size classes:

| Index | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|---|
| Class size (bytes) | 8 | 16 | 32 | 64 | 128 | 256 | 512 | 1024 |

The class index is computed without branches or loops:

```
index = bit_width(size - 1) - 3        // e.g. 100 -> 128 = 2^7 -> index 4
```

### Header

`deallocate` only gets a pointer, so the allocator needs to know which class the block belongs to. Each block starts with an 8-byte header that stores the class index, and the user gets the address right after it. The header is 8 bytes (rather than 1) so returned pointers stay 8-byte aligned.

### Intrusive free lists

Each class has its own free list: a singly linked stack of freed blocks. The links are stored **inside the free blocks themselves**, in the bytes the header uses while the block is allocated. The free lists therefore cost no memory beyond 8 head pointers.

```
allocated:  [ header: class index ][ user data (class size) ]
             ^ block                ^ block + 8  (returned to the user)

free:       [ next free block     ][ unused                 ]
             ^ block  (linked from heads[index])
```

### Operations

**`allocate(size)`**
1. Reject sizes outside `[8, 1024]`.
2. Compute the class index.
3. If the class's free list isn't empty, pop its top block.
4. Otherwise, carve a new block from the untouched part of the buffer (bump pointer). Return `nullptr` if there's no room.
5. Write the class index into the header and return `block + 8`.

**`deallocate(ptr)`**
1. Step back 8 bytes to the block start.
2. Read the class index from the header.
3. Push the block onto that class's free list. This overwrites the header with the `next` link, which is why the index has to be read first.

## Complexity

| Operation | Time | Notes |
|---|---|---|
| `allocate` | O(1) | Bit operation, one pop or one bump |
| `deallocate` | O(1) | Pointer arithmetic, one push |
| Memory overhead | 8 B per block | Plus 8 head pointers and an offset |

## Trade-offs

- **Internal fragmentation:** rounding up to powers of two wastes up to ~50% of a block (a 513-byte request uses a 1024-byte block).
- **No coalescing:** freed blocks only return to their own class. A buffer full of freed 16-byte blocks can't serve a 512-byte request. This is accepted in exchange for constant-time operations.
- **Header overhead:** 8 bytes per allocation.
- **Not thread-safe:** designed for a single thread, which is typical for per-thread allocators on a hot path.

## Alternatives considered

- **Bump allocator:** fastest possible `allocate`, but freed memory is never reused.
- **Single fixed-size pool:** O(1) with no header, but every allocation would take 1024 bytes.
- **First-fit free list with coalescing:** less fragmentation, but `allocate` scans the list in O(n).
- **Headerless size classes:** split the buffer into regions per class and recover the class from the pointer's address. This removes the per-block header but partitions memory rigidly between classes.

## Build and run

```bash
g++ -std=c++20 -O2 -Wall -Wextra allocator.cpp -o allocator
./allocator
```

With sanitizers, to catch memory bugs:

```bash
g++ -std=c++20 -g -Wall -Wextra -fsanitize=address,undefined allocator.cpp -o allocator
./allocator
```

## Tests

`main` runs a self-contained test suite. It exits with code 0 when all tests pass and 1 otherwise. The tests cover:

- out-of-range sizes are rejected
- returned pointers are 8-byte aligned for every class
- live allocations never overlap
- freed blocks are reused within the same class, in LIFO order
- classes don't share blocks
- a full buffer returns `nullptr` and recovers after a free
- user writes don't corrupt the header
- 100,000 allocate/free cycles don't leak buffer space
- `deallocate(nullptr)` is a no-op
