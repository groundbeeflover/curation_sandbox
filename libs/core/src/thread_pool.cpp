// SPDX-License-Identifier: Apache-2.0

#include <cs/core/thread_pool.hpp>

#include <algorithm>
#include <exception>
#include <stdexcept>

namespace cs {

// One call of parallel_for. Threads claim chunks by incrementing next_chunk; the caller waits
// until every claimed chunk has finished.
struct ThreadPool::Loop {
    std::size_t count = 0;
    std::size_t chunk_size = 1;
    std::size_t chunks = 0;
    const std::function<void(std::size_t, std::size_t)>* body = nullptr;
    const CancelToken* cancel = nullptr;

    std::atomic<std::size_t> next_chunk{0};
    std::atomic<bool> failed{false}; // a chunk threw: skip the rest
    std::atomic<bool> skipped{false};

    std::mutex mutex; // guards finished_chunks and error
    std::condition_variable finished;
    std::size_t finished_chunks = 0;
    std::exception_ptr error;

    bool has_chunks_left() const { return next_chunk.load(std::memory_order_relaxed) < chunks; }
};

ThreadPool::ThreadPool(std::size_t thread_count) {
    if (thread_count == 0) {
        throw std::invalid_argument("ThreadPool: thread_count must be at least 1");
    }
    workers_.reserve(thread_count - 1);
    try {
        for (std::size_t i = 1; i < thread_count; ++i) {
            workers_.emplace_back([this] { work(); });
        }
    } catch (...) {
        stop();
        throw;
    }
}

ThreadPool::~ThreadPool() {
    stop();
}

void ThreadPool::stop() {
    {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    for (auto& worker : workers_) {
        worker.join();
    }
}

void ThreadPool::run_chunks(Loop& loop) {
    std::size_t done = 0;
    std::exception_ptr error;
    for (;;) {
        const std::size_t chunk = loop.next_chunk.fetch_add(1, std::memory_order_relaxed);
        if (chunk >= loop.chunks) {
            break;
        }
        ++done;
        if (loop.failed.load(std::memory_order_relaxed) ||
            (loop.cancel != nullptr && loop.cancel->cancelled())) {
            loop.skipped.store(true, std::memory_order_relaxed);
            continue;
        }
        const std::size_t begin = chunk * loop.chunk_size;
        const std::size_t end = begin + std::min(loop.chunk_size, loop.count - begin);
        try {
            (*loop.body)(begin, end);
        } catch (...) {
            if (!error) {
                error = std::current_exception();
            }
            loop.failed.store(true, std::memory_order_relaxed);
        }
    }
    if (done == 0) {
        return;
    }
    // The last chunk to finish wakes the caller. Notifying under the lock keeps the loop alive
    // until the notification is made: the caller cannot return and destroy it before.
    const std::lock_guard lock(loop.mutex);
    if (error && !loop.error) {
        loop.error = error;
    }
    loop.finished_chunks += done;
    if (loop.finished_chunks == loop.chunks) {
        loop.finished.notify_all();
    }
}

void ThreadPool::work() {
    for (;;) {
        std::shared_ptr<Loop> loop;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] {
                if (stopping_) {
                    return true;
                }
                const auto it = std::find_if(loops_.begin(), loops_.end(),
                                             [](const auto& l) { return l->has_chunks_left(); });
                if (it == loops_.end()) {
                    return false;
                }
                loop = *it;
                return true;
            });
            if (!loop) {
                return; // stopping, with nothing left to help with
            }
        }
        run_chunks(*loop);
    }
}

bool ThreadPool::parallel_for(std::size_t count, std::size_t chunk_size,
                              const std::function<void(std::size_t begin, std::size_t end)>& body,
                              const CancelToken* cancel) {
    if (chunk_size == 0) {
        throw std::invalid_argument("ThreadPool::parallel_for: chunk_size must be at least 1");
    }
    if (count == 0) {
        return true;
    }

    auto loop = std::make_shared<Loop>();
    loop->count = count;
    loop->chunk_size = chunk_size;
    loop->chunks = count / chunk_size + (count % chunk_size != 0 ? 1 : 0);
    loop->body = &body;
    loop->cancel = cancel;

    const bool helped = loop->chunks > 1 && !workers_.empty();
    if (helped) {
        {
            const std::lock_guard lock(mutex_);
            loops_.push_back(loop);
        }
        wake_.notify_all();
    }

    run_chunks(*loop);

    {
        std::unique_lock lock(loop->mutex);
        loop->finished.wait(lock, [&] { return loop->finished_chunks == loop->chunks; });
    }
    if (helped) {
        const std::lock_guard lock(mutex_);
        loops_.erase(std::find(loops_.begin(), loops_.end(), loop));
    }

    if (loop->error) {
        std::rethrow_exception(loop->error);
    }
    return !loop->skipped.load(std::memory_order_relaxed);
}

} // namespace cs
