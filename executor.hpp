// Copyright (c) 2026 Nicolae Popescu. MIT License.

/**
 * @brief A thread pool executor built out of untangle::async::execution objects.
 *
 * N workers run in continuous mode behind a shared queue. A task goes straight to a free worker
 * only when the queue is empty; otherwise it joins the back, and a worker takes the front as it
 * frees up.
 */
#pragma once

#include <algorithm>
#include <async.hpp>
#include <atomic>
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
 * @tparam taskT The type of the task the pool runs. One pool runs one signature, and a non-void
 * return is run and dropped.
 */
template <typename taskT>
class executor {
  // A task is queued as it stands, with nothing bound to it, so it must take no arguments.
  static_assert(std::is_invocable_v<taskT>,
                "executor: the task type must be callable with no arguments");

 public:
  /**
   * @brief Builds \p worker_count workers. They do not run until \ref start().
   *
   * @param worker_count - How many workers to run. Must not be zero.
   *
   * @throws std::invalid_argument - \p worker_count is zero. Such a pool would take work it could
   * never run.
   */
  explicit executor(std::size_t worker_count) {
    if (worker_count == 0) {
      throw std::invalid_argument("executor: worker_count must not be zero");
    }

    workers_.reserve(worker_count);

    for (std::size_t index = 0; index < worker_count; ++index) {
      auto next = worker_execution::create_instance("pool_worker_" + std::to_string(index));

      // Tells the pool the moment this worker has room for more.
      next->on_finished = [this, index] { take_next_task(index); };

      // Carries what a task throws to the caller. Assigned here because a worker thread reads it.
      next->on_error = [this, name = next->name](std::exception_ptr thrown) {
        report_task_error(name, thrown);
      };

      // Adds the worker to the poll the destructor waits on.
      poll_.add(*next);

      workers_.push_back(std::move(next));
    }
  }

  /**
   * @brief Finishes what has been added, then stops every worker.
   *
   * Both waits are unbounded, and both report on stderr while they last, naming the workers they
   * are waiting on.
   *
   * @attention A task still running here is refused if it adds more work, even though this is
   * waiting for that very task. It is deliberate: a shutdown that took new work could be kept from
   * ever ending by a task that re-adds itself. \ref add_task() answers false, so the task can see
   * it.
   */
  ~executor() {
    // Whether the pool was still running when it was destroyed. A stopped one cannot empty its
    // queue - only a worker takes from it, through on_finished, and a stopped worker raises no more
    // - so its queue is not waited for.
    const bool was_running = running_.exchange(false);

    {
      std::unique_lock<std::mutex> lock(mutex_);

      // Waits for the queue to empty and every worker to go idle.
      auto interval = std::chrono::milliseconds(report_first_ms);
      auto waited = std::chrono::milliseconds(0);

      while (!finished_cv_.wait_for(lock, interval, [this, was_running] {
        return nothing_running() && (pending_.empty() || !was_running);
      })) {
        waited += interval;

        // Under the lock, so the counts agree with the predicate that just failed.
        std::println(
            stderr, "executor: still waiting to finish after {}s - {} queued, {} in a task{}",
            waited.count() / 1000, pending_.size(), count_workers(&worker_execution::is_busy),
            name_workers(&worker_execution::is_busy));

        interval = std::min(interval * 2, std::chrono::milliseconds(report_max_ms));
      }

      if (!pending_.empty()) {
        std::println(stderr,
                     "executor: {} queued task(s) dropped, the pool was stopped before they ran",
                     pending_.size());
      }
    }

    stop();

    // Waits for every worker to leave its thread, which is what makes destroying them safe. A
    // different question from the wait above: a worker can be idle and still be in its thread.
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

    // time of use: no worker thread is left to reach workers_.
    workers_.clear();
  }

