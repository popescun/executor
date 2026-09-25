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
    pool.start();
    for (int i = 0; i < task_count; ++i) {
      EXPECT_TRUE(pool.add_action([&work] { work.run(); }));
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
    pool.add_action([&finished] {
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
    pool.start();
    for (int i = 0; i < task_count; ++i) {
      pool.add_action([&work, i] { work.run_numbered(i); });
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
      pool.add_action([&busy] { busy(); });
    }

    // Every worker must be at the gate first: one not yet started would be handed the task.
    ASSERT_TRUE(wait_for([&busy] { return busy.arrived() == static_cast<int>(worker_count); }, 5s));
    EXPECT_EQ(pool.pending(), 0u);

    for (std::size_t i = 0; i < queued_count; ++i) {
      pool.add_action([&work] { work.run(); });
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

    pool.add_action([&busy] { busy(); });
    ASSERT_TRUE(wait_for([&busy] { return busy.arrived() == 1; }, 5s));

    pool.add_action([&ran] { ran = true; });

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
      pool.add_action([&busy] { busy(); });
    }
    ASSERT_TRUE(wait_for([&busy] { return busy.arrived() == static_cast<int>(worker_count); }, 5s));

    for (std::size_t i = 0; i < queued_count; ++i) {
      pool.add_action([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
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
      pool.add_action([] { std::this_thread::sleep_for(150ms); });
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
          pool.add_action([&work] { work.run(); });
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

    pool.add_action([&work] { work.run(); });
    pool.add_action([] { throw std::runtime_error("a task that throws"); });
    pool.add_action([&work] { work.run(); });
    pool.add_action([] { throw 42; });  // not a std::exception; the catch-all arm
    pool.add_action([&work] { work.run(); });
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

    executor.add_action([] { throw std::runtime_error("a task that throws"); });
    executor.add_action([] { throw 42; });  // not a std::exception; the catch-all arm
  }

  EXPECT_EQ(reported.load(), 2) << "the executor swallowed what the tasks threw";
  EXPECT_EQ(reported_what, "a task that throws");
  EXPECT_EQ(reported_unknown.load(), 1);
}

/**
 * @brief A queued task a stopped worker refuses is reported rather than passed over.
 *
 * A stopped worker refuses what it is handed and destroys it. add_action() answered true for these
 * while the pool was still running, so the loss is named on stderr.
 *
 * @remark The queue is the only way to reach the refusal: stop() stops the pool accepting, so
 * add_action() never offers a worker anything after it. These were accepted before.
 */
TEST(executor_tests, a_queued_task_a_worker_refuses_is_reported) {
  constexpr int queued_count = 4;

  gate busy;
  std::atomic_int ran = {0};

  testing::internal::CaptureStderr();

  {
    executor executor(1);
    executor.start();

    // Occupies the only worker, so everything after it waits in the queue.
    EXPECT_TRUE(executor.add_action([&busy] { busy(); }));
    ASSERT_TRUE(wait_for([&busy] { return busy.arrived() == 1; }, 5s));

    for (int i = 0; i < queued_count; ++i) {
      EXPECT_TRUE(executor.add_action([&ran] { ran.fetch_add(1, std::memory_order_relaxed); }));
    }
    ASSERT_EQ(executor.pending(), static_cast<std::size_t>(queued_count));

    // The worker is stopped with the queue still full, and takes from it as it finishes.
    executor.stop();
    busy.open();
  }

  const std::string reported = testing::internal::GetCapturedStderr();

  EXPECT_EQ(ran.load(), 0) << "a refused task cannot have run";
  EXPECT_THAT(reported, ::testing::HasSubstr("refused a queued task"))
      << "the queued task was lost without a word; stderr held: " << reported;
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

    pool.add_action(
        [&work, &pool, &submitted] { submitted = pool.add_action([&work] { work.run(); }); });

    ASSERT_TRUE(wait_for([&submitted] { return submitted.load(); }, 5s));
  }
}

/**
 * @brief A pool that is shutting down refuses the task and says so.
 *
 * The refusal is only reachable from inside the pool: add_action() answers false once the
 * destructor has stopped accepting, and the destructor is waiting for this very task to return. So
 * the task polls, and the case fails as a timeout rather than as a hang.
 */
TEST(executor_tests, add_action_is_refused_once_the_pool_is_shutting_down) {
  auto pool = std::make_unique<executor>(2);
  pool->start();

  // The task reads the pool through a raw pointer, not through the unique_ptr: reset() clears the
  // pointer before it runs the destructor, so a task reading the unique_ptr would find it null
  // while the object it is running on is still very much alive.
  executor* const running_pool = pool.get();

  std::atomic_bool refused = {false};
  std::atomic_bool started = {false};

  running_pool->add_action([running_pool, &refused, &started] {
    started = true;
    refused = wait_for([running_pool] { return !running_pool->add_action([] {}); }, 10s);
  });

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

    pool.add_action([&pool, &hold, &refused, &spawned] {
      hold();  // released once the pool has been stopped
      refused = !pool.add_action([&spawned] { spawned = true; });
    });

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

    pool.add_action([&busy] { busy(); });
    ASSERT_TRUE(wait_for([&busy] { return busy.arrived() == 1; }, 5s));

    // The task at the gate is the worker's, not the queue's.
    EXPECT_EQ(pool.pending(), 0u);

    pool.add_action([] {});
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

    EXPECT_FALSE(pool.add_action([&ran] { ran.fetch_add(1, std::memory_order_relaxed); }));
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
      EXPECT_TRUE(pool.add_action([&work] { work.run(); }));
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
    EXPECT_TRUE(pool.add_action(task));

    pool.stop();
    EXPECT_FALSE(pool.add_action(task)) << "a stopped pool took work";

    pool.start();
    EXPECT_TRUE(pool.add_action(task)) << "a restarted pool refused work";
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

    pool.add_action([&started, stall] {
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
    pool.start();

    for (int i = 0; i < tasks; ++i) {
      EXPECT_TRUE(pool.add_action([&ran] { return ran.fetch_add(1, std::memory_order_relaxed); }));
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
      EXPECT_TRUE(pool.add_action(counting_task{&ran}));
    }
  }

  EXPECT_EQ(ran.load(), tasks);
}

// --- step 27, item 24 ----------------------------------------------------------------------------
// The two cases below do not compile: executor.hpp:42 refuses a task type that takes arguments, so
// the whole binary fails to build and the 25 cases above it cannot be run until the fix lands. They
// are left live deliberately - the refusal is the finding, and a guarded case would hide it.

/**
 * @brief A pool runs a task type that takes arguments and returns a value.
 *
 * The signature async::execution already accepts at its own door - add_action() takes an action and
 * the arguments to bind to it - asked of the pool, which today takes the task alone. Nothing about
 * the return is new: a continuous worker collects nothing, so what the task returns is run and
 * dropped, exactly as in a_pool_runs_tasks_that_return_a_value. It is here because a task type is
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
      EXPECT_TRUE(pool.add_action(
          [&work](int n) {
            work.run_numbered(n);
            return n * 2;
          },
          i));
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
    EXPECT_TRUE(pool.add_action(
        [&blocker](int n) {
          blocker();
          return n;
        },
        0));
    ASSERT_TRUE(wait_for([&blocker] { return blocker.arrived() == 1; }, 2s))
        << "the worker never reached the gate, so the tasks below were not queued";

    for (int i = 0; i < task_count; ++i) {
      EXPECT_TRUE(pool.add_action(
          [&work](int n) {
            work.run_numbered(n);
            return n * 2;
          },
          i));
    }

    EXPECT_EQ(pool.pending(), task_count) << "the tasks did not wait in the queue";

    blocker.open();
  }
}

// --- tasks on the pool ---------------------------------------------------------------------------
// The pool has two doors. add_action() is fire and forget: the task runs, what it returns is
// dropped, and the caller hears nothing more. add_task() carries the callback it must notify, built
// by untangle::bind_task() with the callback as its last argument.
//
// Both kinds share one queue, and pending_ keeps them in the order they arrived - which is a
// promise the pool can make and async's queue cannot, because the pool owns its container outright
// while async fires an actuator in two passes. See todo/FEATURE_PLAN.md step 2.

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

TEST(executor_tests, the_queue_keeps_both_kinds_in_the_order_they_arrived) {
  // The promise this pool makes and async's queue does not: one container, so an action posted
  // before a task runs before it. The single worker is held at the gate, so everything below it
  // waits in pending_ and comes out in order.
  using int_executor = untangle::executor<std::function<int(int)>>;

  std::mutex order_mutex;
  std::vector<std::string> order;
  const auto note = [&order_mutex, &order](std::string what) {
    std::lock_guard<std::mutex> lock(order_mutex);
    order.push_back(std::move(what));
  };

  gate blocker;

  {
    int_executor pool(1);
    pool.start();

    EXPECT_TRUE(pool.add_action(
        [&blocker](int n) {
          blocker();
          return n;
        },
        0));
    ASSERT_TRUE(wait_for([&blocker] { return blocker.arrived() == 1; }, 2s))
        << "the worker never reached the gate, so nothing below it was queued";

    EXPECT_TRUE(pool.add_action(
        [&note](int n) {
          note("action 1");
          return n;
        },
        1));
    EXPECT_TRUE(pool.add_task(
        [&note](int n) {
          note("task 2");
          return n;
        },
        2, [](int) {}));
    EXPECT_TRUE(pool.add_action(
        [&note](int n) {
          note("action 3");
          return n;
        },
        3));

    EXPECT_EQ(pool.pending(), 3u) << "pending() does not count both kinds";

    blocker.open();
  }

  std::lock_guard<std::mutex> lock(order_mutex);
  EXPECT_THAT(order, testing::ElementsAre("action 1", "task 2", "action 3"))
      << "the queue reordered the two kinds rather than keeping them as they arrived";
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
      pool.add_action([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
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
