// Mupen64Plus PS5 frontend: the cheat search (fe_search.h).
//
// 8 MiB of RAM is 8,388,608 8-bit values: a step over all of them takes a few milliseconds. The search keeps one
// bit per value still in it (1 MiB at most) and a copy of the RAM from the last step, and the same once more for
// Undo.
//
// SPDX-License-Identifier: MIT

#include "fe_search.h"

#include <cstring>

namespace fe
{
namespace search
{
uint32_t Read(const uint32_t* ram, uint32_t address, int bytes)
{
	// the core keeps the RAM as host-order 32-bit words: byte n of the N64's memory is byte n ^ 3 of the array
	const uint8_t* b = reinterpret_cast<const uint8_t*>(ram);
	switch (bytes)
	{
		case 1: return b[address ^ 3];
		case 2:
		{
			uint16_t v;
			memcpy(&v, b + (address ^ 2), 2);
			return v;
		}
		default: return ram[address >> 2];
	}
}

uint32_t MaxValue(int bytes)
{
	return bytes == 1 ? 0xFFu : bytes == 2 ? 0xFFFFu : 0xFFFFFFFFu;
}

std::string Thousands(uint64_t n)
{
	std::string digits = std::to_string(n), out;
	for (size_t i = 0; i < digits.size(); i++)
	{
		if (i && (digits.size() - i) % 3 == 0)
			out += ',';
		out += digits[i];
	}
	return out;
}

void Search::Start(const uint32_t* ram, uint32_t ram_size, int bytes)
{
	Clear();
	m_bytes = bytes == 2 || bytes == 4 ? bytes : 1;
	m_size = ram_size & ~3u;
	const uint64_t n = m_size / uint32_t(m_bytes);
	m_bits.assign(size_t((n + 63) / 64), ~0ull);
	if (n % 64)
		m_bits.back() = (1ull << (n % 64)) - 1;
	m_count = n;
	m_before.assign(ram, ram + m_size / 4);
}

void Search::Filter(const uint32_t* ram, Test test, uint32_t number)
{
	if (!Active())
		return;
	m_undo_bits = m_bits;
	m_undo_before = m_before;
	m_undo_count = m_count;
	const uint32_t mask = MaxValue(m_bytes);
	number &= mask;
	const uint32_t* before = m_before.data();
	uint64_t count = 0;
	for (size_t w = 0; w < m_bits.size(); w++)
	{
		uint64_t bits = m_bits[w];
		while (bits)
		{
			const int i = __builtin_ctzll(bits);
			bits &= bits - 1;
			const uint32_t address = uint32_t((w * 64 + size_t(i)) * size_t(m_bytes));
			const uint32_t now = Read(ram, address, m_bytes), was = Read(before, address, m_bytes);
			bool keep;
			switch (test)
			{
				case Test::Equal: keep = now == number; break;
				case Test::NotEqual: keep = now != number; break;
				case Test::Greater: keep = now > number; break;
				case Test::Less: keep = now < number; break;
				case Test::Changed: keep = now != was; break;
				case Test::Unchanged: keep = now == was; break;
				case Test::Increased: keep = now > was; break;
				case Test::Decreased: keep = now < was; break;
				case Test::IncreasedBy: keep = ((now - was) & mask) == number && now != was; break;
				case Test::DecreasedBy: keep = ((was - now) & mask) == number && now != was; break;
				default: keep = true; break;
			}
			if (keep)
				count++;
			else
				m_bits[w] &= ~(1ull << i);
		}
	}
	m_count = count;
	m_before.assign(ram, ram + m_size / 4);
	m_steps++;
}

bool Search::Undo()
{
	if (!CanUndo())
		return false;
	m_bits.swap(m_undo_bits);
	m_before.swap(m_undo_before);
	m_count = m_undo_count;
	m_undo_bits.clear();
	m_undo_before.clear();
	m_steps = m_steps > 0 ? m_steps - 1 : 0;
	return true;
}

void Search::Clear()
{
	m_bytes = 0;
	m_size = 0;
	m_count = 0;
	m_steps = 0;
	std::vector<uint64_t>().swap(m_bits);
	std::vector<uint32_t>().swap(m_before);
	std::vector<uint64_t>().swap(m_undo_bits);
	std::vector<uint32_t>().swap(m_undo_before);
	m_undo_count = 0;
}

std::vector<Search::Result> Search::Results(size_t max) const
{
	std::vector<Result> out;
	for (size_t w = 0; w < m_bits.size() && out.size() < max; w++)
	{
		uint64_t bits = m_bits[w];
		while (bits && out.size() < max)
		{
			const int i = __builtin_ctzll(bits);
			bits &= bits - 1;
			const uint32_t address = uint32_t((w * 64 + size_t(i)) * size_t(m_bytes));
			out.push_back({address, Read(m_before.data(), address, m_bytes)});
		}
	}
	return out;
}
} // namespace search
} // namespace fe
