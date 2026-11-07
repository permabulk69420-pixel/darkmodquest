/* The Dark Mod GPL Source Code. See LICENSE.txt for license terms. */
#pragma once
#include <atomic>
#include <cstdint>

// One instance belongs to one frontend worker. The shared arena owns all memory;
// this only reserves a small range to avoid an atomic operation per draw surface.
class FrameAllocCache {
public:
	static constexpr int BlockSize = 16 * 1024;
	static constexpr int Alignment = 128;
	// Android API 21 uses emulated TLS. Letting LLVM scalar-replace this object
	// creates a lookup for every field. Returning its address across a non-inlined
	// boundary keeps one TLS object and one lookup per allocation instead.
#ifdef _MSC_VER
	__declspec(noinline)
#else
	__attribute__((noinline))
#endif
	static FrameAllocCache &Local() {
		static thread_local FrameAllocCache cache;
		return cache;
	}
	unsigned char *Allocate(std::atomic<int> &allocated, unsigned char *memory,
		int capacity, uint64_t generation, int bytes) {
		if (storage != memory || epoch != generation) {
			storage = memory; epoch = generation; next = end = 0;
		}
		if (!bytes) return memory + allocated.load(std::memory_order_relaxed);
		if (bytes <= end - next) {
			unsigned char *result = memory + next;
			next += bytes;
			return result;
		}
		int start = allocated.load(std::memory_order_relaxed);
		for (;;) {
			const int remaining = capacity - start;
			if (bytes > remaining) return nullptr;
			// Near the arena limit reserve the remaining bytes, rather than
			// rejecting a valid small request because a whole block won't fit.
			int reserve = bytes > BlockSize ? bytes : (remaining < BlockSize ? remaining : BlockSize);
			reserve &= ~(Alignment - 1);
			if (allocated.compare_exchange_weak(start, start + reserve,
				std::memory_order_relaxed, std::memory_order_relaxed)) {
				if (bytes <= BlockSize) { next = start + bytes; end = start + reserve; }
				return memory + start;
			}
		}
	}
private:
	unsigned char *storage = nullptr;
	uint64_t epoch = 0;
	int next = 0, end = 0;
};

// Resets and debug rewinds must invalidate every worker's cached range.
void R_ResetFrameAlloc(int bytes);
