// Copyright (c) 2026 Nicolae Popescu. MIT License.

/**
 * @brief Behaviour tests for untangle::executor.
 *
 * Unlike executor_smoke_test.cpp, which walks the pool through its scenarios and prints what
 * happened, every case here states an expectation and fails when it does not hold. Most are about a
 * call count or a call order, which is why the work added below is mostly a mock's method.
 *
 * @remark A few cases are sanitizer-sensitive. Configure with -DEXECUTOR_SANITIZE=thread or
 * =address to run them where a race or a dangling read is named rather than inferred.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <executor.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

// The pool under test, named once: executor is a template on its task type.
using executor = untangle::executor<std::function<void(void)>>;
using namespace std::chrono_literals;
using ::testing::InSequence;

/**
 * @brief Waits for a predicate to hold, so a defect is reported as a failure rather than a hang.
 *
 * @return true - the predicate held before the limit elapsed.
 */
template <typename predicateT>
bool wait_for(predicateT predicate, std::chrono::milliseconds limit) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

/**
 * @brief The work a pool is given, as a call expectation.
 *
 * gmock serialises the calls it records, so a mock may be submitted to every worker at once.
 */
struct mock_work {
  MOCK_METHOD(void, run, ());
  MOCK_METHOD(void, run_numbered, (int));
};

/**
 * @brief A task type that is not a std::function: a callable with a nested result_type.
 *
 * An execution reads that typedef off its action type rather than deducing it, and a bare lambda
 * has nowhere to put one.
 */
struct counting_task {
  using result_type = void;

  void operator()() const { ran->fetch_add(1, std::memory_order_relaxed); }

  std::atomic_int* ran = nullptr;
};

/**
 * @brief A task that stops on entry and stays there until it is released.
 *
 * A pool holding one per worker is fully occupied until the test says otherwise.
 */
class gate {
 public:
  //! The task to add. Blocks the worker that picks it up.
  void operator()() {
    arrived_.fetch_add(1, std::memory_order_relaxed);
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return open_; });
  }

  //! How many workers are standing at the gate.
  int arrived() const { return arrived_.load(std::memory_order_relaxed); }

  //! Lets every waiting worker through, and every later arrival straight past.
  void open() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      open_ = true;
    }
    cv_.notify_all();
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::atomic_int arrived_ = {0};
  bool open_ = false;
};

}  // namespace

/**
 * @brief Every added task is invoked, once each.
 *
 * The mock outlives the pool, so the expectation is verified after the destructor has finished.
 */
TEST(executor_tests, every_submitted_task_runs_exactly_once) {
  constexpr int task_count = 200;
  mock_work work;
  EXPECT_CALL(work, run()).Times(task_count);

  {
    executor pool(4);
    for (int i = 0; i < task_count; ++i) {
      EXPECT_TRUE(pool.add_task([&work] { work.run(); }));
    }
  }
}

/**
 * @brief The destructor does not return until the work already in flight has finished.
 *
 * The task is slow enough that a destructor which only stopped the workers would be caught.
 */
TEST(executor_tests, the_destructor_finishes_work_in_flight) {
  std::atomic_bool finished = {false};

  {
    executor pool(2);
    pool.add_task([&finished] {
      std::this_thread::sleep_for(200ms);
      finished = true;
    });
  }

  EXPECT_TRUE(finished.load());
}

/**
 * @brief One worker, so what comes back is the shared queue's own order.
 */
TEST(executor_tests, the_queue_preserves_submission_order) {
  constexpr int task_count = 50;
  mock_work work;

  {
    // InSequence is the ordering claim itself: any pair arriving the other way round fails.
    InSequence ordered;
    for (int i = 0; i < task_count; ++i) {
      EXPECT_CALL(work, run_numbered(i));
    }
  }

  {
    executor pool(1);
    for (int i = 0; i < task_count; ++i) {
      pool.add_task([&work, i] { work.run_numbered(i); });
    }
  }
}

/**
 * @brief A task added while the queue has work in it joins the back of it.
 *
 * Every worker is held at the gate, so there is no free worker to find and each task lands in the
 * queue in turn.
 */
