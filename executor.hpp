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
 * @tparam taskT The type of the task the pool runs. One pool runs one signature - its arguments as
 * much as its result - and a non-void return is run and dropped.
 */
template <typename taskT>
class executor {
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
   * @brief Queues a task bound to \p args, or hands it to a worker that has nothing to do.
   *
   * The arguments are bound here, while the caller still holds them, and what waits for a worker is
   * one callable carrying its own. That is the shape async::execution::add_action() offers at its
   * door, and it is bound here for the same reason it is bound there: a queue holds callables, and
   * a call site cannot be queued.
   *
   * @remark The first free worker takes it, counting from the start, so a pool that is not busy
   * keeps giving work to the same one. That is the intent: the worker that just ran a task is the
   * warm one, and a free worker is free whichever it is.
   *
   * @tparam Args - The argument types to bind to \p task.
   * @param task - The task to run.
   * @param args - What to bind to it. Copied here and handed to the task as the pool's own lvalues
   * when it runs, so a task taking a reference is given that copy rather than the caller's object,
   * which may be gone by then.
   *
   * @return true - the task was accepted. false - it was refused, by a pool that has not been
   * started, has been stopped, or is being destroyed, or by a worker that has stopped. Either way
   * it will not run. A task calling this from a worker thread is answered like any other caller.
   */
  template <typename... Args>
  bool add_task(taskT task, Args&&... args) {
    // The call the pool will make, which is not quite the one written at the call site: what
    // reaches the task is the copies bound below, so that is the call that has to compile.
    static_assert(std::is_invocable_v<taskT, std::decay_t<Args>&...>,
                  "executor: the task cannot be called with the arguments given to add_task()");

    if (!running_) {
      return false;
    }

    // Bound before the lock: it copies the caller's arguments, and nothing here needs the queue.
    task_call call = bind_task(std::move(task), std::forward<Args>(args)...);

    std::lock_guard<std::mutex> lock(mutex_);

    // The queue comes first: a task does not overtake one already waiting in it.
    if (pending_.empty()) {
      // Always from the start, so a free pool reuses its warmest worker rather than waking a cold
      // one. Under load the workers are busy and the queue below is what spreads the work.
      for (std::size_t index = 0; index < workers_.size(); ++index) {
        if (!workers_[index]->is_busy()) {
          // A refused task is already destroyed, and stop() stops every worker together, so there
          // is nothing to queue and no other worker to try.
          return give_to_worker(index, std::move(call));
        }
      }
    }

    pending_.push_back(std::move(call));
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

  /**
   * @brief A task with its arguments already bound: what the pool queues, and what a worker runs.
   *
   * @remark It names its own result_type rather than leaving async to read one off a std::function,
   * whose result_type C++20 removed. A pool built on a caller's own functor therefore still never
   * touches that typedef, which is what keeps \ref taskT free to be something other than a
   * std::function.
   */
  struct task_call {
    using result_type = typename taskT::result_type;

    result_type operator()() { return call(); }

    std::function<result_type(void)> call;
  };

  /**
   * @brief Binds \p args into \p task, making the one callable the queue and the workers both take.
   *
   * @remark With no arguments to bind, the task is that callable already, and wrapping it would add
   * a layer to every pool that never had an argument to give.
   */
  template <typename... Args>
  static task_call bind_task(taskT task, Args&&... args) {
    if constexpr (sizeof...(Args) == 0) {
      return task_call{std::move(task)};
    } else {
      // Captured by value and called as lvalues, which is all a call deferred to a worker can
      // promise: whatever was passed to add_task() may be gone by the time this runs.
      return task_call{[task = std::move(task), ... args = std::forward<Args>(args)]() mutable ->
                       typename task_call::result_type { return task(args...); }};
    }
  }

  using worker_execution = untangle::async::execution<task_call>;

  /**
   * @brief Hands one task to a worker. Call with the mutex held.
   *
   * @return true - the worker took it, and is busy from here. false - the worker is stopped and has
   * destroyed the task, so \p task is gone either way and cannot be put back.
   */
  [[nodiscard]] bool give_to_worker(std::size_t index, task_call task) {
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
      task_call next = std::move(pending_.front());
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

  //! Tasks waiting for a worker, each bound to its arguments, in the order added.
  std::deque<task_call> pending_;
  std::vector<std::shared_ptr<worker_execution>> workers_;
  //! False until start(), and false again from stop() or the destructor. Read without the mutex.
  std::atomic_bool running_ = {false};
};

}  // namespace untangle
