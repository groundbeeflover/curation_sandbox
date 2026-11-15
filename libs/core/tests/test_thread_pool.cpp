// SPDX-License-Identifier: Apache-2.0

// cs::ThreadPool (thread_pool.hpp), from issue #68 (M1-15): a fixed number of threads,
// parallel_for over index ranges in chunks, cancellation by token. The tests check that every
// index runs exactly once in chunks of the given size, that work is spread over the pool's
// threads, that a result added up per chunk does not depend on the thread count, and that
// cancellation, exceptions and invalid arguments behave as the header says.

#include <cs/core/thread_pool.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using cs::CancelToken;
using cs::ThreadPool;

namespace {

using Chunk = std::pair<std::size_t, std::size_t>;

// Runs a loop on the pool and returns every chunk it was given, sorted by begin.
std::vector<Chunk> chunks_of(ThreadPool& pool, std::size_t count, std::size_t chunk_size,
                             bool* finished = nullptr) {
    std::mutex mutex;
    std::vector<Chunk> chunks;
    const bool all = pool.parallel_for(count, chunk_size, [&](std::size_t begin, std::size_t end) {
        const std::lock_guard lock(mutex);
        chunks.emplace_back(begin, end);
    });
    if (finished != nullptr) {
        *finished = all;
    }
    std::sort(chunks.begin(), chunks.end());
    return chunks;
}

// Checks that the chunks are consecutive, cover [0, count) and are chunk_size long, the last
// one possibly shorter.
void check_chunks(const std::vector<Chunk>& chunks, std::size_t count, std::size_t chunk_size) {
    const std::size_t expected_chunks = (count + chunk_size - 1) / chunk_size;
    REQUIRE(chunks.size() == expected_chunks);
    std::size_t next = 0;
    for (std::size_t c = 0; c < chunks.size(); ++c) {
        INFO("chunk " << c);
        CHECK(chunks[c].first == next);
        CHECK(chunks[c].second > chunks[c].first);
        const std::size_t length = chunks[c].second - chunks[c].first;
        if (c + 1 < chunks.size()) {
            CHECK(length == chunk_size);
        } else {
            CHECK(length == count - chunk_size * (expected_chunks - 1));
        }
        next = chunks[c].second;
    }
    CHECK(next == count);
}

// A value per index whose sum depends on the order of addition in its last bits: terms of very
// different sizes, so that a sum taken in another order (as threads would take it) differs.
double term(std::size_t i) {
    const double x = static_cast<double>(i);
    return std::sin(x) * 1e-3 + 1.0 / (x + 1.0) + (i % 7 == 0 ? 1e6 : 0.0);
}

// The sum of term(i) over [0, count), computed as the lux meter does: one slot per chunk, the
// slots added in chunk order afterwards.
double chunked_sum(ThreadPool& pool, std::size_t count, std::size_t chunk_size) {
    std::vector<double> slots((count + chunk_size - 1) / chunk_size, 0.0);
    pool.parallel_for(count, chunk_size, [&](std::size_t begin, std::size_t end) {
        double sum = 0.0;
        for (std::size_t i = begin; i < end; ++i) {
            sum += term(i);
        }
        slots[begin / chunk_size] = sum;
    });
    double total = 0.0;
    for (const double slot : slots) {
        total += slot;
    }
    return total;
}

} // namespace

TEST_CASE("ThreadPool: thread_count is the count it was made with, the caller included",
          "[core][thread_pool]") {
    const std::size_t threads = GENERATE(1u, 2u, 3u, 8u);
    ThreadPool pool(threads);
    CHECK(pool.thread_count() == threads);
}

TEST_CASE("ThreadPool: a thread count of 0 throws std::invalid_argument", "[core][thread_pool]") {
    CHECK_THROWS_AS(ThreadPool(0), std::invalid_argument);
}

TEST_CASE("ThreadPool: a chunk size of 0 throws std::invalid_argument", "[core][thread_pool]") {
    const std::size_t threads = GENERATE(1u, 4u);
    ThreadPool pool(threads);
    bool ran = false;
    CHECK_THROWS_AS(pool.parallel_for(10, 0, [&](std::size_t, std::size_t) { ran = true; }),
                    std::invalid_argument);
    CHECK_FALSE(ran);
}

