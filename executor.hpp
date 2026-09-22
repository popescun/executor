// Copyright (c) 2026 Nicolae Popescu. MIT License.

/**
 * @brief A thread pool executor built out of untangle::async::execution objects.
 *
 * Imported from the async repo's prototypes/executor.hpp, unchanged apart from its namespace and
 * this comment. The hardening the prototype's README asks for has not been done yet.
 *
 * The pool holds N executions in continuous mode and a shared queue of tasks. A task is handed
 * straight to a free worker only when the shared queue is empty; otherwise it goes to the back of
 * the queue, and a worker takes the front of the queue as it frees up. Nothing overtakes: finding a
 * free worker does not let a task jump a queue that already has work in it.
 */
#pragma once

#include <algorithm>
#include <async.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <print>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace untangle {

/**
 * @brief Runs tasks on a fixed pool of untangle::async::execution workers.
 *
 * @tparam taskT The type of the task the pool runs, specified as std::function<...> like the
 * execution it is handed to. One pool runs one signature, because an execution's action type is
 * fixed by its own template argument.
 *
 * @remark A task that needs to return something carries its own channel: a continuous worker does
 * not collect results, so a non-void return is run and dropped.
 */
template <typename taskT>
class executor {
  // add_action() is the only way in, and the pool calls it with no arguments to bind: what it
  // queues is the task itself, and there is nowhere to keep arguments alongside it until a worker
  // is free. Stated here so a pool on std::function<void(int)> says why rather than failing inside
  // std::bind.
  static_assert(std::is_invocable_v<taskT>,
                "executor: the task type must be callable with no arguments");

 public:
  /**
   * @brief Starts \p worker_count executions in continuous mode.
   *
   * @param worker_count - How many workers to run. Must not be zero.
   *
   * @throws std::invalid_argument - \p worker_count is zero. Such a pool would take work and never
   * run it, so it is refused before any worker starts rather than left to hang in the destructor.
   *
   * @remark std::thread::hardware_concurrency() returns 0 when it cannot tell how many cores there
   * are, and a caller passing that straight through is the likely way in. Choosing a default then
   * is the caller's call, not the pool's.
   */
  explicit executor(std::size_t worker_count) {
    if (worker_count == 0) {
      throw std::invalid_argument("executor: worker_count must not be zero");
    }

    workers_.reserve(worker_count);

    for (std::size_t index = 0; index < worker_count; ++index) {
      auto next = worker_execution::create_instance("pool_worker_" + std::to_string(index));

      // How a worker learns there is more waiting for it. is_busy() answers whether a worker has
      // room; this is what tells the pool the moment one frees up, without anybody polling.
      next->on_finished = [this, index] { take_next_task(index); };

      // What a task throws, on its way to whoever submitted it. The worker catches it and would
      // otherwise print a warning naming itself, which reaches a log and no code.
      //
      // Assigned whether or not a caller has set on_task_error, because it cannot be assigned later
      // - a worker thread reads it, and this is the last moment nothing is running. The name is
      // captured rather than looked up for the same reason: report_task_error() runs on the
      // worker's thread and taking mutex_ there to read workers_ would be a lock this path does not
      // need.
      next->on_error = [this, name = next->name](std::exception_ptr thrown) {
        report_task_error(name, thrown);
      };

      // A worker runs in a detached thread and cannot be joined, so asking whether it is still
      // running is the only way to wait for one, and the poll answers that for any number of them
      // at once. A poll answers for the executions added to it, and this one is added the pool's
      // own workers and nothing else, so what the destructor waits for is exactly these. Nothing
      // here has to unregister - the record runs both ways, so whichever of the two dies first
      // takes itself out of the other.
      poll_.add(*next);

      workers_.push_back(std::move(next));
      workers_.back()->start();
    }
  }

  /**
   * @brief Finishes what has been submitted, then stops every worker.
   *
   * Both waits are unbounded - a pool that abandoned a running task would be worse than one that
   * waits for it - and both report on stderr rather than stalling in silence, naming the workers
   * they are still waiting on.
   *
   * @remark Every worker has left its thread before the first execution is destroyed. ~execution()
   * waits only for its *own* worker, so destroying them one at a time would free the first while
   * another's thread was still reading it through nothing_running(); the poll waits for all of
   * them. It is this pool's own poll, so a second pool running alongside it is not waited for.
   */
  ~executor() {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      accepting_ = false;

      // Unbounded, and reported rather than bounded. Both waits below keep waiting for as long as
      // it takes; what they stop doing is keeping it to themselves.
      auto interval = std::chrono::milliseconds(report_first_ms);
      auto waited = std::chrono::milliseconds(0);

      while (!finished_cv_.wait_for(lock, interval,
                                    [this] { return pending_.empty() && nothing_running(); })) {
        waited += interval;

        // The lock is held here, which is what makes both reads consistent with the predicate that
        // just failed. is_busy() takes each execution's action_mutex_ under this one, which is the
        // same order give_to_worker() uses - no new lock order is introduced by reporting.
        std::println(
            stderr, "executor: still waiting to finish after {}s - {} queued, {} in a task{}",
            waited.count() / 1000, pending_.size(), count_workers(&worker_execution::is_busy),
            name_workers(&worker_execution::is_busy));

        interval = std::min(interval * 2, std::chrono::milliseconds(report_max_ms));
      }
    }