TEST(executor_tests, tasks_queue_while_every_worker_is_busy) {
  constexpr std::size_t worker_count = 2;
  constexpr std::size_t queued_count = 5;

  gate busy;
  mock_work work;
  EXPECT_CALL(work, run()).Times(queued_count);

  {
    executor pool(worker_count);

    for (std::size_t i = 0; i < worker_count; ++i) {
      pool.add_task([&busy] { busy(); });
    }

    // Every worker must be at the gate first: one not yet started would be handed the task.
    ASSERT_TRUE(wait_for([&busy] { return busy.arrived() == static_cast<int>(worker_count); }, 5s));
    EXPECT_EQ(pool.pending(), 0u);

    for (std::size_t i = 0; i < queued_count; ++i) {
      pool.add_task([&work] { work.run(); });
      EXPECT_EQ(pool.pending(), i + 1);
    }

    busy.open();
  }
}

/**
 * @brief A task handed straight to a free worker, because the queue is empty.
 *
 * With nothing waiting, a task does not sit in the queue while a worker is idle.
 */
TEST(executor_tests, a_free_worker_takes_a_task_with_an_empty_queue) {
  gate busy;
  std::atomic_bool ran = {false};

  {
    executor pool(2);

    pool.add_task([&busy] { busy(); });
    ASSERT_TRUE(wait_for([&busy] { return busy.arrived() == 1; }, 5s));

    pool.add_task([&ran] { ran = true; });

    // The second worker is free and the queue empty, so this runs with the first still held.
    EXPECT_TRUE(wait_for([&ran] { return ran.load(); }, 5s));
    EXPECT_EQ(pool.pending(), 0u);

    busy.open();
  }
}

/**
 * @brief Work queued behind busy workers is picked up as they free up, without being prompted.
 */
