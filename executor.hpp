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

//! Numbers the pools that are not given a name, across every task type, so no two are alike.
inline std::atomic<std::size_t> unnamed_pools = {0};

/**
 * @brief Runs tasks on a fixed pool of untangle::async::execution workers.
 *
 * \ref add_task() is the one door: it carries a callback and reports the result to it.
 *
 * Each worker keeps its own queue. A task goes to a worker when it is submitted - the first idle
 * one, else the one given the fewest since it last drained - and waits there for that worker.
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
   * @param name - What its workers are named after, `<name>_worker_<i>`, in every warning they
   * print. Left empty, it is `pool_<n>`, numbered across the process, so two pools are never alike.
   * @throws std::invalid_argument - \p worker_count is zero: such a pool could never run its work.
   */
  explicit executor(std::size_t worker_count, std::string name = {}) {
    if (worker_count == 0) {
      throw std::invalid_argument("executor: worker_count must not be zero");
    }

    if (name.empty()) {
      name = "pool_" + std::to_string(unnamed_pools.fetch_add(1) + 1);
    }

    workers_.reserve(worker_count);
    given_ = std::make_unique<std::atomic<std::size_t>[]>(worker_count);

    for (std::size_t index = 0; index < worker_count; ++index) {
      auto next = worker_execution::create_instance(name + "_worker_" + std::to_string(index));

      // Tells the pool this worker has run everything it was given.
      next->on_finished = [this, index] { worker_drained(index); };

      // Carries what a task throws to the caller. Assigned here because a worker thread reads it.
      next->on_error = [this, worker = next->name](std::exception_ptr thrown) {
        report_task_error(worker, thrown);
      };

      // Adds the worker to the poll the destructor waits on.
      poll_.add(*next);

      workers_.push_back(std::move(next));
    }
  }

  //! Not copied or moved: every worker's callbacks hold this pool's `this`.
  executor(const executor&) = delete;
  executor& operator=(const executor&) = delete;
  executor(executor&&) = delete;
  executor& operator=(executor&&) = delete;

  /**
   * @brief Drains what was added, then stops the workers and destroys them.
   *
   * The drain is \ref wait(); the shutdown after it stops the workers, waits for each to leave its
   * thread, and destroys them only then. Both waits are unbounded and report on stderr meanwhile.
   *
   * @remark Nothing is dropped: a worker runs what it was given before it leaves its thread, a pool
   * stopped earlier included.
   *
   * @attention This waits for the pool, not for what its work touched: see \ref wait().
   */
  ~executor() {
    // Nothing is stopped before this, or the queue could never empty. wait() leaves the pool
    // accepting, so stop() follows at once: that window is the only one a shutdown would take.
    wait();
    stop();

    // Waits for every worker to leave its thread, which is what makes destroying them safe. A
    // different question from the wait above: a worker can be idle and still be in its thread.
    // A worker leaves within microseconds of stop(), so the first tick is short and doubles up to
    // the ceiling: a fixed tick made every destruction sleep it once.
    auto interval = std::chrono::milliseconds(report_first_ms);
    auto waited = std::chrono::microseconds(0);
    auto tick = std::chrono::microseconds(poll_first_us);

    while (poll_.is_running()) {  // time of check
      std::this_thread::sleep_for(tick);
      waited += tick;
      tick = std::min(tick * 2, std::chrono::microseconds(poll_max_ms * 1000));

      if (waited >= interval) {
        std::println(stderr, "executor: still waiting to stop after {}s - {} not left its thread{}",
                     std::chrono::duration_cast<std::chrono::seconds>(waited).count(),
                     count_workers(&worker_execution::is_running),
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
   * Drained means no worker has anything queued or running, so every task accepted before this has
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
   * @remark A stopped pool is waited for too: its workers run what they were given before they
   * leave, so the wait ends.
   *
   * @attention It waits for the work, not for what the work touched. Held as a member, ~executor()
   * runs once the owner's derived parts and later members are gone: call this before that.
   */
  void wait() {
    // One drain at a time: two threads between the exchange below and the restore at the end would
    // read each other's cleared flag, and the last one out could leave the pool refusing work.
    std::lock_guard<std::mutex> drain(wait_mutex_);

    // Put back exactly as found below: a wait must not leave a stopped pool accepting.
    const bool was_running = running_.exchange(false);

    {
      std::unique_lock<adaptive_mutex> lock(mutex_);

      // Counted under the lock, before the first check: a worker that drains after this sees it and
      // notifies, and one that drained before it is seen by that first check.
      ++waiting_;

      // Waits for every worker to go idle.
      auto interval = std::chrono::milliseconds(report_first_ms);
      auto waited = std::chrono::milliseconds(0);

      while (!finished_cv_.wait_for(lock, interval, [this] { return nothing_running(); })) {
        waited += interval;

        std::println(
            stderr, "executor: still waiting to drain after {}s - {} queued, {} in a task{}",
            waited.count() / 1000, total_pending(), count_workers(&worker_execution::is_busy),
            name_workers(&worker_execution::is_busy));

        interval = std::min(interval * 2, std::chrono::milliseconds(report_max_ms));
      }

      --waiting_;
    }

    running_ = was_running;
  }

  /**
   * @brief Gives work that must report to a worker, which queues it.
   *
   * The first idle worker takes it, the warmest one; otherwise the worker given the fewest since it
   * last drained, and the task waits behind that worker's queue - in the order it arrived there.
   * Once the task has run, its callback is invoked with the result, on the worker's thread.
   *
   * @attention The last argument is the callback, which the signature cannot say: it arrives in
   * \p args and untangle::bind_task() splits it off. A missing or unusable one is a static_assert,
   * and so is a task that cannot be called with the arguments before it.
   *
   * @attention A task that throws is not notified - what it threw goes to \ref on_task_error - and
   * a refused one does not notify either, so a caller waiting on the callback alone waits for ever.
   *
   * @attention An empty callback or action is refused here too, silently: the answer is the report.
   * Only a run time check can see it, and a task that can neither run nor notify is turned away
   * before any worker is asked.
   *
   * @tparam Args - The argument types to bind, of which the last is the callback.
   * @param task - The task to run, copied into the worker's queue.
   * @param args - What to bind, followed by the callback. Copied by the worker's queue, so a task
   * taking a reference gets the pool's copy.
   *
   * @return true - accepted, and the callback will be notified once it has run.
   * @return false - refused by a pool not started, stopped, draining or being destroyed, by a
   * stopped worker, or because the task could never do its job. It will not run or notify.
   */
  template <typename... Args>
  bool add_task(const actionT& task, Args&&... args) {
    // The call the pool will make, which is not quite the one written at the call site: what
    // reaches the task is the copies the worker binds, so that is the call that has to compile.
    // Without this the mismatch surfaces deep inside bind_task()'s std::apply.
    static_assert(invocable_with_bound<Args...>(),
                  "executor: the task cannot be called with the arguments given to add_task() "
                  "before its callback");

    if (!running_ || task_is_empty(task, args...)) {
      return false;
    }

    return give_to_worker(pick_worker(), task, std::forward<Args>(args)...);
  }

  /**
   * @brief Gives work that must report to a worker as the overload above does, moving the task into
   * the queue rather than copying it. A temporary and a lambda land here.
   */
  template <typename... Args>
  bool add_task(actionT&& task, Args&&... args) {
    static_assert(invocable_with_bound<Args...>(),
                  "executor: the task cannot be called with the arguments given to add_task() "
                  "before its callback");

    if (!running_ || task_is_empty(task, args...)) {
      return false;
    }

    return give_to_worker(pick_worker(), std::move(task), std::forward<Args>(args)...);
  }

  /**
   * @brief How many tasks are waiting in the workers' queues, not yet taken to run.
   *
   * @remark Advisory: true the moment it is read.
   */
  std::size_t pending() const { return total_pending(); }

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
   * What the workers were given they still run, then leave; anything added afterwards is refused, a
   * running task included.
   * \ref start() revives the workers; harmless twice over.
   *
   * @attention It does not wait for the workers to leave their threads - only the destructor does,
   * and that wait is what makes destroying the pool safe.
   */
  void stop() {
    // First, so that nothing is accepted for workers that are about to stop. A task already on a
    // worker still runs; one arriving from here is refused.
    running_ = false;

    // No lock: a worker draining after stop() runs on_finished, which may take mutex_.
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

  using worker_execution = untangle::async::execution<actionT>;

  /**
   * @brief The first idle worker, the warmest; else the one given the fewest since it last drained.
   *
   * Read without a lock, so two submitters may pick alike: the counts steer balance only.
   */
  std::size_t pick_worker() const {
    std::size_t best = 0;
    std::size_t best_count = given_[0].load(std::memory_order_relaxed);

    for (std::size_t index = 0; index < workers_.size(); ++index) {
      const std::size_t count = given_[index].load(std::memory_order_relaxed);
      if (count == 0) {
        return index;
      }
      if (count < best_count) {
        best = index;
        best_count = count;
      }
    }

    return best;
  }

  /**
   * @brief Hands a task, with its arguments and callback, to worker @p index, and counts it.
   *
   * The worker binds it, as for any caller of execution::add_task(): the task and its arguments are
   * forwarded once, from add_task() through here.
   *
   * @tparam action_t - The task's type as passed: a reference for one the caller keeps.
   * @return true - the worker queued it. false - the worker is stopped and refused it.
   */
  template <typename action_t, typename... Args>
  bool give_to_worker(std::size_t index, action_t&& task, Args&&... args) {
    // No lock: the counts only steer pick_worker(), so a stale one costs balance, not correctness.
    given_[index].fetch_add(1, std::memory_order_relaxed);

    if (workers_[index]->add_task(std::forward<action_t>(task), std::forward<Args>(args)...)) {
      return true;
    }

    // Only a stopped worker refuses: add_task() turned an empty task away before. Not counted, or
    // an idle worker would look busy until it is next given work and drains.
    forget_given(index);
    return false;
  }

  /**
   * @brief Whether untangle::bind_task() would build an empty task from these: the action or its
   * callback - the last argument - is empty, so it could never run or notify. Read by reference:
   * nothing is copied or moved.
   */
  template <typename... Args>
  static bool task_is_empty(const actionT& task, const Args&... args) {
    if constexpr (testable_for_emptiness<actionT>) {
      if (!task) {
        return true;
      }
    }

    if constexpr (sizeof...(Args) > 0) {
      const auto& callback = std::get<sizeof...(Args) - 1>(std::forward_as_tuple(args...));
      if constexpr (testable_for_emptiness<std::decay_t<decltype(callback)>>) {
        if (!callback) {
          return true;
        }
      }
    }

    return false;
  }

  /**
   * @brief Takes back the count of a task the worker refused, unless a drain has reset it since.
   */
  void forget_given(std::size_t index) {
    std::size_t count = given_[index].load(std::memory_order_relaxed);
    while (count > 0 &&
           !given_[index].compare_exchange_weak(count, count - 1, std::memory_order_relaxed)) {
    }
  }

  /**
   * @brief Called on the worker's own thread once it has run everything it was given.
   */
  void worker_drained(std::size_t index) {
    given_[index].store(0, std::memory_order_relaxed);

    // The lock only for a wait(), which counts itself under it before its first check.
    if (waiting_.load() > 0) {
      std::lock_guard<adaptive_mutex> lock(mutex_);
      finished_cv_.notify_all();
    }
  }

  //! What the workers hold queued, summed.
  std::size_t total_pending() const {
    std::size_t total = 0;
    for (const auto& one : workers_) {
      total += one->pending();
    }
    return total;
  }

  /**
   * @brief Is every worker idle?
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

  //! How long the destructor first sleeps before it asks again whether the workers have left, and
  //! the ceiling that sleep doubles to.
  static constexpr int poll_first_us = 100;
  static constexpr int poll_max_ms = 50;

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

  std::vector<std::shared_ptr<worker_execution>> workers_;

  //! Per worker: tasks given since it last drained. Steers pick_worker() only, so read unlocked.
  std::unique_ptr<std::atomic<std::size_t>[]> given_;

  //! How many wait() calls are waiting. A drained worker takes mutex_ to notify only then.
  std::atomic<std::size_t> waiting_ = {0};
  //! Whether the pool accepts work. False until start(), false from stop(), and false for as long
  //! as wait() is draining, which puts back whatever it found.
  std::atomic_bool running_ = {false};
};

}  // namespace untangle

#endif  // UNTANGLE_EXECUTOR_EXECUTOR_HPP
