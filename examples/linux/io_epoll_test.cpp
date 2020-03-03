/*
 * Copyright 2019-present Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <unifex/config.hpp>
#if !UNIFEX_NO_EPOLL

#include <unifex/inplace_stop_token.hpp>
#include <unifex/just.hpp>
#include <unifex/let.hpp>
#include <unifex/linux/io_epoll_context.hpp>
#include <unifex/scheduler_concepts.hpp>
#include <unifex/scope_guard.hpp>
#include <unifex/sequence.hpp>
#include <unifex/sync_wait.hpp>
#include <unifex/transform.hpp>
#include <unifex/when_all.hpp>
#include <unifex/repeat.hpp>
#include <unifex/typed_via.hpp>
#include <unifex/with_query_value.hpp>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace unifex;
using namespace unifex::linuxos;
using namespace std::chrono_literals;

template <typename F>
auto lazy(F&& f) {
  return transform(just(), (F &&) f);
}
template <typename S>
auto discard(S&& s) {
  return transform(s, [](auto&&...){});
}

//! Seconds to warmup the benchmark
static constexpr int WARMUP_DURATION = 3;

//! Seconds to run the benchmark
static constexpr int BENCHMARK_DURATION = 10;

static constexpr unsigned char data[6] = {'h', 'e', 'l', 'l', 'o', '\n'};

int main() {
  io_epoll_context ctx;

  inplace_stop_source stopSource;
  std::thread t{[&] { ctx.run(stopSource.get_token()); }};
  scope_guard stopOnExit = [&]() noexcept {
    stopSource.request_stop();
    t.join();
  };

  auto scheduler = ctx.get_scheduler();
  try {
    {
      auto start = std::chrono::steady_clock::now();
      inplace_stop_source timerStopSource;
      sync_wait(
          when_all(
              transform(
                  schedule_at(scheduler, now(scheduler) + 1s),
                  []() { std::printf("timer 1 completed (1s)\n"); }),
              transform(
                  schedule_at(scheduler, now(scheduler) + 2s),
                  []() { std::printf("timer 2 completed (2s)\n"); }),
              transform(
                  schedule_at(scheduler, now(scheduler) + 1500ms),
                  [&]() {
                    std::printf("timer 3 completed (1.5s) cancelling\n");
                    timerStopSource.request_stop();
                  })),
          timerStopSource.get_token());
      auto end = std::chrono::steady_clock::now();

      std::printf(
          "completed in %i ms\n",
          (int)std::chrono::duration_cast<std::chrono::milliseconds>(
              end - start)
              .count());
    }
  } catch (const std::exception& ex) {
    std::printf("error: %s\n", ex.what());
  }

  auto [rPipe, wPipe] = open_pipe(scheduler);

  inplace_stop_source stopWrite;
  std::thread w{[&] { 
      const auto buffer = as_bytes(span{data});
      try {
        while (!stopWrite.stop_requested()) {
          sync_wait(
            async_write_some(wPipe, buffer), stopWrite.get_token());
        }
      } catch (const std::error_code& ec) {
        std::printf("async_write_some error: %s\n", ec.message().c_str());
      } catch (const std::exception& ex) {
        std::printf("async_write_some exception: %s\n", ex.what());
      }
    }};
  scope_guard waitForWrites = [&]() noexcept {
    stopWrite.request_stop();
    w.join();
  };

  inplace_stop_source stopWarmup;
  inplace_stop_source stopRead;
  auto buffer = std::vector<char>{};
  buffer.resize(1);
  auto offset = 0;
  auto reps = 0;
  auto pipe_bench = [&](int seconds, auto& stopSource) {
    return with_query_value(//just(), get_stop_token, stopSource.get_token());
      discard(
        when_all(
          transform(
            schedule_at(scheduler, now(scheduler) + std::chrono::seconds(seconds)), [&]{
              stopSource.request_stop();
            }),
          typed_via(
            repeat(
              transform(
                discard(
                  async_read_some(rPipe, as_writable_bytes(span{buffer.data() + 0, 1}))),
                [&]{
                  assert(data[(reps + offset)%sizeof(data)] == buffer[0]);
                  ++reps;
                })), 
            scheduler)
        )), get_stop_token, stopSource.get_token());
  };
  auto start = std::chrono::steady_clock::now();
  auto end = std::chrono::steady_clock::now();
  try {
    sync_wait(
      sequence(
        pipe_bench(WARMUP_DURATION, stopWarmup), // warmup
        lazy([&]{
          // restart reps and keep offset in data
          offset = reps%sizeof(data);
          reps = 0;
          // exclude the warmup time
          start = std::chrono::steady_clock::now();
          printf("warmup completed!\n");
        }),
        pipe_bench(BENCHMARK_DURATION, stopRead)), 
      stopRead.get_token());
    int ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
            end - start)
            .count();
    int ns = (int)std::chrono::duration_cast<std::chrono::nanoseconds>(
            end - start)
            .count();
    std::printf(
        "completed in %i ms, %ins-per-op, %iops-per-ms\n",
        ms,
        ns/reps,
        reps/ms);
  } catch (const std::error_code& ec) {
    std::printf("async_read_some error: %s\n", ec.message().c_str());
  } catch (const std::exception& ex) {
    std::printf("async_read_some exception: %s\n", ex.what());
  }

  return 0;
}

#else // !UNIFEX_NO_EPOLL
#include <cstdio>
int main() {
  printf("epoll support not found\n");
}
#endif // !UNIFEX_NO_EPOLL