    // The workers are stopped through the same call a caller would make, after the wait above has
    // left nothing for them to do.
    stop();

    // The poll stands in for a join. Until it reports idle, a worker can still be inside
    // take_next_task(), reading every execution through nothing_running() and is_busy() - which is
    // what makes destroying them here a use-after-free rather than a teardown.
    //
    // A different question from the finish above, and reported as one: that wait is about workers
    // still inside a task, this is about workers whose thread has not left. A worker can be idle
    // and still be here.
    auto interval = std::chrono::milliseconds(report_first_ms);
    auto waited = std::chrono::milliseconds(0);

    while (poll_.is_running()) {  // time of check
      const auto tick = std::chrono::milliseconds(poll_interval_ms);
      std::this_thread::sleep_for(tick);
      waited += tick;

      if (waited >= interval) {
        std::println(stderr, "executor: still waiting to stop after {}s - {} not left its thread{}",
                     waited.count() / 1000, count_workers(&worker_execution::is_running),
                     name_workers(&worker_execution::is_running));

        interval = std::min(interval * 2, std::chrono::milliseconds(report_max_ms));
      }
    }

    // time of use: no worker thread is left to reach workers_, so clearing it races nothing.
    workers_.clear();
  }

  /**
   * @brief Queues a task, or hands it to a worker that has nothing to do.
   *
   * @return true - the task was accepted. false - the pool is shutting down and refused it.
   */
  bool add_task(taskT task) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!accepting_) {
      return false;
    }

    // The queue comes first whenever it has anything in it: a task that arrives while others are
    // waiting does not get to overtake them just because a worker happens to be free.
    if (pending_.empty()) {
      for (std::size_t index = 0; index < workers_.size(); ++index) {
        if (!workers_[index]->is_busy()) {
          give_to_worker(index, std::move(task));
          return true;
        }
      }
    }

    pending_.push_back(std::move(task));
    return true;
  }

  /**
   * @brief How many tasks are waiting for a worker.
   */
  std::size_t pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_.size();
  }

  /**
   * @brief Stops every worker. What they are already running, they finish.
   *
   * A worker stops taking work from the moment this returns, so anything still in the queue stays
   * there unrun, and a task added afterwards is refused by the worker it is handed to. The
   * destructor calls this itself, after waiting for the queue to empty - which is the difference
   * between stopping a pool and finishing one.
   *
   * @remark Calling it twice is harmless: the second stop() finds a worker that has already
   * stopped and does nothing.
   *
   * @attention It does not wait for the workers to leave their threads. Only the destructor does
   * that, and it is what makes destroying the pool safe rather than this.
   */
  void stop() {
    // Outside any lock: stop() wakes each worker, and a worker waking up runs on_finished, which
    // wants mutex_. workers_ is not written after the constructor, so reading it here races
    // nothing - only the destructor clears it, and only after the poll says no worker is left.
    for (auto& one : workers_) {
      one->stop();
    }
  }

  /**
   * @brief Called with whatever a task threw. Assigned by the caller.
   *
   * The pool runs the caller's code and nothing comes back from it: add_task() has answered true
   * long before the task runs, and a task that failed is otherwise indistinguishable from one that
   * succeeded. This is the one way out. Without it the only record is a warning on stderr naming a
   * worker the caller neither chose nor can look up.
   *
   * @remark It receives a std::exception_ptr because a task may throw something that is not a
   * std::exception, and that case is the one most worth hearing about. Rethrow it to read it.
   *
   * @attention It runs on a worker's thread, and more than one worker may be in it at once -
   * whatever it touches has to be safe for that. Assign it before the first add_task(): a worker
   * reads it, so assigning it while tasks are running is a data race.
   *
   * @attention Nothing may escape it. It is called from inside the worker's own catch, and an
   * exception leaving a detached thread function calls std::terminate - async catches around it to
   * keep that from being fatal, but the exception is then lost.
   */
  std::function<void(std::exception_ptr)> on_task_error;

 private:
  /**
   * @brief Hands what a task threw to \ref on_task_error, or keeps the floor if nobody is
   * listening.
   *
   * @param worker - The name of the worker that ran the task, for the warning.
   * @param thrown - What the task threw.
   */
  void report_task_error(const std::string& worker, std::exception_ptr thrown) {
    if (on_task_error) {
      on_task_error(thrown);
      return;
    }

    // Nobody is listening, so this stands in for the warning async would have printed had the pool
    // not taken its on_error. Losing it silently would make a pool worse than the execution it
    // wraps.
    try {
      std::rethrow_exception(thrown);
    } catch (const std::exception& e) {
      std::println(stderr, "warning: executor task on '{}' threw: {}", worker, e.what());
    } catch (...) {
      std::println(stderr, "warning: executor task on '{}' threw an unknown type", worker);
    }
  }

  using worker_execution = untangle::async::execution<taskT>;

  /**
   * @brief Hands one task to a worker. Call with the mutex held.
   *
   * The worker is busy from the moment add_action() returns - it says so itself - so nothing has to
   * be booked here.
   */
  void give_to_worker(std::size_t index, taskT task) {
    // add_action() takes the execution's own action_mutex while this holds mutex_. That is only
    // safe in one direction, and it holds: a worker calls on_finished with action_mutex released,
    // so it never takes mutex_ while holding action_mutex, and the two never form a cycle.
    workers_[index]->add_action(std::move(task));
  }

  /**
   * @brief Called on the worker's own thread once it has finished its queue.
   */
  void take_next_task(std::size_t index) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!pending_.empty()) {
      taskT next = std::move(pending_.front());
      pending_.pop_front();
      give_to_worker(index, std::move(next));
      return;
    }

    if (nothing_running()) {
      finished_cv_.notify_all();
    }
  }

  /**
   * @brief Is every worker idle? Call with the mutex held.
   *
   * Asked of the workers rather than counted here. A tally of what the pool dispatched would be the
   * pool's belief about the workers; this is the workers' own answer, and it cannot drift.
   */
  bool nothing_running() const {
    for (const auto& one : workers_) {
      if (one->is_busy()) {
        return false;
      }
    }

    return true;
  }

  /**
   * @brief Counts the workers a predicate holds for. Call the is_busy() form with the mutex held.
   *
   * @param state - \ref worker_execution::is_busy or \ref worker_execution::is_running.
   */
  std::size_t count_workers(bool (worker_execution::*state)() const) const {
    std::size_t count = 0;

    for (const auto& one : workers_) {
      if ((one.get()->*state)()) {
        ++count;
      }
    }

    return count;
  }

  /**
   * @brief Names those same workers, for a report that has to say which one.
   *
   * The execution's own name rather than the index, so one worker reads the same here and in
   * async's own warnings. The task cannot be named: taskT is a callable and carries nothing to
   * report.
   *
   * @return The matching names behind a ": " separator, empty when none match, so it appends to a
   * message cleanly either way.
   */
  std::string name_workers(bool (worker_execution::*state)() const) const {
    std::string names;

    for (const auto& one : workers_) {
      if ((one.get()->*state)()) {
        names += names.empty() ? ": " : ", ";
        names += one->name;
      }
    }

    return names;
  }

  //! How often the destructor asks the poll whether the workers have left. Matches the interval
  //! async's own smoke test waits at.
  static constexpr int poll_interval_ms = 50;

  //! How long a shutdown may be silent before it reports, and the ceiling the interval doubles to.
  //! A stuck teardown says something almost at once; a long one does not turn into a log flood.
  static constexpr int report_first_ms = 1000;
  static constexpr int report_max_ms = 30000;

  mutable std::mutex mutex_;
  std::condition_variable finished_cv_;

  /**
   * @brief Stands in for joining this pool's workers, which are detached and cannot be joined.
   *
   * Holds this pool's workers and nothing else. A poll answers for the executions added to it, so
   * one shared with a second pool would make this destructor wait for that pool's workers too -
   * and a continuous worker runs for the life of its execution, so that wait would not end.
   *
   * Declared before the workers so it outlives them, though it does not depend on that: an
   * execution withdraws from every poll holding it, and a poll releases every execution it holds.
   */
  async::execution_poll poll_;

  std::deque<taskT> pending_;  //!< Tasks waiting for a worker, in the order submitted.
  std::vector<std::shared_ptr<worker_execution>> workers_;
  bool accepting_ = true;
};

}  // namespace untangle
