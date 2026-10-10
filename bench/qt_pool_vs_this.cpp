// Copyright (c) 2026 Nicolae Popescu. MIT License.

/**
 * @file qt_pool_vs_this.cpp
 *
 * @brief A batch of tasks on untangle::executor against Qt's thread pool, results delivered to the
 * main thread.
 *
 * The main thread submits a batch, the pool runs it, and every result is moved to the main thread
 * the way a UI update is - which is what a flux store and presenter do with each answer. Three
 * pools take turns:
 *  - **executor:** `add_task(task, i, callback)`; the callback runs on the worker and posts the
 *    result with `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`, as the prototypes'
 *    presenters do;
 *  - **QThreadPool:** `start(lambda)`; the lambda runs the task and posts the result the same way;
 *  - **QtConcurrent:** `QtConcurrent::run(&pool, task, i).then(context, ...)` - Qt's own idiom,
 *    whose continuation runs on the context object's thread.
 *
 * Each pool is its own instance with the same worker count. Every run records three times from the
 * first submit: when the main thread finished submitting, when the last task finished on a worker,
 * and when the last result arrived on the main thread. The results are summed on the main thread
 * and checked.
 *
 * Usage: `qt_pool_vs_this [--reps N]` - N timed runs per case (default 21; batches of one task run
 * ten times as many).
 */
#include <QCoreApplication>
#include <QEventLoop>
#include <QFuture>
#include <QObject>
#include <QThreadPool>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <executor.hpp>
#include <functional>
#include <random>
#include <string>
#include <vector>

#ifdef __APPLE__
#include <pthread/qos.h>
#endif

namespace bench {

using clock_t_ = std::chrono::steady_clock;

/**
 * @brief Asks for a performance core, once per thread.
 *
 * Called from the task itself, so the workers of every pool ask alike - left alone, macOS may place
 * one pool's threads on efficiency cores and not the other's.
 */
void raise_thread() {
#ifdef __APPLE__
  thread_local const bool raised =
      pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) == 0;
  (void)raised;
#endif
}

//! The work of one task: @p iterations steps of a linear congruential generator.
int compute(int seed, std::uint32_t iterations) {
  auto x = static_cast<std::uint32_t>(seed);
  for (std::uint32_t k = 0; k < iterations; ++k) {
    x = x * 1664525u + 1013904223u;
  }
  return static_cast<int>(x >> 8);
}

/**
 * @brief Counts tasks finished on the workers, and records when the last one did.
 */
struct worker_side {
  std::atomic<std::size_t> finished{0};
  std::atomic<std::int64_t> last_at{0};  //!< clock_t_ ticks since its epoch.
  std::size_t target = 0;

  void reset(std::size_t tasks) {
    finished = 0;
    last_at = 0;
    target = tasks;
  }

  void task_finished() {
    if (finished.fetch_add(1) + 1 == target) {
      last_at = clock_t_::now().time_since_epoch().count();
    }
  }
};

/**
 * @brief Takes the results on the main thread, and ends the run's event loop with the last one.
 */
struct main_side {
  std::size_t delivered = 0;
  std::size_t target = 0;
  std::int64_t sum = 0;
  clock_t_::time_point last_at;
  QEventLoop* loop = nullptr;

  void reset(std::size_t tasks, QEventLoop* run_loop) {
    delivered = 0;
    target = tasks;
    sum = 0;
    loop = run_loop;
  }

  //! Can run before the loop does: QtConcurrent calls a continuation at once when its task has
  //! already finished, so the last result may arrive while the batch is still being submitted.
  void take(int result) {
    sum += result;
    if (++delivered == target) {
      last_at = clock_t_::now();
      loop->quit();
    }
  }
};

//! A run's three times, in microseconds from the first submit.
struct sample {
  double submitted = 0;
  double processed = 0;
  double delivered = 0;
};

using task_t = std::function<int(int)>;

/**
 * @brief What every pool shares: the task, the two sides that count, and the main thread's context
 * object the results are posted to.
 */
struct shared {
  QObject context;
  worker_side workers;
  main_side main;
  std::vector<std::uint32_t> iterations;  //!< Per task.
  task_t task;

  explicit shared(std::vector<std::uint32_t> per_task)
      : iterations(std::move(per_task)), task([this](int i) {
          raise_thread();
          const int result = compute(i, iterations[static_cast<std::size_t>(i)]);
          workers.task_finished();
          return result;
        }) {}

  //! Posts @p result to the main thread, as a presenter posts an answer.
  void post(int result) {
    QMetaObject::invokeMethod(
        &context, [this, result] { main.take(result); }, Qt::QueuedConnection);
  }
};

struct executor_pool {
  static constexpr const char* name = "executor";

  executor_pool(shared& s, std::size_t worker_count) : common(s), pool(worker_count) {
    pool.start();
  }