  /**
   * @brief Queues a task, or hands it to a worker that has nothing to do.
   *
   * @return true - the task was accepted. false - it was refused, by a pool that has not been
   * started, has been stopped, or is being destroyed, or by a worker that has stopped. Either way
   * it will not run. A task calling this from a worker thread is answered like any other caller.
   */
  bool add_task(taskT task) {
    if (!running_) {
      return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // The queue comes first: a task does not overtake one already waiting in it.
    if (pending_.empty()) {
      for (std::size_t index = 0; index < workers_.size(); ++index) {
        if (!workers_[index]->is_busy()) {
          // A refused task is already destroyed, and stop() stops every worker together, so there
          // is nothing to queue and no other worker to try.
          return give_to_worker(index, std::move(task));
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
   * @brief Starts every worker, and starts the pool accepting work.
   *
   * A pool refuses work until this is called, and refuses it again after \ref stop() until this is
   * called a second time - a stopped pool is restarted by it, workers and all. Calling it twice
   * over is harmless.
   */
  void start() {
    // The workers first, so anything accepted after this is handed to one that is running.
    for (auto& one : workers_) {
      one->start();
    }

    running_ = true;
  }

  /**
   * @brief Stops every worker. What they are already running, they finish.
   *
   * Queued tasks stay unrun and a task added afterwards is refused. That holds whoever is asking,
   * a task already running on a worker included - a pool that is not running takes no work.
   * \ref start() gives the workers a new working life. Calling it twice is harmless.
   *
   * @attention It does not wait for the workers to leave their threads. Only the destructor does
   * that, and that wait is what makes destroying the pool safe.
   */
  void stop() {
    // First, so that nothing is accepted for workers that are about to stop. A task already on a
    // worker still runs; one arriving from here is refused.
    running_ = false;

    // No lock: a worker waking from stop() runs on_finished, which takes mutex_.
    for (auto& one : workers_) {
      one->stop();
    }
  }

  /**
   * @brief Called with whatever a task threw. Assigned by the caller.
   *
   * Nothing else reports a failed task: add_task() has answered long before the task runs. Left
   * unset, the pool warns on stderr instead.
   *
   * @remark It receives a std::exception_ptr, because a task may throw what is not a
   * std::exception. Rethrow it to read it.
   *
   * @attention It runs on a worker's thread, and several workers may be in it at once. Assign it
   * before the first add_task(), and let nothing escape it.
   */
  std::function<void(std::exception_ptr)> on_task_error;

 private:
  /**
   * @brief Hands what a task threw to \ref on_task_error, or warns on stderr when none is set.
   *
   * @param worker - The name of the worker that ran the task.
   * @param thrown - What the task threw.
   */
  void report_task_error(const std::string& worker, std::exception_ptr thrown) {
    if (on_task_error) {
      on_task_error(thrown);
      return;
    }

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
   * @return true - the worker took it, and is busy from here. false - the worker is stopped and has
   * destroyed the task, so \p task is gone either way and cannot be put back.
   */
  [[nodiscard]] bool give_to_worker(std::size_t index, taskT task) {
    // add_action() takes the worker's action_mutex under mutex_. The two never cycle, because a
    // worker calls on_finished with its action_mutex released.
    return workers_[index]->add_action(std::move(task));
  }

  /**
   * @brief Called on the worker's own thread once it has finished its queue.
   */
  void take_next_task(std::size_t index) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!pending_.empty()) {
      taskT next = std::move(pending_.front());
      pending_.pop_front();

      if (!give_to_worker(index, std::move(next))) {
        // The task is already destroyed, so it is reported rather than put back.
        std::println(stderr, "executor: {} refused a queued task, which is lost",
                     workers_[index]->name);
      }

      return;
    }

    if (nothing_running()) {
      finished_cv_.notify_all();
    }
  }

  /**
   * @brief Is every worker idle? Call with the mutex held.
   *
   * Asked of the workers rather than tallied here, so it cannot drift from what they are doing.
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
   * @brief How many workers \p state holds for. Call the is_busy() form with the mutex held.
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
   * @brief The names of those same workers, for a report that has to say which one.
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

  //! How often the destructor asks the poll whether the workers have left.
  static constexpr int poll_interval_ms = 50;

  //! How long a wait may be silent before it reports, and the ceiling its interval doubles to.
  static constexpr int report_first_ms = 1000;
  static constexpr int report_max_ms = 30000;

  mutable std::mutex mutex_;
  std::condition_variable finished_cv_;

  /**
   * @brief Stands in for joining the workers, which are detached and cannot be joined.
   *
   * Holds this pool's workers and nothing else, so the destructor waits for those and no others.
   */
  async::execution_poll poll_;

  std::deque<taskT> pending_;  //!< Tasks waiting for a worker, in the order added.
  std::vector<std::shared_ptr<worker_execution>> workers_;
  //! False until start(), and false again from stop() or the destructor. Read without the mutex.
  std::atomic_bool running_ = {false};
};

}  // namespace untangle
