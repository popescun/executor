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
#include <functional>
#include <memory>
#include <mutex>
#include <print>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace untangle {

/**
 * @brief Runs tasks on a fixed pool of untangle::async::execution workers.
 *
 * @remark Tasks are std::function<void(void)>. An execution's action type is fixed by its template
 * argument, so one pool runs one signature; a task that needs to return something carries its own
 * channel, because a continuous worker does not collect results.
 */
class executor {
 public:
  //! The work a pool runs. One pool runs one signature, because an execution's action type is
  //! fixed by its template argument.
  using task_t = std::function<void(void)>;

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

    // Outside the lock: stop() wakes each worker, and a worker waking up runs on_finished, which
    // wants this mutex.
    for (auto& one : workers_) {
      one->stop();
    }

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
  bool submit(task_t task) {
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

 private:
  using worker_execution = untangle::async::execution<std::function<void(void)>>;

  /**
   * @brief Hands one task to a worker. Call with the mutex held.
   *
   * The worker is busy from the moment add_action() returns - it says so itself - so nothing has to
   * be booked here.
   */
  void give_to_worker(std::size_t index, task_t task) {
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
      task_t next = std::move(pending_.front());
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
   * async's own warnings. The task cannot be named: task_t is std::function<void(void)> and carries
   * nothing to report.
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

  std::deque<task_t> pending_;  //!< Tasks waiting for a worker, in the order submitted.
  std::vector<std::shared_ptr<worker_execution>> workers_;
  bool accepting_ = true;
};

}  // namespace untangle