TEST_CASE("ThreadPool: a count of 0 runs nothing and returns true", "[core][thread_pool]") {
    const std::size_t threads = GENERATE(1u, 4u);
    ThreadPool pool(threads);
    std::atomic<int> calls{0};
    CHECK(pool.parallel_for(0, 16, [&](std::size_t, std::size_t) { ++calls; }));
    CHECK(calls.load() == 0);
}

TEST_CASE("ThreadPool: chunks are consecutive, chunk_size long and cover [0, count)",
          "[core][thread_pool]") {
    const std::size_t threads = GENERATE(1u, 2u, 8u);
    ThreadPool pool(threads);
    INFO(threads << " threads");

    SECTION("a count that is not a multiple of the chunk size: the last chunk is shorter") {
        // 1000 = 142 x 7 + 6: 143 chunks, the last 6 long.
        bool finished = false;
        const auto chunks = chunks_of(pool, 1000, 7, &finished);
        CHECK(finished);
        check_chunks(chunks, 1000, 7);
        REQUIRE(!chunks.empty());
        CHECK(chunks.back() == Chunk{994, 1000});
    }
    SECTION("a count that is a multiple of the chunk size: every chunk is full") {
        const auto chunks = chunks_of(pool, 640, 64);
        check_chunks(chunks, 640, 64);
    }
    SECTION("a chunk size of 1: one chunk per index") {
        const auto chunks = chunks_of(pool, 37, 1);
        check_chunks(chunks, 37, 1);
    }
    SECTION("a count smaller than the chunk size: one chunk, [0, count)") {
        const auto chunks = chunks_of(pool, 5, 64);
        REQUIRE(chunks.size() == 1);
        CHECK(chunks.front() == Chunk{0, 5});
    }
    SECTION("the largest chunk size: one chunk, without overflow") {
        const auto chunks = chunks_of(pool, 10, std::numeric_limits<std::size_t>::max());
        REQUIRE(chunks.size() == 1);
        CHECK(chunks.front() == Chunk{0, 10});
    }
}

TEST_CASE("ThreadPool: every index runs exactly once", "[core][thread_pool]") {
    const std::size_t threads = GENERATE(1u, 3u, 8u);
    const std::size_t chunk_size = GENERATE(1u, 13u, 256u);
    INFO(threads << " threads, chunks of " << chunk_size);
    ThreadPool pool(threads);

    constexpr std::size_t count = 10'000;
    std::vector<std::atomic<int>> runs(count);
    CHECK(pool.parallel_for(count, chunk_size, [&](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            runs[i].fetch_add(1, std::memory_order_relaxed);
        }
    }));
    std::size_t wrong = 0;
    for (const auto& r : runs) {
        wrong += r.load() == 1 ? 0 : 1;
    }
    CHECK(wrong == 0);
}

TEST_CASE("ThreadPool(1) runs every chunk on the calling thread, in index order",
          "[core][thread_pool]") {
    ThreadPool pool(1);
    const auto caller = std::this_thread::get_id();
    std::vector<Chunk> order;
    bool other_thread = false;
    CHECK(pool.parallel_for(100, 9, [&](std::size_t begin, std::size_t end) {
        other_thread = other_thread || std::this_thread::get_id() != caller;
        order.emplace_back(begin, end); // no lock: only the caller runs chunks
    }));
    CHECK_FALSE(other_thread);
    // Unsorted: the order the chunks ran in.
    check_chunks(order, 100, 9);
}

TEST_CASE("ThreadPool: work is spread over every thread of the pool, the caller included",
          "[core][thread_pool]") {
    // As many chunks as threads, and each chunk waits until every thread is inside a chunk at
    // once. One thread cannot finish them alone, so the loop completes only if all thread_count
    // threads take a chunk each, concurrently. A shared deadline turns a pool that does not
    // spread its work into a failure after 20 s instead of a hang.
    const std::size_t threads = GENERATE(2u, 4u, 8u);
    INFO(threads << " threads");
    ThreadPool pool(threads);

    std::mutex mutex;
    std::condition_variable arrived;
    std::size_t inside = 0;
    bool timed_out = false;
    std::set<std::thread::id> ids;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);

    CHECK(pool.parallel_for(threads, 1, [&](std::size_t, std::size_t) {
        std::unique_lock lock(mutex);
        ids.insert(std::this_thread::get_id());
        ++inside;
        arrived.notify_all();
        if (!arrived.wait_until(lock, deadline, [&] { return inside >= threads; })) {
            timed_out = true;
        }
    }));

    CHECK_FALSE(timed_out);
    CHECK(ids.size() == threads);
    CHECK(ids.contains(std::this_thread::get_id()));
}

