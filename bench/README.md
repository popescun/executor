# qt_pool_vs_this — the executor against Qt's thread pool

The main thread submits a batch of tasks to a pool. The pool runs them, and every result is moved to
the main thread, as a UI update would be. That is the path of a flux store's answer to its presenter.

```sh
cmake -S bench -B bench/build -G Ninja     # Release unless told otherwise; -DQT_PATH=<Qt>/<ver>/<platform>
cmake --build bench/build
bench/build/qt_pool_vs_this [--reps N]
```

It prints a markdown table and exits non-zero if any check failed.

## What is compared

| pool | submit | result to the main thread |
|---|---|---|
| **executor** | `add_task(task, i, callback)` | the callback runs on the worker and posts with `QMetaObject::invokeMethod(context, ..., Qt::QueuedConnection)`, as the prototypes' presenters do |
| **QThreadPool** | `start(lambda)` | the lambda runs the task and posts the same way |
| **QtConcurrent** | `QtConcurrent::run(&pool, task, i)` | `.then(context, ...)`, Qt's own idiom, which runs the continuation on the context object's thread |

- **Same pools, same task:** every pool is its own instance with the same worker count, so there is
  no global pool. Each runs the same `std::function<int(int)>` task, which spins a linear
  congruential generator for the nominal time. "0 µs" means an empty task, which measures overhead
  alone.
- **Cores:** each worker asks once for a performance core, as does the main thread. Otherwise macOS
  could place one pool's threads on efficiency cores and not the other's.
- **Cases:** 1 or 4 workers; batches of 1 task (latency) or 1000 (throughput); 21 runs per case, or
  210 for a single task. The pools take turns, after an untimed run each.
- **What is recorded:** each run records three times from the first submit, as medians:
  - **submitted:** when the main thread finished submitting, which is how long the UI thread was
    busy;
  - **processed:** when the last task finished on a worker;
  - **delivered:** when the last result arrived on the main thread.
- **Checks:** the results are summed on the main thread and checked. A run that has not delivered
  everything after 10 s fails.

## Results

Apple M4 (4 performance and 6 efficiency cores), macOS 26.6.2, Apple clang 17, Qt 6.11.2, Release,
2026-10-09, in µs. Two full runs agreed except for the empty 1000-task batches, whose submit times
vary by up to 4x between runs. This is the second run.

| workers | tasks | task µs | pool         |  submitted |  processed |  delivered | per task |
|--------:|------:|--------:|--------------|-----------:|-----------:|-----------:|---------:|
|       1 |     1 |     0.0 | executor     |        0.6 |        3.1 |        6.9 |     6.88 |
|       1 |     1 |     0.0 | QThreadPool  |        0.6 |        3.2 |        6.8 |     6.83 |
|       1 |     1 |     0.0 | QtConcurrent |        1.2 |        3.7 |        7.2 |     7.25 |
|       1 |  1000 |     0.0 | executor     |      114.2 |      386.1 |      396.7 |     0.40 |
|       1 |  1000 |     0.0 | QThreadPool  |       74.0 |      264.6 |      277.5 |     0.28 |
|       1 |  1000 |     0.0 | QtConcurrent |      567.8 |      595.7 |     1049.5 |     1.05 |
|       4 |     1 |     0.0 | executor     |        0.7 |        3.2 |        7.2 |     7.25 |
|       4 |     1 |     0.0 | QThreadPool  |        0.7 |        3.6 |        7.5 |     7.50 |
|       4 |     1 |     0.0 | QtConcurrent |        1.3 |        4.2 |        7.6 |     7.58 |
|       4 |  1000 |     0.0 | executor     |      473.3 |      582.2 |      604.4 |     0.60 |
|       4 |  1000 |     0.0 | QThreadPool  |       87.8 |      382.0 |      412.6 |     0.41 |
|       4 |  1000 |     0.0 | QtConcurrent |     1796.4 |     1798.7 |     2251.1 |     2.25 |
|       1 |     1 |    10.0 | executor     |        0.6 |       15.5 |       18.1 |    18.12 |
|       1 |     1 |    10.0 | QThreadPool  |        0.5 |       16.0 |       19.3 |    19.29 |
|       1 |     1 |    10.0 | QtConcurrent |        1.2 |       16.2 |       20.0 |    19.96 |
|       1 |  1000 |    10.0 | executor     |       23.2 |    11063.7 |    11066.8 |    11.07 |
|       1 |  1000 |    10.0 | QThreadPool  |       18.3 |    11031.0 |    11033.9 |    11.03 |
|       1 |  1000 |    10.0 | QtConcurrent |      270.1 |    11208.6 |    11214.5 |    11.21 |
|       4 |     1 |    10.0 | executor     |        0.5 |       14.9 |       17.4 |    17.42 |
|       4 |     1 |    10.0 | QThreadPool  |        0.6 |       14.1 |       17.3 |    17.33 |
|       4 |     1 |    10.0 | QtConcurrent |        1.1 |       14.0 |       17.6 |    17.58 |
|       4 |  1000 |    10.0 | executor     |       34.8 |     3165.5 |     3169.2 |     3.17 |
|       4 |  1000 |    10.0 | QThreadPool  |       29.5 |     3081.4 |     3084.5 |     3.08 |
|       4 |  1000 |    10.0 | QtConcurrent |       627.1 |     3623.0 |     3647.2 |     3.65 |

- **With real work, the executor and `QThreadPool` are equal** (within 3%). Both are bound by the work.
  QtConcurrent is 2–15% slower, which is the cost of its futures.
- **A single task makes the round trip in 7 µs on every pool:** waking a worker plus one event-loop
  hop. With 10 µs of work it takes 17–20 µs.
- **On empty tasks, where only overhead counts, `QThreadPool` is ahead.** It takes 0.28–0.41 µs per
  task against the executor's 0.40–0.60. The difference is mostly in submitting:
  - The executor's `add_task` keeps the main thread busy 1.5–5x longer, and longest with 4 workers,
    whose `on_finished` takes the same mutex the main thread pushes under.
  - This is the one place where the executor costs the UI thread something: about 0.5 µs per task at
    worst. It is the place to look if the executor is ever tuned.
- **QtConcurrent is the slowest to submit by far** (0.6–1.8 ms per 1000 tasks), because it builds a
  future and a continuation per task.
- **Four workers run 1000 × 10 µs in 3.1 ms**, 3.5x one worker's 11 ms.
- **Delivery adds little:** with 1000 tasks the last result reaches the main thread 3–30 µs after the
  last task finished, except with QtConcurrent, whose continuations add 0.5 ms.

### A trap found on the way

`QFuture::then(context, ...)` runs the continuation **at once, on the calling thread**, when the
future has already finished by the time `.then()` is attached. So a fast task can deliver its result
while the main thread is still submitting. Code that starts an event loop to wait for the last
result, as this benchmark first did, then calls `quit()` before `exec()` and hangs. The harness now
enters the loop only while results are missing.

## Not covered

One hop per batch instead of per result, results delivered to a real UI (painting), several pools at
once (one per store), and memory.