  bool submit(int i) {
    return pool.add_task(common.task, i, [this](int result) { common.post(result); });
  }

  shared& common;
  untangle::executor<task_t> pool;
};

struct qthreadpool_pool {
  static constexpr const char* name = "QThreadPool";

  qthreadpool_pool(shared& s, std::size_t worker_count) : common(s) {
    pool.setMaxThreadCount(static_cast<int>(worker_count));
  }

  bool submit(int i) {
    pool.start([this, i] { common.post(common.task(i)); });
    return true;
  }

  shared& common;
  QThreadPool pool;
};

struct qtconcurrent_pool {
  static constexpr const char* name = "QtConcurrent";

  qtconcurrent_pool(shared& s, std::size_t worker_count) : common(s) {
    pool.setMaxThreadCount(static_cast<int>(worker_count));
  }

  bool submit(int i) {
    // The continuation runs on the context object's thread: the main thread.
    QtConcurrent::run(&pool, common.task, i).then(&common.context, [this](int result) {
      common.main.take(result);
    });
    return true;
  }

  shared& common;
  QThreadPool pool;
};

//! How long a run may take before it counts as hung.
constexpr std::chrono::milliseconds run_timeout{10'000};

double micros(clock_t_::duration d) { return std::chrono::duration<double, std::micro>(d).count(); }

/**
 * @brief Submits @p tasks tasks, runs the main thread's event loop until the last result is in,
 * and returns the run's times. False when a task was refused, the results took longer than
 * @ref run_timeout, or their sum is wrong.
 */
template <typename pool_t>
bool run_batch(pool_t& pool, shared& common, std::size_t tasks, std::int64_t expected_sum,
               sample& out) {
  QEventLoop loop;
  common.workers.reset(tasks);
  common.main.reset(tasks, &loop);

  const auto start = clock_t_::now();
  for (std::size_t i = 0; i < tasks; ++i) {
    if (!pool.submit(static_cast<int>(i))) {
      std::fprintf(stderr, "%s refused task %zu\n", pool_t::name, i);
      return false;
    }
  }
  const auto submitted = clock_t_::now();

  // The results are posted events, so they arrive only while a loop runs - unless all of them
  // arrived during the submit, and a quit() before exec() would be lost.
  if (common.main.delivered < tasks) {
    QTimer::singleShot(run_timeout, &loop, &QEventLoop::quit);
    loop.exec();
  }
  if (common.main.delivered < tasks) {
    std::fprintf(stderr, "%s: %zu of %zu results after %lld ms\n", pool_t::name,
                 common.main.delivered, tasks, static_cast<long long>(run_timeout.count()));
    return false;
  }

  const clock_t_::time_point processed{clock_t_::duration{common.workers.last_at.load()}};
  out = {micros(submitted - start), micros(processed - start), micros(common.main.last_at - start)};

  if (common.main.sum != expected_sum) {
    std::fprintf(stderr, "%s: results add up to %lld, expected %lld\n", pool_t::name,
                 static_cast<long long>(common.main.sum), static_cast<long long>(expected_sum));
    return false;
  }
  return true;
}

double median(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}

struct series {
  std::vector<double> submitted, processed, delivered;

  void add(const sample& s) {
    submitted.push_back(s.submitted);
    processed.push_back(s.processed);
    delivered.push_back(s.delivered);
  }
};

void print_row(const char* pool_name, std::size_t worker_count, std::size_t tasks, const char* work,
               const series& s) {
  const double delivered = median(s.delivered);
  std::printf("| %7zu | %5zu | %7s | %-12s | %10.1f | %10.1f | %10.1f | %8.2f |\n", worker_count,
              tasks, work, pool_name, median(s.submitted), median(s.processed), delivered,
              delivered / static_cast<double>(tasks));
  std::fflush(stdout);
}

/**
 * @brief One case on all three pools: they take turns within each repetition, after one untimed
 * run each, which also starts their threads. @p iterations holds one task's work per task, and
 * @p work names it in the table.
 */
bool run_case(std::size_t worker_count, const char* work, const std::vector<std::uint32_t>& iterations,
              std::size_t reps) {
  const std::size_t tasks = iterations.size();
  shared common(iterations);
  executor_pool executor(common, worker_count);
  qthreadpool_pool qthreadpool(common, worker_count);
  qtconcurrent_pool qtconcurrent(common, worker_count);

  std::int64_t expected_sum = 0;
  for (std::size_t i = 0; i < tasks; ++i) {
    expected_sum += compute(static_cast<int>(i), iterations[i]);
  }

  sample one;
  bool ok = run_batch(executor, common, tasks, expected_sum, one) &&
            run_batch(qthreadpool, common, tasks, expected_sum, one) &&
            run_batch(qtconcurrent, common, tasks, expected_sum, one);

  series executor_times, qthreadpool_times, qtconcurrent_times;
  for (std::size_t rep = 0; ok && rep < reps; ++rep) {
    ok = run_batch(executor, common, tasks, expected_sum, one);
    executor_times.add(one);
    ok = ok && run_batch(qthreadpool, common, tasks, expected_sum, one);
    qthreadpool_times.add(one);
    ok = ok && run_batch(qtconcurrent, common, tasks, expected_sum, one);
    qtconcurrent_times.add(one);
  }
  if (!ok) {
    return false;
  }

  print_row(executor_pool::name, worker_count, tasks, work, executor_times);
  print_row(qthreadpool_pool::name, worker_count, tasks, work, qthreadpool_times);
  print_row(qtconcurrent_pool::name, worker_count, tasks, work, qtconcurrent_times);
  return true;
}

//! How many generator steps make @p micros_wanted microseconds of work on this machine.
std::uint32_t iterations_for(double micros_wanted) {
  if (micros_wanted <= 0) {
    return 0;
  }
  // The fastest of a few probes, so a cold first one does not stretch the work.
  constexpr std::uint32_t probe = 10'000'000;
  double per_iteration = 0;
  for (int attempt = 0; attempt < 5; ++attempt) {
    const auto start = clock_t_::now();
    volatile int sink = compute(attempt, probe);
    (void)sink;
    const double measured = micros(clock_t_::now() - start) / probe;
    per_iteration = attempt == 0 ? measured : std::min(per_iteration, measured);
  }
  return static_cast<std::uint32_t>(micros_wanted / per_iteration);
}

/**
 * @brief @p tasks task lengths: one in ten @p long_iterations, the rest @p short_iterations.
 *
 * Drawn from a fixed seed, so every pool and every run gets the same batch. Not a fixed stride: the
 * executor spreads a batch nearly round-robin, and a stride matching the worker count would put
 * every long task on one worker.
 */
std::vector<std::uint32_t> mixed_batch(std::size_t tasks, std::uint32_t short_iterations,
                                       std::uint32_t long_iterations) {
  std::mt19937 draw(2026);
  std::vector<std::uint32_t> lengths(tasks);
  for (auto& length : lengths) {
    length = draw() % 10 == 0 ? long_iterations : short_iterations;
  }
  return lengths;
}

}  // namespace bench

