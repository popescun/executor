// Copyright (c) 2026 Nicolae Popescu. MIT License.

/**
 * @brief Behaviour tests for untangle::executor.
 *
 * Unlike executor_smoke_test.cpp, which walks the pool through its scenarios and prints what
 * happened, every case here states an expectation and fails when it does not hold.
 *
 * A task is a callable the pool is meant to invoke, so what most of these cases are really about is
 * a call count and a call order. Both are gmock's to state rather than the test's to tally, which
 * is why the work submitted below is mostly a mock's method.
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

// The pool under test, named once: executor is a template on its task type, the way the execution
// it runs tasks on is a template on its action type.
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
 * What an execution requires of its action type is the typedef, which it reads rather than deduces.
 * A bare lambda has nowhere to put one; this does, which is what makes it a task type a pool can be
 * built on.
 */
struct counting_task {
  using result_type = void;

  void operator()() const { ran->fetch_add(1, std::memory_order_relaxed); }

  std::atomic_int* ran = nullptr;
};

/**
 * @brief A task that stops on entry and stays there until it is released.
 *
 * What makes a busy worker something a test can arrange rather than wait out: a pool holding one of
 * these per worker is fully occupied, and stays that way until the test says otherwise.
 */
class gate {
 public:
  //! The task to submit. Blocks the worker that picks it up.
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
 * @brief Every submitted task is invoked, once each.
 *
 * The mock outlives the pool, so the expectation is verified after the destructor has finished -
 * which is also what makes this a test of the finish.
 */
TEST(executor_tests, every_submitted_task_runs_exactly_once) {
  constexpr int task_count = 200;
  mock_work work;
  EXPECT_CALL(work, run()).Times(task_count);

  {
    executor pool(4);
    for (int i = 0; i < task_count; ++i) {
      EXPECT_TRUE(pool.submit([&work] { work.run(); }));
    }
  }
}

/**
 * @brief The destructor does not return until the work already in flight has finished.
 *
 * Long enough that a destructor which only stopped the workers would be caught leaving a task
 * unrun, rather than winning the race by chance.
 */
TEST(executor_tests, the_destructor_finishes_work_in_flight) {
  std::atomic_bool finished = {false};

  {
    executor pool(2);
    pool.submit([&finished] {
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
    // InSequence is the ordering claim itself: any pair of calls arriving the other way round is a
    // failure, rather than something the test has to reconstruct from a vector afterwards.
    InSequence ordered;
    for (int i = 0; i < task_count; ++i) {
      EXPECT_CALL(work, run_numbered(i));
    }
  }

  {
    executor pool(1);
    for (int i = 0; i < task_count; ++i) {
      pool.submit([&work, i] { work.run_numbered(i); });
    }
  }
}

/**
 * @brief A task submitted while the queue has work in it joins the back of it.
 *
 * The rule the pool was asked for: finding a free worker does not let a task jump a queue that
 * already has work in it. With every worker held at the gate there is no free worker to find, so
 * what this states is the other half - each submission lands in the queue, in turn.
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
      pool.submit([&busy] { busy(); });
    }

    // Every worker has to be at the gate before the queue means anything: a submission racing a
    // worker that has not started yet would be handed straight to it.
    ASSERT_TRUE(wait_for([&busy] { return busy.arrived() == static_cast<int>(worker_count); }, 5s));
    EXPECT_EQ(pool.pending(), 0u);

    for (std::size_t i = 0; i < queued_count; ++i) {
      pool.submit([&work] { work.run(); });
      EXPECT_EQ(pool.pending(), i + 1);
    }

    busy.open();
  }
}

/**
 * @brief A task handed straight to a free worker, because the queue is empty.
 *
 * The first half of the placement rule: with nothing waiting, a submission does not sit in the
 * queue behind a busy worker when another one is idle.
 */
TEST(executor_tests, a_free_worker_takes_a_task_with_an_empty_queue) {
  gate busy;
  std::atomic_bool ran = {false};

  {
    executor pool(2);

    pool.submit([&busy] { busy(); });
    ASSERT_TRUE(wait_for([&busy] { return busy.arrived() == 1; }, 5s));

    pool.submit([&ran] { ran = true; });

    // The second worker is free and the queue is empty, so this runs while the first worker is
    // still held - no release is needed to get it to run.
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
      pool.submit([&busy] { busy(); });
    }
    ASSERT_TRUE(wait_for([&busy] { return busy.arrived() == static_cast<int>(worker_count); }, 5s));

    for (std::size_t i = 0; i < queued_count; ++i) {
      pool.submit([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
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
 * Four 150ms tasks on four workers. The bound is loose on purpose: what would fail here is a pool
 * that serialised them, which costs 600ms, not a runner that was a little slow.
 */
TEST(executor_tests, tasks_run_concurrently) {
  const auto started = std::chrono::steady_clock::now();

  {
    executor pool(4);
    for (int i = 0; i < 4; ++i) {
      pool.submit([] { std::this_thread::sleep_for(150ms); });
    }
  }

  const auto elapsed = std::chrono::steady_clock::now() - started;
  EXPECT_LT(elapsed, 500ms);
}

/**
 * @brief Nothing is lost when several threads submit at once.
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
          pool.submit([&work] { work.run(); });
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
 * untangle::async::execution catches what escapes an action and reports it on stderr; the worker
 * carries on. Nothing reaches the submitter, which is why the task after it is what this reads.
 */
TEST(executor_tests, a_throwing_task_does_not_stop_the_worker) {
  mock_work work;
  EXPECT_CALL(work, run()).Times(3);

  {
    executor pool(1);  // one worker, so the task after the throw is the same worker's

    pool.submit([&work] { work.run(); });
    pool.submit([] { throw std::runtime_error("a task that throws"); });
    pool.submit([&work] { work.run(); });
    pool.submit([] { throw 42; });  // not a std::exception; the catch-all arm
    pool.submit([&work] { work.run(); });
  }
}

/**
 * @brief A task may submit more work, and it runs.
 *
 * The inner submission has to be waited for rather than assumed: leaving the scope starts the
 * destructor, and a pool that has stopped accepting refuses the task its own worker is about to
 * hand it. That is the pool's behaviour today - work spawned by an in-flight task is only accepted
 * while the pool is still up.
 */
TEST(executor_tests, a_task_can_submit_more_work) {
  mock_work work;
  EXPECT_CALL(work, run()).Times(1);

  {
    executor pool(2);
    std::atomic_bool submitted = {false};

    pool.submit([&work, &pool, &submitted] { submitted = pool.submit([&work] { work.run(); }); });

    ASSERT_TRUE(wait_for([&submitted] { return submitted.load(); }, 5s));
  }
}

/**
 * @brief A pool that is shutting down refuses the task and says so.
 *
 * The refusal is only reachable from inside the pool: submit() answers false once the destructor
 * has stopped accepting, and the destructor is waiting for this very task to return. So the task
 * polls, and the case fails as a timeout rather than as a hang.
 */
TEST(executor_tests, submit_is_refused_once_the_pool_is_shutting_down) {
  auto pool = std::make_unique<executor>(2);

  // The task reads the pool through a raw pointer, not through the unique_ptr: reset() clears the
  // pointer before it runs the destructor, so a task reading the unique_ptr would find it null
  // while the object it is running on is still very much alive.
  executor* const running_pool = pool.get();

  std::atomic_bool refused = {false};
  std::atomic_bool started = {false};

  running_pool->submit([running_pool, &refused, &started] {
    started = true;
    refused = wait_for([running_pool] { return !running_pool->submit([] {}); }, 10s);
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

    pool.submit([&busy] { busy(); });
    ASSERT_TRUE(wait_for([&busy] { return busy.arrived() == 1; }, 5s));

    // The task at the gate is the worker's, not the queue's.
    EXPECT_EQ(pool.pending(), 0u);

    pool.submit([] {});
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
 * A pool with no workers would take work and never run it: submit() queues the task and answers
 * true, and the destructor then waits for a queue only a worker can empty.
 * std::thread::hardware_concurrency() returns 0 when it cannot tell how many cores there are, and a
 * caller passing that straight through is the likely way in.
 */
TEST(executor_tests, a_pool_with_no_workers_is_refused) {
  EXPECT_THROW(executor(0), std::invalid_argument);
}

/**
 * @brief A destructor waiting on a task that has not returned reports why, and keeps waiting.
 *
 * Abandoning a running task would be worse than waiting for it, so the wait stays unbounded; what
 * it stops doing is keeping quiet about itself. The task holds the pool's only worker, so
 * nothing_running() cannot come true and the closing brace is where this case spends its time.
 *
 * @remark The task times its own stall instead of waiting to be released, which is what lets a
 * plain scope destroy the pool: ~executor() has already begun by the time the scope is left, so
 * anything that released it would have to live outside. `started` is declared before the pool for
 * the same reason - reversed, it would be destroyed while the task was still reading it.
 */
TEST(executor_tests, a_destructor_stuck_on_a_task_reports_why) {
  //! How long the task holds its worker - longer than a first report is due, so the destructor is
  //! stuck across at least one of them.
  constexpr auto stall = 2s;

  testing::internal::CaptureStderr();

  {
    std::atomic_bool started = {false};
    executor pool(1);

    pool.submit([&started, stall] {
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
 * The pool is a template on its task type, so the signature is the caller's to name. A continuous
 * worker collects nothing, so the return is run and discarded - which is why the count rather than
 * the results is what this reads.
 */
TEST(executor_tests, a_pool_runs_tasks_that_return_a_value) {
  using int_executor = untangle::executor<std::function<int(void)>>;

  constexpr int tasks = 16;
  std::atomic_int ran = {0};

  {
    int_executor pool(2);

    for (int i = 0; i < tasks; ++i) {
      EXPECT_TRUE(pool.submit([&ran] { return ran.fetch_add(1, std::memory_order_relaxed); }));
    }
  }

  EXPECT_EQ(ran.load(), tasks);
}

/**
 * @brief A pool runs a task type that is not a std::function.
 *
 * Any callable naming its own result_type will do, because that typedef is all an execution reads
 * from the type. A pool built this way never touches std::function, which is what keeps it clear of
 * a result_type the standard removed in C++20.
 */
TEST(executor_tests, a_pool_runs_a_task_type_that_is_not_a_std_function) {
  using counting_executor = untangle::executor<counting_task>;

  constexpr int tasks = 16;
  std::atomic_int ran = {0};

  {
    counting_executor pool(2);

    for (int i = 0; i < tasks; ++i) {
      EXPECT_TRUE(pool.submit(counting_task{&ran}));
    }
  }

  EXPECT_EQ(ran.load(), tasks);
}

/**
 * @brief Destroying a pool while its workers are still turning over does not race them.
 *
 * ~executor() waits on the poll until no worker is left in its thread, and destroys them only then.
 * Without that wait a worker could still be inside take_next_task(), reading every execution
 * through nothing_running() and is_busy() while the vector holding them was being cleared. Not
 * waiting here is what opens the window: the queue is still backed up when the destructor starts.
 *
 * @attention Sanitizer-sensitive. On an ordinary build the accesses are unsynchronised rather than
 * wrong in any order it can observe, so passing there proves nothing; configure with
 * -DEXECUTOR_SANITIZE=thread, where this aborted before the wait existed. ASan stays silent even
 * so - the race is a free against a read with nothing ordering them, so it faults only when the
 * read lands after the free.
 *
 * @remark What it states on any build is the finish: every task submitted before the scope closed
 * has run by the time the destructor returns.
 */
TEST(executor_tests, destroying_a_pool_under_load_does_not_race_its_workers) {
  constexpr int rounds = 50;
  constexpr int tasks_per_round = 64;
  std::atomic_int ran = {0};

  for (int round = 0; round < rounds; ++round) {
    executor pool(4);

    for (int i = 0; i < tasks_per_round; ++i) {
      pool.submit([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
    }

    // Deliberately no wait: leaving the scope with the queue still backed up is what puts workers
    // inside take_next_task() at the moment the destructor reaches clear().
  }

  EXPECT_EQ(ran.load(), rounds * tasks_per_round);
}
