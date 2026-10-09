// Copyright (c) 2026 Nicolae Popescu. MIT License.

/**
 * @brief A thread pool executor built out of untangle::async::execution objects.
 *
 * N workers in continuous mode behind a shared queue. Nothing overtakes what is already queued.
 */
#ifndef UNTANGLE_EXECUTOR_EXECUTOR_HPP
#define UNTANGLE_EXECUTOR_EXECUTOR_HPP

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
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace untangle {

/**
 * @brief A mutex that spins briefly before it blocks.
 *
 * The pool's lock is held for a few instructions at a time, by the submitter and by every worker
 * after every batch. A std::mutex blocks in the kernel at once on macOS, so each meeting cost a
 * pair of syscalls; spinning first ends most of them in user space. Measured with
 * bench/qt_pool_vs_this.
 */
class adaptive_mutex {
 public:
  //! How many times lock() tries before it blocks.
  static constexpr int spin_limit = 100;

  //! Tries up to spin_limit times, then blocks until the lock is free.
  void lock() {
    for (int attempt = 0; attempt < spin_limit; ++attempt) {
      if (mutex_.try_lock()) {
        return;
      }
      cpu_pause();
    }
    mutex_.lock();
  }

  //! Takes the lock if it is free, and never waits.
  bool try_lock() { return mutex_.try_lock(); }

  //! Releases the lock.
  void unlock() { mutex_.unlock(); }

 private:
  //! Tells the core this is a spin, so it saves power and yields to its sibling thread.
  static void cpu_pause() noexcept {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#elif defined(_MSC_VER) && defined(_M_ARM64)
    __yield();
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield");
#endif
  }

  std::mutex mutex_;
};

/**
 * @brief Runs tasks on a fixed pool of untangle::async::execution workers.
 *
 * \ref add_task() is the one door: it carries a callback and reports the result to it.
 *
 * @tparam actionT - The callable the pool runs. One pool runs one signature, arguments and result.
 */
template <typename actionT>
class executor {
 public:
  /**
   * @brief Builds \p worker_count workers. They do not run until \ref start().
   *
   * @param worker_count - How many workers to run. Must not be zero.
   * @throws std::invalid_argument - \p worker_count is zero: such a pool could never run its work.
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
   * @brief Drains what was added, then stops the workers and destroys them.
   *
   * The drain is \ref wait(); the shutdown after it stops the workers, waits for each to leave its
   * thread, and destroys them only then. Both waits are unbounded and report on stderr meanwhile.
   *
   * @attention Only a pool stopped before its queue emptied leaves work behind, and work dropped
   * here never runs, so it never notifies. The count on stderr says how much, never which.
   *
   * @attention This waits for the pool, not for what its work touched: see \ref wait().
   */
  ~executor() {
    // Nothing is stopped before this, or the queue could never empty. wait() leaves the pool
    // accepting, so stop() follows at once: that window is the only one a shutdown would take.
    wait();
    stop();

    {
      std::lock_guard<adaptive_mutex> lock(mutex_);

      // Only reachable for a pool that was already stopped. wait() drained a running one, and
      // stop() above cannot add to the queue.
      if (!pending_.empty()) {
        std::println(stderr,
                     "executor: {} queued task(s) dropped, the pool was stopped before they ran",
                     pending_.size());
      }
    }

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
   * @brief Drains everything added so far, then leaves the pool as it was found.
   *
   * Drained means the queue is empty and no worker is busy, so every task accepted before this has
   * run, and so has the callback of one - a callback runs inside the worker's drain, not after it.
   *
   * A barrier, not a shutdown: the workers keep their threads and work added afterwards runs as
   * before. Reusable, and one at a time - a second caller drains once the first has returned.
   *
   * The wait is unbounded and reports on stderr while it lasts.
   *
   * @attention New work is refused meanwhile, then accepting is restored, so a task that re-adds
   * itself cannot keep the drain from ending. Only work accepted before it began counts.
   *
   * @attention A pool that is not running is not waited for: only a worker takes from the queue, so
   * this returns at once and what is queued keeps for the next \ref start().
   *
   * @attention It waits for the work, not for what the work touched. Held as a member, ~executor()
   * runs once the owner's derived parts and later members are gone: call this before that.
   */
  void wait() {
    // One drain at a time: two threads between the exchange below and the restore at the end would
    // read each other's cleared flag, and the last one out could leave the pool refusing work.
    std::lock_guard<std::mutex> drain(wait_mutex_);

    // Put back exactly as found below: a wait must not leave a stopped pool accepting, and such a
    // pool cannot empty its queue anyway - only a running worker takes from it.
    const bool was_running = running_.exchange(false);

    {
      std::unique_lock<adaptive_mutex> lock(mutex_);

      // Waits for the queue to empty and every worker to go idle.
      auto interval = std::chrono::milliseconds(report_first_ms);
      auto waited = std::chrono::milliseconds(0);

      while (!finished_cv_.wait_for(lock, interval, [this, was_running] {
        return nothing_running() && (pending_.empty() || !was_running);
      })) {
        waited += interval;

        // Under the lock, so the counts agree with the predicate that just failed.
        std::println(
            stderr, "executor: still waiting to drain after {}s - {} queued, {} in a task{}",
            waited.count() / 1000, pending_.size(), count_workers(&worker_execution::is_busy),
            name_workers(&worker_execution::is_busy));

        interval = std::min(interval * 2, std::chrono::milliseconds(report_max_ms));
      }
    }

    running_ = was_running;
  }

  /**
   * @brief Queues work that must report, or hands it to a worker that has nothing to do.
   *
   * The first free worker takes it, the warmest one; otherwise it waits in the queue, which keeps
   * the order work arrived in. Once the task has run, its callback is invoked with the result, on
   * the worker's thread.
   *
   * @attention The last argument is the callback, which the signature cannot say: it arrives in
   * \p args and untangle::bind_task() splits it off. A missing or unusable one is a static_assert,
   * and so is a task that cannot be called with the arguments before it.
   *
   * @attention A task that throws is not notified - what it threw goes to \ref on_task_error - and
   * a refused one does not notify either, so a caller waiting on the callback alone waits for ever.
   *
   * @attention An empty callback or action is refused here too. Only a run time check can see it,
   * and a task that can neither run nor notify is turned away rather than queued to throw.
   *
   * @tparam Args - The argument types to bind, of which the last is the callback.
   * @param task - The task to run.
   * @param args - What to bind, followed by the callback. Copied here, so a task taking a reference
   * gets the pool's copy.
   *
   * @return true - accepted, and the callback will be notified once it has run.
   * @return false - refused by a pool not started, stopped, draining or being destroyed, by a
   * stopped worker, or because the task could never do its job. It will not run or notify.
   */
  template <typename... Args>
  bool add_task(actionT task, Args&&... args) {
    // The call the pool will make, which is not quite the one written at the call site: what
    // reaches the task is the copies bound below, so that is the call that has to compile. Without
    // this the mismatch surfaces deep inside bind_task()'s std::apply.
    static_assert(invocable_with_bound<Args...>(),
                  "executor: the task cannot be called with the arguments given to add_task() "
                  "before its callback");

    if (!running_) {
      return false;
    }

    // untangle::bind_task() takes the callback off the end of the pack and checks it; what comes
    // back is one callable that runs the work and then notifies, which is what the queue holds.
    auto tsk = untangle::bind_task(std::move(task), std::forward<Args>(args)...);

    // bind_task() builds an empty one from an empty action or callback. Queued, it would throw on a
    // worker and report to on_task_error rather than to the caller, who could still act on it.
    if (!tsk) {
      return false;
    }

    return queue_task(std::move(tsk));
  }

  /**
   * @brief How many tasks are waiting for a worker.
   */
  std::size_t pending() const {
    std::lock_guard<adaptive_mutex> lock(mutex_);
    return pending_.size();
  }

  /**
   * @brief Starts every worker, and starts the pool accepting work.
   *
   * A pool refuses work until this is called, and again after \ref stop() until it is called anew,
   * which revives the workers too. Harmless twice over. \ref wait() needs nothing from here.
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
   * Queued tasks stay unrun and anything added afterwards is refused, a running task included.
   * \ref start() revives the workers; harmless twice over.
   *
   * @attention It does not wait for the workers to leave their threads - only the destructor does,
   * and that wait is what makes destroying the pool safe.
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
   * @brief Called with whatever the work threw. Assigned by the caller.
   *
   * The only report of failed work, since \ref add_task() answered long
   * before it ran. Left unset, the pool warns on stderr. Rethrow the exception_ptr to read it.
   *
   * @remark A task's failure arrives here, and so does its callback's - so this can fire for a task
   * that succeeded, when only the telling failed.
   *
   * @attention It runs on a worker's thread, several at once. Assign it before the first
   * \ref add_task(), and let nothing escape it or a callback.
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
   * @brief Can a task be called with the copies bound from every one of \p Args but the last?
   *
   * The last is the callback, which untangle::bind_task() checks itself, and the rest reach the
   * task as the pool's own lvalues - so that, not the caller's argument types, is what is asked.
   *
   * @remark With no arguments at all there is no callback either, and bind_task() says so; this
   * answers true rather than adding a second, misleading error to its.
   */
  template <typename... Args>
  static consteval bool invocable_with_bound() {
    if constexpr (sizeof...(Args) == 0) {
      return true;
    } else {
      return invocable_with<std::tuple<std::decay_t<Args>...>>(
          std::make_index_sequence<sizeof...(Args) - 1>{});
    }
  }

  //! The half of invocable_with_bound() that needs the indices of what is bound.
  template <typename tupleT, std::size_t... index>
  static consteval bool invocable_with(std::index_sequence<index...>) {
    return std::is_invocable_v<actionT, std::tuple_element_t<index, tupleT>&...>;
  }

  using worker_execution = untangle::async::execution<untangle::task_t>;

  /**
   * @brief Hands one entry to a free worker, or puts it at the back of the queue.
   *
   * What \ref add_task() does once bound: one queue, order out as in.
   *
   * @return true - taken. false - a worker refused it, and it is gone.
   */
  bool queue_task(untangle::task_t task) {
    std::lock_guard<adaptive_mutex> lock(mutex_);

    // The queue comes first: an entry does not overtake one already waiting in it.
    if (pending_.empty()) {
      // Always from the start, so a free pool reuses its warmest worker rather than waking a cold
      // one. Under load the workers are busy and the queue below is what spreads the work.
      for (std::size_t index = 0; index < workers_.size(); ++index) {
        if (!workers_[index]->is_busy()) {
          // A refused entry is already destroyed, and stop() stops every worker together, so there
          // is nothing to queue and no other worker to try.
          return give_to_worker(index, std::move(task));
        }
      }
    }

    pending_.push_back(std::move(task));
    return true;
  }

  /**
   * @brief Hands one entry to a worker. Call with the mutex held.
   *
   * @return true - the worker took it. false - it is stopped and has destroyed the entry, which
   * cannot be put back either way.
   */
  [[nodiscard]] bool give_to_worker(std::size_t index, untangle::task_t task) {
    // The door takes the worker's action_mutex under mutex_. The two never cycle, because a worker
    // calls on_finished with its action_mutex released.
    return workers_[index]->add_action(std::move(task));
  }

  /**
   * @brief Called on the worker's own thread once it has finished its queue.
   */
  void take_next_task(std::size_t index) {
    std::lock_guard<adaptive_mutex> lock(mutex_);

    if (!pending_.empty()) {
      untangle::task_t next = std::move(pending_.front());
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
   * @return The matching names behind a ": " separator, or empty when none match, so either way
   * it appends to a message cleanly.
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

  mutable adaptive_mutex mutex_;
  std::condition_variable_any finished_cv_;

  //! Serialises wait(). Not mutex_, which finished_cv_ has to own while it waits.
  std::mutex wait_mutex_;

  //! Stands in for joining the workers, which are detached and cannot be joined. Holds this pool's
  //! workers and nothing else, so the destructor waits for those and no others.
  async::execution_poll poll_;

  //! Tasks waiting for a worker, each bound to its arguments, in the order added.
  std::deque<untangle::task_t> pending_;
  std::vector<std::shared_ptr<worker_execution>> workers_;
  //! Whether the pool accepts work. False until start(), false from stop(), and false for as long
  //! as wait() is draining, which puts back whatever it found.
  std::atomic_bool running_ = {false};
};

}  // namespace untangle

#endif  // UNTANGLE_EXECUTOR_EXECUTOR_HPP
