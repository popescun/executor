// Copyright (c) 2026 Nicolae Popescu. MIT License.

/**
 * @brief Walks untangle::executor through the scenarios the prototype was written to answer, and
 * reports what it observed.
 *
 * Imported from the async repo's prototypes/executor_demo.cpp. Unlike executor_tests.cpp, which
 * states an expectation for every behaviour, these cases print what happened and assert only what
 * does not depend on how the machine schedules: a timing observation is recorded here and left for
 * a reader, because a threshold on it would fail on a loaded runner rather than on a defect.
 *
 * Meant to be run under ThreadSanitizer as well as plain.
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <executor.hpp>
#include <functional>
#include <mutex>
#include <print>
#include <thread>
#include <vector>

namespace {

// The pool under test, named once: executor is a template on its task type, the way the execution
// it runs tasks on is a template on its action type.
using executor = untangle::executor<std::function<void(void)>>;
using namespace std::chrono_literals;

}  // namespace

//! 200 tasks over 4 workers: every one runs, and the work lands on more than one thread.
TEST(executor_smoke_test, tasks_run_and_spread_across_workers) {
  constexpr int task_count = 200;
  std::atomic_int ran{0};

  std::mutex seen_mutex;
  std::vector<std::thread::id> seen;

  {
    executor pool(4);
    pool.start();
    for (int i = 0; i < task_count; ++i) {
      pool.add_task([&ran, &seen_mutex, &seen] {
        ran.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(seen_mutex);
        seen.push_back(std::this_thread::get_id());
      });
    }
  }  // the destructor finishes, then stops

  std::vector<std::thread::id> distinct;
  for (const auto& id : seen) {
    bool known = false;
    for (const auto& one : distinct) {
      known = known || (one == id);
    }
    if (!known) {
      distinct.push_back(id);
    }
  }

  std::println("1. {}/{} tasks ran, across {} worker thread(s)", ran.load(), task_count,
               distinct.size());

  EXPECT_EQ(ran.load(), task_count);

  // How many of the four a run touches is the scheduler's business; that the pool uses more than
  // the one thread it was called on is not.
  EXPECT_GT(distinct.size(), 1u);
}

//! One worker, so the order observed is the shared queue's own.
TEST(executor_smoke_test, submission_order_is_preserved) {
  std::mutex order_mutex;
  std::vector<int> order;

  {
    executor pool(1);
    pool.start();
    for (int i = 0; i < 50; ++i) {
      pool.add_task([i, &order_mutex, &order] {
        std::lock_guard<std::mutex> lock(order_mutex);
        order.push_back(i);
      });
    }
  }

  bool in_order = (order.size() == 50);
  for (std::size_t i = 0; in_order && i < order.size(); ++i) {
    in_order = (order[i] == static_cast<int>(i));
  }
  std::println("2. {} task(s) ran, submission order {}", order.size(),
               in_order ? "preserved" : "NOT preserved");

  EXPECT_EQ(order.size(), 50u);
  EXPECT_TRUE(in_order);
}

//! Four 150ms tasks on four workers. The elapsed time is printed, not asserted on.
TEST(executor_smoke_test, a_slow_task_does_not_hold_up_the_others) {
  std::atomic_int ran{0};
  const auto started = std::chrono::steady_clock::now();

  {
    executor pool(4);
    pool.start();
    for (int i = 0; i < 4; ++i) {
      pool.add_task([&ran] {
        std::this_thread::sleep_for(150ms);
        ran.fetch_add(1, std::memory_order_relaxed);
      });
    }
  }

  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  std::println("3. 4 x 150ms tasks on 4 workers took {}ms (serial would be ~600ms)",
               elapsed.count());

  EXPECT_EQ(ran.load(), 4);
}

//! 400 tasks pushed in from four threads at once.
TEST(executor_smoke_test, concurrent_submission) {
  constexpr int per_thread = 100;
  constexpr int submitters = 4;
  std::atomic_int ran{0};

  {
    executor pool(3);
    pool.start();
    std::vector<std::thread> threads;
    for (int t = 0; t < submitters; ++t) {
      threads.emplace_back([&pool, &ran] {
        for (int i = 0; i < per_thread; ++i) {
          pool.add_task([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
        }
      });
    }
    for (auto& one : threads) {
      one.join();
    }
  }

  std::println("4. {}/{} tasks ran, submitted from {} threads at once", ran.load(),
               per_thread * submitters, submitters);

  EXPECT_EQ(ran.load(), per_thread * submitters);
}

//! A pool that is still accepting takes the task.
TEST(executor_smoke_test, add_task_is_accepted_while_the_pool_is_up) {
  executor pool(2);
  pool.start();
  const bool accepted = pool.add_task([] {});

  std::println("5. add_task() while accepting: {}", accepted ? "accepted" : "refused");

  EXPECT_TRUE(accepted);
}