TEST_CASE("ThreadPool: a result added up per chunk does not depend on the thread count",
          "[core][thread_pool]") {
    // The pattern the lux meter uses (thread_pool.hpp): one slot per chunk, added in order. With
    // terms of very different sizes, an addition in any other order would change the last bits,
    // so equality is checked with ==.
    constexpr std::size_t count = 20'000;
    constexpr std::size_t chunk_size = 64;
    ThreadPool one(1);
    const double reference = chunked_sum(one, count, chunk_size);

    for (const std::size_t threads : {2u, 3u, 8u}) {
        ThreadPool pool(threads);
        for (int repeat = 0; repeat < 3; ++repeat) {
            INFO(threads << " threads, repeat " << repeat);
            CHECK(chunked_sum(pool, count, chunk_size) == reference);
        }
    }
}

TEST_CASE("ThreadPool: values written per index do not depend on the thread count",
          "[core][thread_pool]") {
    constexpr std::size_t count = 5'000;
    auto fill = [&](std::size_t threads) {
        ThreadPool pool(threads);
        std::vector<double> out(count, 0.0);
        pool.parallel_for(count, 17, [&](std::size_t begin, std::size_t end) {
            for (std::size_t i = begin; i < end; ++i) {
                out[i] = term(i);
            }
        });
        return out;
    };
    const auto reference = fill(1);
    CHECK(fill(4) == reference);
    CHECK(fill(8) == reference);
}

TEST_CASE("ThreadPool: a token cancelled before the call runs nothing and returns false",
          "[core][thread_pool]") {
    const std::size_t threads = GENERATE(1u, 4u);
    ThreadPool pool(threads);
    CancelToken token;
    token.cancel();
    CHECK(token.cancelled());
    std::atomic<int> calls{0};
    CHECK_FALSE(pool.parallel_for(100, 1, [&](std::size_t, std::size_t) { ++calls; }, &token));
    CHECK(calls.load() == 0);
}

TEST_CASE("ThreadPool: a token that is never cancelled runs every chunk and returns true",
          "[core][thread_pool]") {
    const std::size_t threads = GENERATE(1u, 4u);
    ThreadPool pool(threads);
    CancelToken token;
    std::atomic<std::size_t> covered{0};
    CHECK(pool.parallel_for(
        1000, 10, [&](std::size_t begin, std::size_t end) { covered += end - begin; }, &token));
    CHECK(covered.load() == 1000);
    CHECK_FALSE(token.cancelled());
}

TEST_CASE("ThreadPool(1): cancelling mid-way skips the chunks not yet started and returns false",
          "[core][thread_pool]") {
    // One thread runs the chunks in order, so cancelling inside chunk 10 lets exactly chunks 0 to
    // 10 run: chunk 10 was already running and finishes, the rest have not started.
    ThreadPool pool(1);
    CancelToken token;
    std::vector<std::size_t> ran;
    const bool all = pool.parallel_for(
        100, 1,
        [&](std::size_t begin, std::size_t) {
            ran.push_back(begin);
            if (begin == 10) {
                token.cancel();
            }
        },
        &token);
    CHECK_FALSE(all);
    REQUIRE(ran.size() == 11);
    for (std::size_t i = 0; i < ran.size(); ++i) {
        CHECK(ran[i] == i);
    }
}

TEST_CASE("ThreadPool: cancelling mid-way on several threads returns false and skips chunks",
          "[core][thread_pool]") {
    // The first chunk to run cancels. At most the chunks already running when it does, one per
    // thread, can still run, far fewer than the 10 000.
    ThreadPool pool(8);
    CancelToken token;
    constexpr std::size_t count = 10'000;
    std::atomic<std::size_t> ran{0};
    const bool all = pool.parallel_for(
        count, 1,
        [&](std::size_t, std::size_t) {
            ++ran;
            token.cancel();
        },
        &token);
    CHECK_FALSE(all);
    CHECK(ran.load() >= 1);
    CHECK(ran.load() < count);
}