TEST(executor_tests, workers_take_the_queue_as_they_free_up) {
  constexpr std::size_t worker_count = 2;
  constexpr std::size_t queued_count = 20;

  gate busy;
  std::atomic_int ran = {0};

  {
    executor pool(worker_count);

    for (std::size_t i = 0; i < worker_count; ++i) {
      pool.add_task([&busy] { busy(); });
    }
    ASSERT_TRUE(wait_for([&busy] { return busy.arrived() == static_cast<int>(worker_count); }, 5s));

    for (std::size_t i = 0; i < queued_count; ++i) {
      pool.add_task([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
    }
    ASSERT_EQ(pool.pending(), queued_count);

    busy.open();

    EXPECT_TRUE(wait_for([&ran] { return ran.load() == static_cast<int>(queued_count); }, 10s));
    EXPECT_EQ(pool.pending(), 0u);
  }
}

/**
 * @brief Tasks that wait run at the same time, not one after another.
 *
 * Four 150ms tasks on four workers, against a loose bound: a pool that serialised them costs 600ms.
 */
TEST(executor_tests, tasks_run_concurrently) {
  const auto started = std::chrono::steady_clock::now();

  {
    executor pool(4);
    for (int i = 0; i < 4; ++i) {
      pool.add_task([] { std::this_thread::sleep_for(150ms); });
    }
  }

  const auto elapsed = std::chrono::steady_clock::now() - started;
  EXPECT_LT(elapsed, 500ms);
}

/**
 * @brief Nothing is lost when several threads add tasks at once.
 */
TEST(executor_tests, concurrent_submission_loses_nothing) {
  constexpr int per_thread = 100;
  constexpr int submitters = 4;

  mock_work work;
  EXPECT_CALL(work, run()).Times(per_thread * submitters);

  {
    executor pool(3);

    std::vector<std::thread> threads;
    for (int t = 0; t < submitters; ++t) {
      threads.emplace_back([&pool, &work] {
        for (int i = 0; i < per_thread; ++i) {
          pool.add_task([&work] { work.run(); });
        }
      });
    }

    for (auto& one : threads) {
      one.join();
    }
  }
}

/**
 * @brief A task that throws does not take its worker with it.
 *
 * The worker carries on, so what this reads is the task after the throw.
 */
TEST(executor_tests, a_throwing_task_does_not_stop_the_worker) {
  mock_work work;
  EXPECT_CALL(work, run()).Times(3);

  {
    executor pool(1);  // one worker, so the task after the throw is the same worker's

    pool.add_task([&work] { work.run(); });
    pool.add_task([] { throw std::runtime_error("a task that throws"); });
    pool.add_task([&work] { work.run(); });
    pool.add_task([] { throw 42; });  // not a std::exception; the catch-all arm
    pool.add_task([&work] { work.run(); });
  }
}

/**
 * @brief What a task throws reaches the caller, and not only stderr.
 *
 * @remark Both arms are read: async catches a std::exception and anything else separately, and a
 * handler given only the first would leave the second as silent as it was.
 */
TEST(executor_tests, what_a_task_throws_reaches_the_caller) {
  std::atomic_int reported = {0};
  std::string reported_what;
  std::atomic_int reported_unknown = {0};

  {
    executor executor(1);

    executor.on_task_error = [&](std::exception_ptr thrown) {
      reported.fetch_add(1, std::memory_order_relaxed);

      try {
        std::rethrow_exception(thrown);
      } catch (const std::exception& e) {
        reported_what = e.what();
      } catch (...) {
        reported_unknown.fetch_add(1, std::memory_order_relaxed);
      }
    };

    executor.add_task([] { throw std::runtime_error("a task that throws"); });
    executor.add_task([] { throw 42; });  // not a std::exception; the catch-all arm
  }

  EXPECT_EQ(reported.load(), 2) << "the executor swallowed what the tasks threw";
  EXPECT_EQ(reported_what, "a task that throws");
  EXPECT_EQ(reported_unknown.load(), 1);
}

/**
 * @brief A task a worker refuses is not reported to the caller as taken.
 *
 * A stopped worker refuses what it is handed and destroys it, so a task answered for with true
 * would never run and pending() would not count it either.
 */
TEST(executor_tests, a_task_refused_by_a_worker_is_not_reported_as_taken) {
  std::atomic_int ran = {0};
  bool accepted = true;

  {
    executor executor(1);

    executor.stop();

    accepted = executor.add_task([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
  }

  EXPECT_FALSE(accepted) << "the worker refused the task and add_task() said it had been taken";
  EXPECT_EQ(ran.load(), 0) << "a refused task cannot have run";
}

/**
 * @brief A task may add more work, and it runs.
 *
 * The inner task is waited for inside the scope: once the destructor starts, the pool refuses the
 * work its own worker is about to add.
 */
TEST(executor_tests, a_task_can_add_more_work) {
  mock_work work;
  EXPECT_CALL(work, run()).Times(1);

  {
    executor pool(2);
    std::atomic_bool submitted = {false};

    pool.add_task(
        [&work, &pool, &submitted] { submitted = pool.add_task([&work] { work.run(); }); });

    ASSERT_TRUE(wait_for([&submitted] { return submitted.load(); }, 5s));
  }
}

/**
 * @brief A pool that is shutting down refuses the task and says so.
 *
 * The refusal is only reachable from inside the pool: add_task() answers false once the destructor
 * has stopped accepting, and the destructor is waiting for this very task to return. So the task
 * polls, and the case fails as a timeout rather than as a hang.
 */
TEST(executor_tests, add_task_is_refused_once_the_pool_is_shutting_down) {
  auto pool = std::make_unique<executor>(2);

  // The task reads the pool through a raw pointer, not through the unique_ptr: reset() clears the
  // pointer before it runs the destructor, so a task reading the unique_ptr would find it null
  // while the object it is running on is still very much alive.
  executor* const running_pool = pool.get();

  std::atomic_bool refused = {false};
  std::atomic_bool started = {false};

  running_pool->add_task([running_pool, &refused, &started] {
    started = true;
    refused = wait_for([running_pool] { return !running_pool->add_task([] {}); }, 10s);
  });

  ASSERT_TRUE(wait_for([&started] { return started.load(); }, 5s));

  pool.reset();  // blocks in ~executor until the task above returns

  EXPECT_TRUE(refused.load());
}

/**
 * @brief pending() is the queue's depth, not the pool's occupancy.
 */
TEST(executor_tests, pending_counts_only_what_is_waiting) {
  gate busy;

  {
    executor pool(1);

    pool.add_task([&busy] { busy(); });
    ASSERT_TRUE(wait_for([&busy] { return busy.arrived() == 1; }, 5s));

    // The task at the gate is the worker's, not the queue's.
    EXPECT_EQ(pool.pending(), 0u);

    pool.add_task([] {});
    EXPECT_EQ(pool.pending(), 1u);

    busy.open();

    EXPECT_TRUE(wait_for([&pool] { return pool.pending() == 0u; }, 5s));
  }
}

/**
 * @brief A pool nothing was submitted to still shuts down.
 */
TEST(executor_tests, an_idle_pool_shuts_down) {
  executor pool(4);
  EXPECT_EQ(pool.pending(), 0u);
}

/**
 * @brief A pool that can run nothing is refused before it exists.
 *
 * With no workers it would take work and then wait, in the destructor, for a queue nothing can
 * empty.
 */
TEST(executor_tests, a_pool_with_no_workers_is_refused) {
  EXPECT_THROW(executor(0), std::invalid_argument);
}

/**
 * @brief A destructor waiting on a task that has not returned reports why, and keeps waiting.
 *
 * The task holds the pool's only worker, so the closing brace is where this case spends its time.
 *
 * @remark The task times its own stall rather than waiting to be released, so a plain scope can
 * destroy the pool. `started` is declared before the pool so that it outlives the destructor.
 */
TEST(executor_tests, a_destructor_stuck_on_a_task_reports_why) {
  //! Longer than a first report is due, so the destructor is stuck across at least one.
  constexpr auto stall = 2s;

  testing::internal::CaptureStderr();

  {
    std::atomic_bool started = {false};
    executor pool(1);

    pool.add_task([&started, stall] {
      started.store(true);
      std::this_thread::sleep_for(stall);
    });

    EXPECT_TRUE(wait_for([&started] { return started.load(); }, 2s))
        << "the worker never reached the task, so the destructor is not stuck on it";
  }  // ~executor() waits out the rest of the task here, and has to say so while it does

  const std::string reported = testing::internal::GetCapturedStderr();

  EXPECT_THAT(reported, ::testing::HasSubstr("executor"))
      << "the stall went unreported; stderr held: " << reported;
  EXPECT_THAT(reported, ::testing::HasSubstr("waiting"))
      << "the stall went unreported; stderr held: " << reported;
}

/**
 * @brief A pool runs tasks that return a value, and drops what they return.
 *
 * A continuous worker collects nothing, so the count rather than the results is what this reads.
 */
TEST(executor_tests, a_pool_runs_tasks_that_return_a_value) {
  using int_executor = untangle::executor<std::function<int(void)>>;

  constexpr int tasks = 16;
  std::atomic_int ran = {0};

  {
    int_executor pool(2);

    for (int i = 0; i < tasks; ++i) {
      EXPECT_TRUE(pool.add_task([&ran] { return ran.fetch_add(1, std::memory_order_relaxed); }));
    }
  }

  EXPECT_EQ(ran.load(), tasks);
}

/**
 * @brief A pool runs a task type that is not a std::function.
 *
 * Any callable naming its own result_type will do, and such a pool never touches
 * std::function::result_type, which C++20 removed.
 */
TEST(executor_tests, a_pool_runs_a_task_type_that_is_not_a_std_function) {
  using counting_executor = untangle::executor<counting_task>;

  constexpr int tasks = 16;
  std::atomic_int ran = {0};

  {
    counting_executor pool(2);

    for (int i = 0; i < tasks; ++i) {
      EXPECT_TRUE(pool.add_task(counting_task{&ran}));
    }
  }

  EXPECT_EQ(ran.load(), tasks);
}

/**
 * @brief Destroying a pool while its workers are still turning over does not race them.
 *
 * Leaving the scope without waiting is what opens the window: the queue is still backed up when the
 * destructor starts, so workers are inside take_next_task() as it reaches the executions.
 *
 * @attention Sanitizer-sensitive, and only under -DEXECUTOR_SANITIZE=thread. A plain build cannot
 * observe the race and ASan does not report it, so passing on either proves nothing.
 *
 * @remark What it states on any build is the finish: every task added before the scope closed has
 * run by the time the destructor returns.
 */
TEST(executor_tests, destroying_a_pool_under_load_does_not_race_its_workers) {
  constexpr int rounds = 50;
  constexpr int tasks_per_round = 64;
  std::atomic_int ran = {0};

  for (int round = 0; round < rounds; ++round) {
    executor pool(4);

    for (int i = 0; i < tasks_per_round; ++i) {
      pool.add_task([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
    }

    // Deliberately no wait: a backed-up queue is what puts workers inside take_next_task().
  }

  EXPECT_EQ(ran.load(), rounds * tasks_per_round);
}
