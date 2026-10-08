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

#pragma once

#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>

#ifdef PARALLEL_RDP_SHADER_DIR
#include "global_managers.hpp"
#endif

namespace RDP
{
class CommandProcessor;
class CommandRing
{
public:
	void init(
#ifdef PARALLEL_RDP_SHADER_DIR
			Granite::Global::GlobalManagersHandle global_handles,
#endif
			CommandProcessor *processor, unsigned count);
	~CommandRing();
	void drain();

	void enqueue_command(unsigned num_words, const uint32_t *words);
	// Mupen64Plus PS5: several commands under one lock, framed as they sit in the ring (num_words, words...);
	// count is at most the ring's size.
	void enqueue_commands(size_t count, const uint32_t *framed);

	// Mupen64Plus PS5: where the time goes (rdtsc ticks), read and reset by the plugin's report.
	std::atomic<uint64_t> producer_wait_ticks{0}; // enqueue_command waiting for room
	std::atomic<uint64_t> worker_busy_ticks{0};   // the worker processing commands
	std::atomic<uint64_t> batches{0}, words_in{0};
	std::atomic<uint64_t> wakeups{0};             // the producer waking the sleeping worker

private:
	CommandProcessor *processor = nullptr;
	std::thread thr;
	std::mutex lock;
	// Mupen64Plus PS5: one condition variable each way, signalled only when the other side sleeps, and the
	// worker takes every queued command at once (command_ring.cpp): a wake-up per RDP command cost more
	// than the GPU's work on the PS5.
	std::condition_variable cond_data;  // the worker waits for commands
	std::condition_variable cond_space; // the producer waits for room, or for drain()
	bool worker_waiting = false;
	bool producer_waiting = false;
	std::atomic<uint64_t> published{0}; // write_count, for the worker's spin without the lock
	void wait_for_room(std::unique_lock<std::mutex> &holder, size_t count);
	void publish();

	std::vector<uint32_t> ring;
	uint64_t write_count = 0;
	uint64_t read_count = 0;
	uint64_t completed_count = 0;

	void thread_loop();
	void teardown_thread();
#ifdef PARALLEL_RDP_SHADER_DIR
	Granite::Global::GlobalManagersHandle global_handles;
#endif
};
}
