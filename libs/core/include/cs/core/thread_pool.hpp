// SPDX-License-Identifier: Apache-2.0

#pragma once

// A fixed set of threads for data-parallel work
// ---------------------------------------------
// ThreadPool(n) runs loops over index ranges on n threads: n - 1 workers it starts once and
// keeps, plus the thread that calls parallel_for, which works too rather than waiting idle. So
// ThreadPool(1) runs everything on the caller, in order, and nested or concurrent calls to
// parallel_for never deadlock: each caller can always finish its own loop by itself.
//
// Which thread runs which index is not fixed, so a result must not depend on it. Code that adds
// up results (the lux meter) gives each chunk its own slot and adds the slots in index order
// afterwards, so that its result is the same, bit for bit, whatever the thread count.

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace cs {

// Asks running work to stop. Shared between whoever may cancel (a UI, a newer edit) and the
// loops that check it; cancelling is permanent for the token.
class CancelToken {
  public:
    void cancel() { cancelled_.store(true, std::memory_order_relaxed); }
    bool cancelled() const { return cancelled_.load(std::memory_order_relaxed); }

  private:
    std::atomic<bool> cancelled_{false};
};

class ThreadPool {
  public:
    // thread_count threads run each loop, including the caller of parallel_for. Throws
    // std::invalid_argument when it is 0.
    explicit ThreadPool(std::size_t thread_count);

    // Waits for the workers to finish what they are running and stops them. No parallel_for may
    // be running.
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    std::size_t thread_count() const { return workers_.size() + 1; }

    // Calls body(begin, end) for consecutive chunks [begin, end) that cover [0, count), each at
    // most chunk_size long (the last may be shorter), spread over the pool's threads; returns
    // when every chunk has run. Chunks start in index order but run concurrently, so body must
    // be safe to call from several threads at once.
    //
    // A chunk not yet started when `cancel` is cancelled is skipped; chunks already running
    // finish. Returns true when every chunk ran, false when some were skipped.
    //
    // If body throws, the chunks not yet started are skipped and the first exception is rethrown
    // here once the running ones have finished.
    //
    // Throws std::invalid_argument when chunk_size is 0. A count of 0 runs nothing.
    bool parallel_for(std::size_t count, std::size_t chunk_size,
                      const std::function<void(std::size_t begin, std::size_t end)>& body,
                      const CancelToken* cancel = nullptr);

  private:
    struct Loop;

    void work();
    static void run_chunks(Loop& loop);

    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable wake_;            // workers wait here for a loop with chunks left
    std::deque<std::shared_ptr<Loop>> loops_; // loops being run, oldest first
    bool stopping_ = false;
};

} // namespace cs
