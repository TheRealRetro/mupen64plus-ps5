// Mupen64Plus PS5 frontend: the cheat search -- finding where a game keeps a value (lives, health, money) in the
// N64's RAM, by narrowing every address down to the ones that changed the way the value did.
//
// A search looks at every 1, 2 or 4-byte value of the RAM (8, 16 or 32-bit; at addresses of that alignment).
// Start() remembers the RAM; each Filter() keeps the addresses whose value passes the test -- against a number,
// or against what it was the last time -- and remembers the RAM again. Undo() goes one step back.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fe
{
namespace search
{
enum class Test
{
	Equal, // == the number
	NotEqual,
	Greater, // > the number
	Less,
	Changed, // than the last time
	Unchanged,
	Increased,
	Decreased,
	IncreasedBy, // by the number
	DecreasedBy,
};

// the value at N64 RAM address `address` (bytes 1, 2 or 4), from RAM in the core's layout (n64_bridge.h)
uint32_t Read(const uint32_t* ram, uint32_t address, int bytes);
// the biggest value of that size
uint32_t MaxValue(int bytes);
// 1234567 -> "1,234,567"
std::string Thousands(uint64_t n);

class Search
{
public:
	bool Active() const { return m_bytes != 0; }
	int Bytes() const { return m_bytes; }
	uint64_t Count() const { return m_count; }
	int Steps() const { return m_steps; } // filters since Start
	bool CanUndo() const { return !m_undo_bits.empty(); }

	// a new search over the RAM (ram_size bytes) for values of `bytes` bytes: every address is in it
	void Start(const uint32_t* ram, uint32_t ram_size, int bytes);
	// keeps the addresses that pass `test` (with `number` where the test has one); the RAM is remembered again
	void Filter(const uint32_t* ram, Test test, uint32_t number);
	bool Undo();
	void Clear();

	struct Result
	{
		uint32_t address; // in the RAM, 0..size-1
		uint32_t before; // the value the last step saw
	};
	// the first `max` addresses still in the search, lowest first
	std::vector<Result> Results(size_t max) const;

private:
	int m_bytes = 0;
	uint32_t m_size = 0;
	uint64_t m_count = 0;
	int m_steps = 0;
	std::vector<uint64_t> m_bits; // one bit per address / bytes
	std::vector<uint32_t> m_before; // the RAM at the last step
	std::vector<uint64_t> m_undo_bits;
	std::vector<uint32_t> m_undo_before;
	uint64_t m_undo_count = 0;
};
} // namespace search
} // namespace fe