int main(int argc, char* argv[]) {
  QCoreApplication app(argc, argv);
  bench::raise_thread();

  std::size_t reps = 21;
  if (argc == 3 && std::strcmp(argv[1], "--reps") == 0 && std::strtoull(argv[2], nullptr, 10) > 0) {
    reps = static_cast<std::size_t>(std::strtoull(argv[2], nullptr, 10));
  } else if (argc != 1) {
    std::fprintf(stderr, "usage: %s [--reps N]\n", argv[0]);
    return 2;
  }

#ifdef NDEBUG
  const char* build = "release";
#else
  const char* build = "debug - not worth measuring";
#endif
  std::printf(
      "Qt %s, %s build; %zu runs per case (x10 for one task); median, us from the first "
      "submit\n\n",
      qVersion(), build, reps);
  std::printf(
      "| %7s | %5s | %7s | %-12s | %10s | %10s | %10s | %8s |\n"
      "|--------:|------:|--------:|--------------|-----------:|-----------:|-----------:|---------"
      ":"
      "|\n",
      "workers", "tasks", "task us", "pool", "submitted", "processed", "delivered", "per task");

  bool ok = true;
  for (const double task_us : {0.0, 10.0}) {
    const std::uint32_t iterations = bench::iterations_for(task_us);
    char work[16];
    std::snprintf(work, sizeof work, "%.1f", task_us);
    for (const std::size_t worker_count : {1, 4}) {
      for (const std::size_t tasks : {1, 1000}) {
        ok = bench::run_case(worker_count, work, std::vector<std::uint32_t>(tasks, iterations),
                             tasks == 1 ? reps * 10 : reps) &&
             ok;
      }
    }
  }

  // Mixed lengths: a pool that fixes a task's worker at submission can leave short tasks waiting
  // behind a long one while another worker idles.
  const std::uint32_t short_iterations = bench::iterations_for(10.0);
  const std::uint32_t long_iterations = bench::iterations_for(1000.0);
  const std::vector<std::uint32_t> mixed =
      bench::mixed_batch(1000, short_iterations, long_iterations);
  for (const std::size_t worker_count : {1, 4}) {
    ok = bench::run_case(worker_count, "mixed", mixed, reps) && ok;
  }
  std::printf("\nmixed: %zu of %zu tasks take 1000 us, the rest 10 us\n",
              static_cast<std::size_t>(std::count(mixed.begin(), mixed.end(), long_iterations)),
              mixed.size());
  return ok ? 0 : 1;
}
