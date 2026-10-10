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

#include <algorithm>
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
#include <type_traits>
#include <vector>

namespace {

// The pool under test, named once: executor is a template on its task type.
using executor = untangle::executor<std::function<void(void)>>;
using namespace std::chrono_literals;
using ::testing::InSequence;

//! The callback for work whose result nobody reads: add_task() requires one, and this takes
//! whatever the task returns, or nothing for a void task.
constexpr auto ignore_result = [](auto&&...) {};

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
    pool.start();
    for (int i = 0; i < task_count; ++i) {
      EXPECT_TRUE(pool.add_task([&work] { work.run(); }, ignore_result));
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
    pool.start();
    pool.add_task(
        [&finished] {
          std::this_thread::sleep_for(200ms);
          finished = true;
        },
        ignore_result);
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
    pool.start();
    for (int i = 0; i < task_count; ++i) {
      pool.add_task([&work, i] { work.run_numbered(i); }, ignore_result);
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
    pool.start();

    for (std::size_t i = 0; i < worker_count; ++i) {
      pool.add_task([&busy] { busy(); }, ignore_result);
    }

    // Every worker must be at the gate first: one not yet started would be handed the task.
    ASSERT_TRUE(wait_for([&busy] { return busy.arrived() == static_cast<int>(worker_count); }, 5s));
    EXPECT_EQ(pool.pending(), 0u);

    for (std::size_t i = 0; i < queued_count; ++i) {
      pool.add_task([&work] { work.run(); }, ignore_result);
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
    pool.start();

    pool.add_task([&busy] { busy(); }, ignore_result);
    ASSERT_TRUE(wait_for([&busy] { return busy.arrived() == 1; }, 5s));

    pool.add_task([&ran] { ran = true; }, ignore_result);

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
    pool.start();

    for (std::size_t i = 0; i < worker_count; ++i) {
      pool.add_task([&busy] { busy(); }, ignore_result);
    }
    ASSERT_TRUE(wait_for([&busy] { return busy.arrived() == static_cast<int>(worker_count); }, 5s));

    for (std::size_t i = 0; i < queued_count; ++i) {
      pool.add_task([&ran] { ran.fetch_add(1, std::memory_order_relaxed); }, ignore_result);
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
    pool.start();
    for (int i = 0; i < 4; ++i) {
      pool.add_task([] { std::this_thread::sleep_for(150ms); }, ignore_result);
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
    pool.start();

    std::vector<std::thread> threads;
    for (int t = 0; t < submitters; ++t) {
      threads.emplace_back([&pool, &work] {
        for (int i = 0; i < per_thread; ++i) {
          pool.add_task([&work] { work.run(); }, ignore_result);
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
    pool.start();

    pool.add_task([&work] { work.run(); }, ignore_result);
    pool.add_task([] { throw std::runtime_error("a task that throws"); }, ignore_result);
    pool.add_task([&work] { work.run(); }, ignore_result);
    pool.add_task([] { throw 42; }, ignore_result);  // not a std::exception; the catch-all arm
    pool.add_task([&work] { work.run(); }, ignore_result);
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
    executor.start();

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

    executor.add_task([] { throw std::runtime_error("a task that throws"); }, ignore_result);
    executor.add_task([] { throw 42; }, ignore_result);  // not a std::exception; the catch-all arm
  }

  EXPECT_EQ(reported.load(), 2) << "the executor swallowed what the tasks threw";
  EXPECT_EQ(reported_what, "a task that throws");
  EXPECT_EQ(reported_unknown.load(), 1);
}

/**
 * @brief stop() runs what was already submitted: a stopped worker drains its own queue.
 *
 * Each worker keeps its own queue, so work a worker was given before stop() is its to run. Nothing
 * is refused or dropped, so stderr says nothing about it.
 */
TEST(executor_tests, stop_runs_what_was_already_submitted) {
  constexpr int queued_count = 4;

  gate busy;
  std::atomic_int ran = {0};

  testing::internal::CaptureStderr();

  {
    executor executor(1);
    executor.start();

    // Occupies the only worker, so everything after it waits in its queue.
    EXPECT_TRUE(executor.add_task([&busy] { busy(); }, ignore_result));
    ASSERT_TRUE(wait_for([&busy] { return busy.arrived() == 1; }, 5s));

    for (int i = 0; i < queued_count; ++i) {
      EXPECT_TRUE(executor.add_task([&ran] { ran.fetch_add(1, std::memory_order_relaxed); },
                                    ignore_result));
    }
    EXPECT_EQ(executor.pending(), static_cast<std::size_t>(queued_count));

    executor.stop();
    busy.open();
  }

  const std::string reported = testing::internal::GetCapturedStderr();

  EXPECT_EQ(ran.load(), queued_count) << "work submitted before stop() did not run";
  EXPECT_THAT(reported, ::testing::Not(::testing::HasSubstr("refused")))
      << "stderr held: " << reported;
  EXPECT_THAT(reported, ::testing::Not(::testing::HasSubstr("dropped")))
      << "stderr held: " << reported;
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
    pool.start();
    std::atomic_bool submitted = {false};

    pool.add_task(
        [&work, &pool, &submitted] {
          submitted = pool.add_task([&work] { work.run(); }, ignore_result);
        },
        ignore_result);

    ASSERT_TRUE(wait_for([&submitted] { return submitted.load(); }, 5s));
  }
}

/**
 * @brief A pool that is shutting down refuses the task and says so.
 *
 * The refusal is only reachable from inside the pool: add_task() answers false once the
 * destructor has stopped accepting, and the destructor is waiting for this very task to return. So
 * the task polls, and the case fails as a timeout rather than as a hang.
 */
TEST(executor_tests, add_task_is_refused_once_the_pool_is_shutting_down) {
  auto pool = std::make_unique<executor>(2);
  pool->start();

  // The task reads the pool through a raw pointer, not through the unique_ptr: reset() clears the
  // pointer before it runs the destructor, so a task reading the unique_ptr would find it null
  // while the object it is running on is still very much alive.
  executor* const running_pool = pool.get();

  std::atomic_bool refused = {false};
  std::atomic_bool started = {false};

  running_pool->add_task(
      [running_pool, &refused, &started] {
        started = true;
        refused =
            wait_for([running_pool] { return !running_pool->add_task([] {}, ignore_result); }, 10s);
      },
      ignore_result);

  ASSERT_TRUE(wait_for([&started] { return started.load(); }, 5s));

  pool.reset();  // blocks in ~executor until the task above returns

  EXPECT_TRUE(refused.load());
}

/**
 * @brief A task running when the pool stops cannot add more work either.
 *
 * One rule for both ways a pool stops taking work, and it does not care who is asking: a task on a
 * worker thread is refused like any other caller. Letting in-flight work spawn more is what could
 * keep a shutdown from ever ending.
 */
TEST(executor_tests, a_running_task_cannot_add_work_once_the_pool_stops) {
  gate hold;
  std::atomic_bool refused = {false};
  std::atomic_bool spawned = {false};

  {
    executor pool(1);
    pool.start();

    pool.add_task(
        [&pool, &hold, &refused, &spawned] {
          hold();  // released once the pool has been stopped
          refused = !pool.add_task([&spawned] { spawned = true; }, ignore_result);
        },
        ignore_result);

    ASSERT_TRUE(wait_for([&hold] { return hold.arrived() == 1; }, 5s));

    pool.stop();
    hold.open();
  }  // ~executor() waits for the task above, so refused is set by the time it returns

  EXPECT_TRUE(refused.load()) << "a stopped pool took work from a task it was still running";
  EXPECT_FALSE(spawned.load()) << "a refused task cannot have run";
}

/**
 * @brief pending() is the queue's depth, not the pool's occupancy.
 */
TEST(executor_tests, pending_counts_only_what_is_waiting) {
  gate busy;

  {
    executor pool(1);
    pool.start();

    pool.add_task([&busy] { busy(); }, ignore_result);
    ASSERT_TRUE(wait_for([&busy] { return busy.arrived() == 1; }, 5s));

    // The task at the gate is the worker's, not the queue's.
    EXPECT_EQ(pool.pending(), 0u);

    pool.add_task([] {}, ignore_result);
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
  pool.start();
  EXPECT_EQ(pool.pending(), 0u);
}

/**
 * @brief A pool that has not been started refuses work.
 *
 * Its workers are built but not running, so a task taken now would sit unrun and the destructor
 * would wait for it.
 */
TEST(executor_tests, an_unstarted_pool_refuses_work) {
  std::atomic_int ran = {0};

  {
    executor pool(2);

    EXPECT_FALSE(
        pool.add_task([&ran] { ran.fetch_add(1, std::memory_order_relaxed); }, ignore_result));
    EXPECT_EQ(pool.pending(), 0u);
  }

  EXPECT_EQ(ran.load(), 0);
}

/**
 * @brief A pool that was never started destructs without waiting for anything.
 */
TEST(executor_tests, an_unstarted_pool_shuts_down) {
  const auto began = std::chrono::steady_clock::now();

  {
    executor pool(4);
  }

  EXPECT_LT(std::chrono::steady_clock::now() - began, 2s);
}

/**
 * @brief start() runs the workers, and the pool takes work from then on.
 */
TEST(executor_tests, a_started_pool_runs_what_it_is_given) {
  mock_work work;
  EXPECT_CALL(work, run()).Times(8);

  {
    executor pool(2);
    pool.start();

    for (int i = 0; i < 8; ++i) {
      EXPECT_TRUE(pool.add_task([&work] { work.run(); }, ignore_result));
    }
  }
}

/**
 * @brief A stopped pool can be started again.
 *
 * stop() ends the workers' working life and start() gives them a new one, so the tasks refused in
 * between are the only ones lost.
 */
TEST(executor_tests, a_stopped_pool_can_be_started_again) {
  std::atomic_int ran = {0};
  const auto task = [&ran] { ran.fetch_add(1, std::memory_order_relaxed); };

  {
    executor pool(1);

    pool.start();
    EXPECT_TRUE(pool.add_task(task, ignore_result));

    pool.stop();
    EXPECT_FALSE(pool.add_task(task, ignore_result)) << "a stopped pool took work";

    pool.start();
    EXPECT_TRUE(pool.add_task(task, ignore_result)) << "a restarted pool refused work";
  }

  EXPECT_EQ(ran.load(), 2);
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
    pool.start();

    pool.add_task(
        [&started, stall] {
          started.store(true);
          std::this_thread::sleep_for(stall);
        },
        ignore_result);

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
 * @brief A pool runs tasks that return a value.
 *
 * The callback throws each result away, so the count rather than the results is what this reads.
 */
TEST(executor_tests, a_pool_runs_tasks_that_return_a_value) {
  using int_executor = untangle::executor<std::function<int(void)>>;

  constexpr int tasks = 16;
  std::atomic_int ran = {0};

  {
    int_executor pool(2);
    pool.start();

    for (int i = 0; i < tasks; ++i) {
      EXPECT_TRUE(pool.add_task([&ran] { return ran.fetch_add(1, std::memory_order_relaxed); },
                                ignore_result));
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
    pool.start();

    for (int i = 0; i < tasks; ++i) {
      EXPECT_TRUE(pool.add_task(counting_task{&ran}, ignore_result));
    }
  }

  EXPECT_EQ(ran.load(), tasks);
}

namespace {

/**
 * @brief A task type that is not a std::function, and counts its own copies and moves.
 *
 * A std::function relocates what it holds differently on each standard library, so counting
 * through one would measure the library.
 */
struct move_counting_task {
  using result_type = int;

  move_counting_task() = default;
  move_counting_task(const move_counting_task&) { copies.fetch_add(1, std::memory_order_relaxed); }
  move_counting_task(move_counting_task&&) noexcept {
    moves.fetch_add(1, std::memory_order_relaxed);
  }

  int operator()(int n) const { return n; }

  static inline std::atomic_int copies = {0};
  static inline std::atomic_int moves = {0};
};

//! How many times a sealed task moves a callable it is built from: the standard library's share,
//! which the cases below allow for so that they count only the pool's own moves.
int moves_to_seal_a_task() {
  const auto before = move_counting_task::moves.load();
  untangle::task_t sealed = [task = move_counting_task{}] { (void)task(0); };
  (void)sealed;
  return move_counting_task::moves.load() - before;
}

}  // namespace

/**
 * @brief A task the caller gives up is moved into its worker's queue once.
 *
 * What every submission through a binding or a temporary costs, so a move more is paid per task.
 */
TEST(executor_tests, submitting_moves_a_task_given_up_once) {
  using counting_executor = untangle::executor<move_counting_task>;

  const int sealing = moves_to_seal_a_task();
  move_counting_task task;
  counting_executor pool(1);
  pool.start();

  const auto copies_before = move_counting_task::copies.load();
  const auto moves_before = move_counting_task::moves.load();

  EXPECT_TRUE(pool.add_task(std::move(task), 7, ignore_result));

  EXPECT_EQ(move_counting_task::copies.load() - copies_before, 0)
      << "a task moved into add_task() was copied";
  EXPECT_EQ(move_counting_task::moves.load() - moves_before, sealing + 1)
      << "the task was moved " << move_counting_task::moves.load() - moves_before - sealing
      << " times on its way into the queue";
}

/**
 * @brief A task the caller keeps is copied into its worker's queue once, and not moved after.
 */
TEST(executor_tests, submitting_copies_a_task_the_caller_keeps_once) {
  using counting_executor = untangle::executor<move_counting_task>;

  const int sealing = moves_to_seal_a_task();
  const move_counting_task task;
  counting_executor pool(1);
  pool.start();

  const auto copies_before = move_counting_task::copies.load();
  const auto moves_before = move_counting_task::moves.load();

  EXPECT_TRUE(pool.add_task(task, 7, ignore_result));

  EXPECT_EQ(move_counting_task::copies.load() - copies_before, 1)
      << "the queue does not hold one copy of the caller's task";
  EXPECT_EQ(move_counting_task::moves.load() - moves_before, sealing)
      << "the copy was moved " << move_counting_task::moves.load() - moves_before - sealing
      << " times on its way into the queue";
}

// --- step 27, item 24 ----------------------------------------------------------------------------
// The two cases below do not compile: executor.hpp:42 refuses a task type that takes arguments, so
// the whole binary fails to build and the 25 cases above it cannot be run until the fix lands. They
// are left live deliberately - the refusal is the finding, and a guarded case would hide it.

/**
 * @brief A pool runs a task type that takes arguments and returns a value.
 *
 * The signature async::execution already accepts at its own door - an action and the arguments to
 * bind to it - asked of the pool. Nothing about the return is new: the callback throws it away,
 * exactly as in a_pool_runs_tasks_that_return_a_value. It is here because a task type is
 * one signature, and the pool has to carry both halves of it or neither.
 *
 * @remark Every worker is free as this adds, so each task goes straight to one rather than through
 * the queue. Which argument reaches which task is the claim; the order they arrive in is not, so
 * the expectations are set per value rather than in sequence.
 */
TEST(executor_tests, a_pool_runs_a_task_type_that_takes_arguments_and_returns_a_value) {
  using numbered_executor = untangle::executor<std::function<int(int)>>;

  constexpr int task_count = 50;
  mock_work work;

  for (int i = 0; i < task_count; ++i) {
    EXPECT_CALL(work, run_numbered(i));
  }

  {
    numbered_executor pool(4);
    pool.start();

    for (int i = 0; i < task_count; ++i) {
      // The return is dropped by the pool, so the mock call is what says the task body ran through.
      EXPECT_TRUE(pool.add_task(
          [&work](int n) {
            work.run_numbered(n);
            return n * 2;
          },
          i, ignore_result));
    }
  }
}

/**
 * @brief An argument survives the wait for a worker, and arrives with the task it was given to.
 *
 * This is the half the group 7 ruling turned on - that a pool binding arguments would have "nowhere
 * to keep them until a worker frees up". The single worker is held at the gate, so every task below
 * it waits in pending_ with its argument and is run from there.
 */
TEST(executor_tests, an_argument_survives_the_queue) {
  using numbered_executor = untangle::executor<std::function<int(int)>>;

  constexpr int task_count = 50;
  mock_work work;

  {
    // One worker runs the queue in order, so the arguments must arrive in the order they were
    // added; a pair the other way round is an argument that went to the wrong task.
    InSequence ordered;
    for (int i = 0; i < task_count; ++i) {
      EXPECT_CALL(work, run_numbered(i));
    }
  }

  gate blocker;

  {
    numbered_executor pool(1);
    pool.start();

    // The only worker stops here, so nothing added after this can be handed to one.
    EXPECT_TRUE(pool.add_task(
        [&blocker](int n) {
          blocker();
          return n;
        },
        0, ignore_result));
    ASSERT_TRUE(wait_for([&blocker] { return blocker.arrived() == 1; }, 2s))
        << "the worker never reached the gate, so the tasks below were not queued";

    for (int i = 0; i < task_count; ++i) {
      EXPECT_TRUE(pool.add_task(
          [&work](int n) {
            work.run_numbered(n);
            return n * 2;
          },
          i, ignore_result));
    }

    EXPECT_EQ(pool.pending(), task_count) << "the tasks did not wait in the queue";

    blocker.open();
  }
}

// --- tasks on the pool ---------------------------------------------------------------------------
// The pool has one door. add_task() carries the callback it must notify, built by
// untangle::bind_task() with the callback as its last argument; work nobody wants told about passes
// a callback that ignores the result.

TEST(executor_tests, a_task_notifies_its_callback_with_the_result) {
  // Every worker is free as this adds, so the task goes straight to one rather than through the
  // queue - the shortest path the notification has to survive.
  using int_executor = untangle::executor<std::function<int(int)>>;

  std::atomic_int reported = {-1};
  std::atomic_bool ran = {false};

  {
    int_executor pool(2);
    pool.start();

    EXPECT_TRUE(pool.add_task(
        [&ran](int n) {
          ran = true;
          return n * 2;
        },
        21, [&reported](int result) { reported.store(result); }));
  }

  ASSERT_TRUE(ran.load()) << "the task itself never ran";
  EXPECT_EQ(reported.load(), 42) << "the pool ran the task but never notified its callback";
}

TEST(executor_tests, a_callback_survives_the_queue) {
  // The other half of an_argument_survives_the_queue: the single worker is held at the gate, so the
  // task below it waits in pending_ with its callback and is run from there.
  using int_executor = untangle::executor<std::function<int(int)>>;

  std::atomic_int reported = {-1};
  gate blocker;

  {
    int_executor pool(1);
    pool.start();

    // The only worker stops here, so what follows cannot be handed to one.
    EXPECT_TRUE(pool.add_task(
        [&blocker](int n) {
          blocker();
          return n;
        },
        0, [](int) {}));
    ASSERT_TRUE(wait_for([&blocker] { return blocker.arrived() == 1; }, 2s))
        << "the worker never reached the gate, so the task below was not queued";

    EXPECT_TRUE(pool.add_task([](int n) { return n * 2; }, 21,
                              [&reported](int result) { reported.store(result); }));

    EXPECT_EQ(pool.pending(), 1u) << "the task did not wait in the queue";

    blocker.open();
  }

  EXPECT_EQ(reported.load(), 42) << "a task run from the queue never notified its callback";
}

TEST(executor_tests, a_void_task_notifies_that_it_finished) {
  // The half no earlier case covered, and the reason a task's callback is required rather than
  // inferred: a task with no result still has a completion to report.
  constexpr int task_count = 8;
  std::atomic_int notified = {0};

  {
    executor pool(2);
    pool.start();

    for (int i = 0; i < task_count; ++i) {
      EXPECT_TRUE(pool.add_task([] {}, [&notified] { notified.fetch_add(1); }));
    }
  }

  EXPECT_EQ(notified.load(), task_count) << "a void task finished without saying so";
}

TEST(executor_tests, a_task_that_cannot_notify_is_refused) {
  // The callback's type is checked at compile time, its emptiness cannot be: an empty std::function
  // is a callback of the right type that can never be called. untangle::bind_task() builds an empty
  // task from one, and a task that can neither run nor notify is refused rather than queued - the
  // answer is the whole report, as it is for a task added to a stopped pool.
  using int_executor = untangle::executor<std::function<int(int)>>;

  std::atomic_bool ran = {false};
  std::function<void(int)> no_callback;

  {
    int_executor pool(1);
    pool.start();

    EXPECT_FALSE(pool.add_task(
        [&ran](int n) {
          ran = true;
          return n * 2;
        },
        21, no_callback))
        << "the pool accepted a task whose callback can never be called";
  }

  EXPECT_FALSE(ran.load()) << "a refused task ran anyway";
}

TEST(executor_tests, a_task_that_throws_does_not_notify_and_reaches_the_caller) {
  // Finished does not mean failed, through the pool's own reporting seam: no result to hand over,
  // so no notification, and what it threw goes to on_task_error as a failing action's does.
  using int_executor = untangle::executor<std::function<int(int)>>;

  std::atomic_bool notified = {false};
  std::atomic_int reported_errors = {0};

  {
    int_executor pool(1);
    pool.on_task_error = [&reported_errors](std::exception_ptr) {
      reported_errors.fetch_add(1, std::memory_order_relaxed);
    };
    pool.start();

    EXPECT_TRUE(pool.add_task([](int) -> int { throw std::runtime_error("task failed"); }, 1,
                              [&notified](int) { notified = true; }));
  }

  EXPECT_EQ(reported_errors.load(), 1) << "what the task threw never reached on_task_error";
  EXPECT_FALSE(notified.load()) << "a task that threw reported as though it had finished";
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
    pool.start();

    for (int i = 0; i < tasks_per_round; ++i) {
      pool.add_task([&ran] { ran.fetch_add(1, std::memory_order_relaxed); }, ignore_result);
    }

    // Deliberately no wait: a backed-up queue is what puts workers inside take_next_task().
  }

  EXPECT_EQ(ran.load(), rounds * tasks_per_round);
}

/**
 * @brief Destroying a pool with tasks still queued does not race the callbacks they carry.
 *
 * The same shape as destroying_a_pool_under_load_does_not_race_its_workers, with the other door:
 * the queue is backed up when the destructor starts, so workers are inside take_next_task() as it
 * reaches the executions - and now each one is also notifying a callback on its own thread while
 * that happens. Several callbacks run at once, on different workers, touching the same counter.
 *
 * @attention Sanitizer-sensitive, and only under -DEXECUTOR_SANITIZE=thread. A plain build cannot
 * observe the race and ASan does not report it, so passing on either proves nothing.
 *
 * @remark What it states on any build is the accounting: every task added before the scope closed
 * has run **and** notified by the time the destructor returns, and the two counts agree. A callback
 * that was skipped shows up as the second count falling short of the first, which no count of runs
 * alone would catch.
 */
TEST(executor_tests, destroying_a_pool_with_tasks_queued_does_not_race_their_callbacks) {
  using int_executor = untangle::executor<std::function<int(int)>>;

  constexpr int rounds = 50;
  constexpr int tasks_per_round = 64;

  std::atomic_int ran = {0};
  std::atomic_int notified = {0};
  std::atomic_int mismatched = {0};

  for (int round = 0; round < rounds; ++round) {
    int_executor pool(4);
    pool.start();

    for (int i = 0; i < tasks_per_round; ++i) {
      pool.add_task(
          [&ran](int n) {
            ran.fetch_add(1, std::memory_order_relaxed);
            return n * 2;
          },
          i,
          [&notified, &mismatched, i](int result) {
            // The callback is handed its own task's result, not another's - which a shared queue
            // running several tasks at once is exactly where it could go wrong.
            if (result != i * 2) {
              mismatched.fetch_add(1, std::memory_order_relaxed);
            }
            notified.fetch_add(1, std::memory_order_relaxed);
          });
    }

    // Deliberately no wait: a backed-up queue is what puts workers inside take_next_task().
  }

  EXPECT_EQ(ran.load(), rounds * tasks_per_round);
  EXPECT_EQ(notified.load(), ran.load()) << "a task ran without notifying its callback";
  EXPECT_EQ(mismatched.load(), 0) << "a callback was handed a result from a different task";
}

/**
 * @brief wait() is a barrier, so a pool goes on running and takes more work after one.
 *
 * The case the old wait() failed: it destroyed the workers, so a later task was accepted and then
 * never ran. The pool is destroyed already waited on, which drains it a second time.
 */
TEST(executor_tests, wait_is_reusable_and_the_pool_runs_what_comes_after) {
  constexpr int rounds = 3;
  std::atomic_int ran = {0};

  executor pool(2);
  pool.start();

  for (int round = 1; round <= rounds; ++round) {
    EXPECT_TRUE(pool.add_task([&ran] { ran.fetch_add(1); }, ignore_result))
        << "round " << round << " was refused, so wait() did not leave the pool accepting";

    pool.wait();  // deliberately no start() in between

    EXPECT_EQ(ran.load(), round) << "round " << round << " had not run when wait() returned";
  }
}

/**
 * @brief wait() restores whatever it found, so it never leaves a pool that was not running.
 *
 * The only case that fails if the restore is written as an unconditional resumption.
 */
TEST(executor_tests, wait_leaves_a_pool_that_is_not_running_alone) {
  executor pool(1);

  pool.wait();  // never started, so there is nothing to drain and nothing to resume
  EXPECT_FALSE(pool.add_task([] {}, ignore_result)) << "wait() left an unstarted pool accepting";

  pool.start();
  pool.stop();

  pool.wait();
  EXPECT_FALSE(pool.add_task([] {}, ignore_result)) << "wait() left a stopped pool accepting";
}

/**
 * @brief wait() returns once the queue is empty and every callback has run.
 *
 * More tasks than workers, so most of this waits in pending_ rather than in flight. A callback runs
 * inside the worker's drain, which is what makes it part of the drain rather than after it.
 */
TEST(executor_tests, wait_drains_the_queue_and_every_callback_with_it) {
  constexpr int task_count = 32;
  std::atomic_int notified = {0};

  executor pool(2);
  pool.start();

  for (int i = 0; i < task_count; ++i) {
    ASSERT_TRUE(pool.add_task([] {}, [&notified] { notified.fetch_add(1); }));
  }

  pool.wait();

  EXPECT_EQ(notified.load(), task_count) << "wait() returned before every callback had run";
  EXPECT_EQ(pool.pending(), 0u) << "wait() returned with work still queued";
}

/**
 * @brief A pool takes no work while a wait() drains it, and takes work again once that returns.
 *
 * Refusing is what lets the drain end at all - work arriving during it, from a task re-adding
 * itself as much as from another thread, would otherwise keep the queue from ever emptying.
 */
TEST(executor_tests, wait_refuses_new_work_until_it_returns) {
  gate busy;
  std::atomic_bool returned = {false};

  executor pool(1);
  pool.start();

  pool.add_task([&busy] { busy(); }, ignore_result);
  ASSERT_TRUE(wait_for([&busy] { return busy.arrived() == 1; }, 5s));

  // On its own thread, because it blocks until the gate opens below.
  std::thread waiter([&pool, &returned] {
    pool.wait();
    returned = true;
  });

  EXPECT_TRUE(wait_for([&pool] { return !pool.add_task([] {}, ignore_result); }, 5s))
      << "the pool kept accepting work while a wait() was draining it";

  busy.open();

  EXPECT_TRUE(wait_for([&returned] { return returned.load(); }, 10s)) << "wait() never returned";
  waiter.join();

  EXPECT_TRUE(pool.add_task([] {}, ignore_result)) << "wait() did not put accepting back";
}

/**
 * @brief Two threads draining one pool leave it accepting, and neither returns early.
 *
 * Overlapping waits used to read each other's cleared flag: the last one out restored the value it
 * had found already false, and the pool refused work from then on with nothing to explain it.
 */
TEST(executor_tests, concurrent_waits_leave_the_pool_accepting) {
  // The interleaving is a race, so one round is not a reliable probe: unserialised, this reproduced
  // in roughly one run in three.
  constexpr int rounds = 20;
  constexpr int queued_count = 8;

  for (int round = 1; round <= rounds; ++round) {
    gate busy;
    std::atomic_int ran = {0};

    executor pool(1);
    pool.start();

    pool.add_task([&busy] { busy(); }, ignore_result);
    ASSERT_TRUE(wait_for([&busy] { return busy.arrived() == 1; }, 5s)) << "round " << round;

    // Behind the held worker, so both waits have a queue to drain rather than nothing to do.
    for (int i = 0; i < queued_count; ++i) {
      ASSERT_TRUE(pool.add_task([&ran] { ran.fetch_add(1); }, ignore_result)) << "round " << round;
    }

    std::thread first([&pool] { pool.wait(); });
    std::thread second([&pool] { pool.wait(); });

    // A refusal is the only observable sign that a wait is under way, and it queues nothing.
    ASSERT_TRUE(wait_for([&pool] { return !pool.add_task([] {}, ignore_result); }, 5s))
        << "round " << round;

    busy.open();
    first.join();
    second.join();

    // Before adding anything more, or the new task could have run by the time this reads it.
    ASSERT_EQ(ran.load(), queued_count)
        << "round " << round << ": a wait returned with work queued";

    ASSERT_TRUE(pool.add_task([&ran] { ran.fetch_add(1); }, ignore_result))
        << "round " << round << ": two overlapping waits left the pool refusing work";

    pool.wait();
    ASSERT_EQ(ran.load(), queued_count + 1)
        << "round " << round << ": the pool did not run what it accepted afterwards";
  }
}

/**
 * @brief Opens two gates when it goes out of scope, so a failed check cannot leave a worker held.
 *
 * Declared after the pool, it is destroyed first: the pool's destructor waits for its workers.
 */
struct open_on_exit {
  gate& first;
  gate& second;
  ~open_on_exit() {
    first.open();
    second.open();
  }
};

/**
 * @brief A task given to a busy worker waits for that worker, even when another goes idle.
 *
 * Each worker keeps its own queue, filled at submission: the pool balances by count, not by how
 * long the work takes.
 */
TEST(executor_tests, a_task_waits_for_the_worker_it_was_given) {
  gate busy_a;
  gate busy_b;
  std::atomic_int ran = {0};
  executor pool(2);
  open_on_exit opener{busy_a, busy_b};
  pool.start();

  // Worker 0 is tried first, so each gate lands on its own worker.
  ASSERT_TRUE(pool.add_task([&busy_a] { busy_a(); }, ignore_result));
  ASSERT_TRUE(wait_for([&busy_a] { return busy_a.arrived() == 1; }, 2s));
  ASSERT_TRUE(pool.add_task([&busy_b] { busy_b(); }, ignore_result));
  ASSERT_TRUE(wait_for([&busy_b] { return busy_b.arrived() == 1; }, 2s));

  // Both workers hold one each, so these alternate: two per worker.
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(pool.add_task([&ran] { ran.fetch_add(1); }, ignore_result));
  }

  busy_a.open();
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(ran.load(), 2) << "the freed worker ran tasks given to the other";

  busy_b.open();
  EXPECT_TRUE(wait_for([&ran] { return ran.load() == 4; }, 2s));
}

/**
 * @brief Tasks submitted back to back to an idle pool go to different workers.
 */
TEST(executor_tests, a_burst_is_spread_over_idle_workers) {
  gate busy_a;
  gate busy_b;
  executor pool(2);
  open_on_exit opener{busy_a, busy_b};
  pool.start();

  // Without waiting in between: the second must not queue behind the first.
  ASSERT_TRUE(pool.add_task([&busy_a] { busy_a(); }, ignore_result));
  ASSERT_TRUE(pool.add_task([&busy_b] { busy_b(); }, ignore_result));

  EXPECT_TRUE(wait_for([&] { return busy_a.arrived() == 1 && busy_b.arrived() == 1; }, 2s))
      << "both tasks went to one worker";
}

/**
 * @brief An empty action or callback is refused by the pool itself, without a word on stderr.
 *
 * The answer is the whole report, as before the workers kept their own queues: no worker is asked,
 * so none warns.
 */
TEST(executor_tests, an_empty_task_is_refused_without_a_word) {
  testing::internal::CaptureStderr();

  {
    executor pool(1);
    pool.start();

    EXPECT_FALSE(pool.add_task(std::function<void()>{}, ignore_result)) << "an empty action ran";
    EXPECT_FALSE(pool.add_task([] {}, std::function<void()>{})) << "an empty callback was taken";
  }

  const std::string reported = testing::internal::GetCapturedStderr();
  EXPECT_EQ(reported, "") << "a refusal the caller is told about was also printed";
}

/**
 * @brief A refused task leaves its worker free: the next task still goes to the warmest worker.
 *
 * A worker counts the tasks it was given until it drains; one it never took must not count, or an
 * idle worker looks busy until it is given real work.
 */
TEST(executor_tests, a_refused_task_leaves_its_worker_free) {
  executor pool(2);
  pool.start();

  std::thread::id first;
  ASSERT_TRUE(pool.add_task([&first] { first = std::this_thread::get_id(); }, ignore_result));
  pool.wait();

  EXPECT_FALSE(pool.add_task([] {}, std::function<void()>{}));

  std::thread::id second;
  ASSERT_TRUE(pool.add_task([&second] { second = std::this_thread::get_id(); }, ignore_result));
  pool.wait();

  EXPECT_EQ(second, first) << "the refused task left the warmest worker looking busy";
}

/**
 * @brief Destroying a pool does not sleep a fixed tick: its workers leave within microseconds.
 *
 * The fastest of five destructions, so a slow machine or a sanitizer build does not trip it.
 */
TEST(executor_tests, destroying_a_pool_does_not_wait_a_whole_tick) {
  auto fastest = std::chrono::steady_clock::duration::max();

  for (int round = 0; round < 5; ++round) {
    auto pool = std::make_unique<executor>(1);
    pool->start();

    const auto start = std::chrono::steady_clock::now();
    pool.reset();
    fastest = std::min(fastest, std::chrono::steady_clock::now() - start);
  }

  EXPECT_LT(fastest, 20ms) << "destroying an idle pool took "
                           << std::chrono::duration_cast<std::chrono::microseconds>(fastest).count()
                           << " us";
}

/**
 * @brief The worker a pool's warning names, read from what it printed for a task that threw.
 */
std::string worker_named_in(const std::string& reported) {
  const std::string marker = "executor task on '";
  const auto start = reported.find(marker);
  if (start == std::string::npos) {
    return {};
  }
  const auto first = start + marker.size();
  return reported.substr(first, reported.find('\'', first) - first);
}

/**
 * @brief Two pools name their workers apart, so a warning says which pool it came from.
 */
TEST(executor_tests, two_pools_name_their_workers_apart) {
  std::string named[2];

  for (auto& name : named) {
    testing::internal::CaptureStderr();
    {
      executor pool(1);
      pool.start();
      ASSERT_TRUE(pool.add_task([] { throw std::runtime_error("expected"); }, ignore_result));
      pool.wait();
    }
    name = worker_named_in(testing::internal::GetCapturedStderr());
  }

  ASSERT_FALSE(named[0].empty()) << "no warning named a worker";
  EXPECT_NE(named[0], named[1]) << "both pools named their worker " << named[0];
}

/**
 * @brief A pool given a name prefixes its workers with it.
 */
TEST(executor_tests, a_named_pool_prefixes_its_workers) {
  testing::internal::CaptureStderr();
  {
    executor pool(1, "store_a");
    pool.start();
    ASSERT_TRUE(pool.add_task([] { throw std::runtime_error("expected"); }, ignore_result));
    pool.wait();
  }

  const std::string worker = worker_named_in(testing::internal::GetCapturedStderr());
  EXPECT_TRUE(worker.starts_with("store_a")) << "the worker was named " << worker;
}

//! The workers hold the pool's this, so a pool is neither copied nor moved.
TEST(executor_tests, a_pool_is_neither_copied_nor_moved) {
  static_assert(!std::is_copy_constructible_v<executor>);
  static_assert(!std::is_copy_assignable_v<executor>);
  static_assert(!std::is_move_constructible_v<executor>);
  static_assert(!std::is_move_assignable_v<executor>);
}

// ---------------------------------------------------------------------------
// adaptive_mutex - the pool's lock: spins briefly, then blocks
// ---------------------------------------------------------------------------

//! Many threads, one counter that is not atomic: only the lock keeps the count exact.
TEST(adaptive_mutex_tests, excludes_under_contention) {
  untangle::adaptive_mutex mutex;
  int count = 0;
  constexpr int threads = 8;
  constexpr int rounds = 20'000;

  {
    std::vector<std::jthread> contenders;
    for (int t = 0; t < threads; ++t) {
      contenders.emplace_back([&] {
        for (int i = 0; i < rounds; ++i) {
          std::lock_guard<untangle::adaptive_mutex> lock(mutex);
          ++count;
        }
      });
    }
  }

  EXPECT_EQ(count, threads * rounds) << "two threads were inside the lock at once";
}

//! try_lock() never waits: it fails while another thread holds the lock.
TEST(adaptive_mutex_tests, try_lock_fails_while_held_elsewhere) {
  untangle::adaptive_mutex mutex;
  mutex.lock();

  bool taken_while_held = true;
  std::thread([&] { taken_while_held = mutex.try_lock(); }).join();
  EXPECT_FALSE(taken_while_held) << "try_lock() took a held lock";

  mutex.unlock();

  bool taken_when_free = false;
  std::thread([&] {
    taken_when_free = mutex.try_lock();
    if (taken_when_free) {
      mutex.unlock();
    }
  }).join();
  EXPECT_TRUE(taken_when_free) << "try_lock() failed on a free lock";
}

//! A holder that outlasts the spin: lock() blocks until it lets go, then takes it.
TEST(adaptive_mutex_tests, lock_waits_out_a_long_holder) {
  untangle::adaptive_mutex mutex;
  std::atomic_bool released = {false};
  std::atomic_bool taken = {false};

  mutex.lock();
  std::thread waiter([&] {
    std::lock_guard<untangle::adaptive_mutex> lock(mutex);
    taken = true;
    EXPECT_TRUE(released.load()) << "lock() returned while the lock was held";
  });

  std::this_thread::sleep_for(50ms);
  EXPECT_FALSE(taken.load()) << "lock() returned while the lock was held";

  released = true;
  mutex.unlock();
  waiter.join();

  EXPECT_TRUE(taken.load()) << "lock() never took the released lock";
}

//! The pool waits on it through std::condition_variable_any.
TEST(adaptive_mutex_tests, works_with_condition_variable_any) {
  untangle::adaptive_mutex mutex;
  std::condition_variable_any ready_cv;
  bool ready = false;

  std::thread notifier([&] {
    std::this_thread::sleep_for(10ms);
    {
      std::lock_guard<untangle::adaptive_mutex> lock(mutex);
      ready = true;
    }
    ready_cv.notify_all();
  });

  {
    std::unique_lock<untangle::adaptive_mutex> lock(mutex);
    EXPECT_TRUE(ready_cv.wait_for(lock, 5s, [&] { return ready; })) << "the wait never woke";
  }

  notifier.join();
}
