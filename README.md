# low-latency-cpp

Low-latency C++ building blocks: custom allocators, lock-free structures, and more, each with design write-ups and tests.

Every component lives in its own folder, with a README covering the problem, the design, the trade-offs, and how to run the tests.

## Components

| Component | Description |
|---|---|
| [Size-class allocator](allocator/) | O(1) allocator with power-of-two size classes and intrusive free lists, using a single upfront memory request |

## Requirements

- A C++20 compiler (GCC 10+ or Clang 12+)