TEST_CASE("ThreadPool(1): an exception from the body is rethrown and later chunks are skipped",
          "[core][thread_pool]") {
    ThreadPool pool(1);
    std::vector<std::size_t> ran;
    CHECK_THROWS_WITH(pool.parallel_for(100, 1,
                                        [&](std::size_t begin, std::size_t) {
                                            ran.push_back(begin);
                                            if (begin == 5) {
                                                throw std::runtime_error("chunk 5");
                                            }
                                        }),
                      "chunk 5");
    CHECK(ran.size() == 6);
}

TEST_CASE("ThreadPool: an exception on any thread is rethrown to the caller, and the pool still "
          "works afterwards",
          "[core][thread_pool]") {
    ThreadPool pool(8);
    std::atomic<int> calls{0};
    CHECK_THROWS_AS(pool.parallel_for(1000, 1,
                                      [&](std::size_t begin, std::size_t) {
                                          ++calls;
                                          if (begin % 3 == 1) {
                                              throw std::out_of_range("index " +
                                                                      std::to_string(begin));
                                          }
                                      }),
                    std::out_of_range);
    CHECK(calls.load() >= 1);

    // The pool is not left broken: the next loop runs everything.
    const auto chunks = chunks_of(pool, 500, 5);
    check_chunks(chunks, 500, 5);
}

TEST_CASE("ThreadPool: a parallel_for inside a chunk of the same pool does not deadlock",
          "[core][thread_pool]") {
    // thread_pool.hpp: each caller can always finish its own loop by itself.
    ThreadPool pool(4);
    std::atomic<std::size_t> inner{0};
    CHECK(pool.parallel_for(8, 1, [&](std::size_t, std::size_t) {
        pool.parallel_for(100, 10, [&](std::size_t begin, std::size_t end) {
            inner.fetch_add(end - begin, std::memory_order_relaxed);
        });
    }));
    CHECK(inner.load() == 8 * 100);
}

TEST_CASE("ThreadPool: loops started from two threads at once on one pool both complete",
          "[core][thread_pool]") {
    ThreadPool pool(4);
    constexpr std::size_t count = 20'000;
    std::vector<std::atomic<int>> a(count);
    std::vector<std::atomic<int>> b(count);
    auto run = [&](std::vector<std::atomic<int>>& runs) {
        pool.parallel_for(count, 50, [&](std::size_t begin, std::size_t end) {
            for (std::size_t i = begin; i < end; ++i) {
                runs[i].fetch_add(1, std::memory_order_relaxed);
            }
        });
    };
    std::thread other([&] { run(a); });
    run(b);
    other.join();

    std::size_t wrong = 0;
    for (std::size_t i = 0; i < count; ++i) {
        wrong += (a[i].load() == 1 && b[i].load() == 1) ? 0 : 1;
    }
    CHECK(wrong == 0);
}

TEST_CASE("ThreadPool: one pool runs many loops in a row", "[core][thread_pool]") {
    ThreadPool pool(4);
    std::size_t total = 0;
    for (std::size_t loop = 0; loop < 200; ++loop) {
        std::atomic<std::size_t> covered{0};
        pool.parallel_for(loop, 3, [&](std::size_t begin, std::size_t end) {
            covered.fetch_add(end - begin, std::memory_order_relaxed);
        });
        CHECK(covered.load() == loop);
        total += covered.load();
    }
    CHECK(total == 199 * 200 / 2);
}

TEST_CASE("ThreadPool: a pool that never ran a loop is destroyed without hanging",
          "[core][thread_pool]") {
    for (const std::size_t threads : {1u, 2u, 16u}) {
        ThreadPool pool(threads);
        CHECK(pool.thread_count() == threads);
    }
}

TEST_CASE("CancelToken: starts not cancelled, and cancelling is permanent", "[core][thread_pool]") {
    CancelToken token;
    CHECK_FALSE(token.cancelled());
    token.cancel();
    CHECK(token.cancelled());
    token.cancel();
    CHECK(token.cancelled());
}
