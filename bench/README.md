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
2026-10-09, in µs, with the executor at `e9ef8ea` (the pool's lock spins before it blocks, fix plan
step 28). Three full runs agreed except for the empty 1000-task batches, whose times vary by up to
1.5x between runs. This is the third run.

| workers | tasks | task µs | pool         |  submitted |  processed |  delivered | per task |
|--------:|------:|--------:|--------------|-----------:|-----------:|-----------:|---------:|
|       1 |     1 |     0.0 | executor     |        0.7 |        1.8 |        3.2 |     3.17 |
|       1 |     1 |     0.0 | QThreadPool  |        0.8 |        1.9 |        4.8 |     4.79 |
|       1 |     1 |     0.0 | QtConcurrent |        1.3 |        1.9 |        3.1 |     3.08 |
|       1 |  1000 |     0.0 | executor     |       60.9 |      348.5 |      351.0 |     0.35 |
|       1 |  1000 |     0.0 | QThreadPool  |       68.2 |      249.8 |      260.3 |     0.26 |
|       1 |  1000 |     0.0 | QtConcurrent |      521.6 |      528.7 |      963.5 |     0.96 |
|       4 |     1 |     0.0 | executor     |        0.7 |        3.4 |        7.4 |     7.38 |
|       4 |     1 |     0.0 | QThreadPool  |        0.7 |        3.5 |        7.3 |     7.33 |
|       4 |     1 |     0.0 | QtConcurrent |        1.2 |        4.0 |        8.0 |     8.00 |
|       4 |  1000 |     0.0 | executor     |      148.5 |      411.4 |      435.1 |     0.44 |
|       4 |  1000 |     0.0 | QThreadPool  |       69.0 |      347.4 |      383.4 |     0.38 |
|       4 |  1000 |     0.0 | QtConcurrent |     1760.9 |     1766.5 |     2188.0 |     2.19 |
|       1 |     1 |    10.0 | executor     |        0.7 |       15.5 |       18.2 |    18.25 |
|       1 |     1 |    10.0 | QThreadPool  |        0.6 |       15.9 |       19.4 |    19.38 |
|       1 |     1 |    10.0 | QtConcurrent |        1.3 |       16.2 |       20.1 |    20.12 |
|       1 |  1000 |    10.0 | executor     |       18.8 |    10783.3 |    10790.8 |    10.79 |
|       1 |  1000 |    10.0 | QThreadPool  |       16.2 |    10704.0 |    10709.3 |    10.71 |
|       1 |  1000 |    10.0 | QtConcurrent |      248.3 |    10910.0 |    10915.7 |    10.92 |
|       4 |     1 |    10.0 | executor     |        0.6 |       15.2 |       17.7 |    17.67 |
|       4 |     1 |    10.0 | QThreadPool  |        0.5 |       14.4 |       17.5 |    17.54 |
|       4 |     1 |    10.0 | QtConcurrent |        1.0 |       14.3 |       17.9 |    17.88 |
|       4 |  1000 |    10.0 | executor     |       37.3 |     3124.0 |     3127.0 |     3.13 |
|       4 |  1000 |    10.0 | QThreadPool  |       30.8 |     3089.8 |     3093.7 |     3.09 |
|       4 |  1000 |    10.0 | QtConcurrent |      698.2 |     3667.1 |     3679.6 |     3.68 |

- **With real work, the executor and `QThreadPool` are equal** (within 1%). Both are bound by the
  work. QtConcurrent is 2–19% slower, which is the cost of its futures.
- **A single task makes the round trip in 3–8 µs on every pool:** waking a worker plus one event-loop
  hop. With 10 µs of work it takes 17–20 µs.
- **On empty tasks, where only overhead counts, `QThreadPool` is still ahead overall.** It takes
  0.26–0.38 µs per task against the executor's 0.35–0.44.
  - **Submitting is no longer where the gap is.** With 1 worker, flux's default, the executor submits
    1000 tasks in 61 µs against `QThreadPool`'s 68. With 4 workers it takes 149 µs against 69.
  - **What is left is on the worker side:** with 1 worker the last task finishes at 349 µs against
    250.
- **QtConcurrent is the slowest to submit by far** (0.5–1.8 ms per 1000 tasks), because it builds a
  future and a continuation per task.
- **Four workers run 1000 × 10 µs in 3.1 ms**, 3.5x one worker's 10.8 ms.
- **Delivery adds little:** with 1000 tasks the last result reaches the main thread 2–24 µs after the
  last task finished, except with QtConcurrent, whose continuations add 0.4–0.5 ms.

### Before step 28

The first two runs, with the executor at `f844e17`, differed only in the empty 1000-task batches.
There the executor's `add_task` kept the main thread busy 1.5–5x longer than `QThreadPool`'s
`start`, longest with 4 workers. The profile showed the main thread and the workers handing the
pool's `std::mutex` back and forth through the kernel. Medians of the `submitted` column, all runs:

| 1000 empty tasks, `submitted` µs | executor before | executor after | QThreadPool |
|---|---|---|---|
| 1 worker | 467.9, 114.2 | 85.5, 59.3, 60.9 | 68.2–99.9 |
| 4 workers | 524.1, 473.3 | 205.5, 185.2, 148.5 | 69.0–87.8 |

### A trap found on the way

`QFuture::then(context, ...)` runs the continuation **at once, on the calling thread**, when the
future has already finished by the time `.then()` is attached. So a fast task can deliver its result
while the main thread is still submitting. Code that starts an event loop to wait for the last
result, as this benchmark first did, then calls `quit()` before `exec()` and hangs. The harness now
enters the loop only while results are missing.

## Not covered

One hop per batch instead of per result, results delivered to a real UI (painting), several pools at
once (one per store), and memory.
