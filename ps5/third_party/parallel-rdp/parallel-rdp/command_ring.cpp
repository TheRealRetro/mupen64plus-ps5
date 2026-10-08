/* Copyright (c) 2020 Themaister
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <chrono>
#include "command_ring.hpp"
#include "rdp_device.hpp"
#include "thread_id.hpp"
#include <assert.h>

namespace RDP
{
void CommandRing::init(
#ifdef PARALLEL_RDP_SHADER_DIR
		Granite::Global::GlobalManagersHandle global_handles_,
#endif
		CommandProcessor *processor_, unsigned count)
{
	assert((count & (count - 1)) == 0);
	teardown_thread();
	processor = processor_;
	ring.resize(count);
	write_count = 0;
	read_count = 0;
	published = 0;
#ifdef PARALLEL_RDP_SHADER_DIR
	global_handles = std::move(global_handles_);
#endif
	thr = std::thread(&CommandRing::thread_loop, this);
}

void CommandRing::teardown_thread()
{
	if (thr.joinable())
	{
		enqueue_command(0, nullptr);
		thr.join();
	}
}

CommandRing::~CommandRing()
{
	teardown_thread();
}

void CommandRing::drain()
{
	std::unique_lock<std::mutex> holder{lock};
	while (write_count != completed_count)
	{
		producer_waiting = true;
		cond_space.wait(holder);
	}
	producer_waiting = false;
}

void CommandRing::wait_for_room(std::unique_lock<std::mutex> &holder, size_t count)
{
	assert(count <= ring.size());
	if (write_count + count > read_count + ring.size())
	{
		const uint64_t t0 = __builtin_ia32_rdtsc();
		while (write_count + count > read_count + ring.size())
		{
			producer_waiting = true;
			cond_space.wait(holder);
		}
		producer_wait_ticks.fetch_add(__builtin_ia32_rdtsc() - t0, std::memory_order_relaxed);
	}
	producer_waiting = false;
}

void CommandRing::publish()
{
	published.store(write_count, std::memory_order_release);
	// Mupen64Plus PS5: wake the worker only when it sleeps, once.
	if (worker_waiting)
	{
		worker_waiting = false;
		wakeups.fetch_add(1, std::memory_order_relaxed);
		cond_data.notify_one();
	}
}

void CommandRing::enqueue_command(unsigned num_words, const uint32_t *words)
{
	std::unique_lock<std::mutex> holder{lock};
	wait_for_room(holder, num_words + 1);

	size_t mask = ring.size() - 1;
	ring[write_count++ & mask] = num_words;
	for (unsigned i = 0; i < num_words; i++)
		ring[write_count++ & mask] = words[i];

	publish();
}

void CommandRing::enqueue_commands(size_t count, const uint32_t *framed)
{
	if (count == 0)
		return;
	std::unique_lock<std::mutex> holder{lock};
	wait_for_room(holder, count);

	size_t mask = ring.size() - 1;
	for (size_t i = 0; i < count; i++)
		ring[write_count++ & mask] = framed[i];

	publish();
}

void CommandRing::thread_loop()
{
	Util::register_thread_index(0);

#ifdef PARALLEL_RDP_SHADER_DIR
	// Here to let the RDP play nice with full Granite.
	// When we move to standalone Granite, we won't need to interact with global subsystems like this.
	Granite::Global::set_thread_context(*global_handles);
	global_handles.reset();
#endif

	// Mupen64Plus PS5: every command queued so far is taken in one go, and processed outside the lock.
	std::vector<uint32_t> batch;
	batch.reserve(ring.size());
	size_t mask = ring.size() - 1;

	for (;;)
	{
		bool is_idle = false;
		uint64_t batch_end = 0;
		// Mupen64Plus PS5: spin a little (~40 us of PAUSEs on Zen 2) before sleeping, so the emulator rarely
		// has to wake this thread (a syscall each time). read_count is only written by this thread.
		if (published.load(std::memory_order_acquire) == read_count)
			for (unsigned i = 0; i < 2048 && published.load(std::memory_order_relaxed) == read_count; i++)
				__builtin_ia32_pause();
		{
			std::unique_lock<std::mutex> holder{lock};
			if (write_count == read_count)
			{
				worker_waiting = true;
				cond_data.wait_for(holder, std::chrono::microseconds(500), [this]() { return write_count > read_count; });
				worker_waiting = false;
			}
			if (write_count > read_count)
			{
				batch.resize(size_t(write_count - read_count));
				for (size_t i = 0; i < batch.size(); i++)
					batch[i] = ring[read_count++ & mask];
				batch_end = read_count;
				// room in the ring again
				if (producer_waiting)
					cond_space.notify_one();
			}
			else
			{
				// If we don't receive commands at a steady pace,
				// notify rendering thread that we should probably kick some work.
				batch.resize(1);
				batch[0] = 0;
				is_idle = true;
			}
		}

		if (is_idle)
		{
			uint32_t idle = uint32_t(Op::MetaIdle) << 24;
			processor->enqueue_command_direct(1, &idle);
			continue;
		}

		const uint64_t busy0 = __builtin_ia32_rdtsc();
		batches.fetch_add(1, std::memory_order_relaxed);
		words_in.fetch_add(batch.size(), std::memory_order_relaxed);
		bool quit = false;
		for (size_t i = 0; i < batch.size();)
		{
			uint32_t num_words = batch[i++];
			if (num_words == 0)
			{
				quit = true;
				break;
			}
			processor->enqueue_command_direct(num_words, batch.data() + i);
			i += num_words;
		}
		worker_busy_ticks.fetch_add(__builtin_ia32_rdtsc() - busy0, std::memory_order_relaxed);

		{
			std::lock_guard<std::mutex> holder{lock};
			completed_count = batch_end;
			if (producer_waiting)
				cond_space.notify_one();
		}
		if (quit)
			break;
	}
}
}
