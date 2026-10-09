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
2026-10-09, in µs, with the executor at `d1245d7`: each worker keeps its own queue (fix plan step 31),
on async `11aa5a8`, whose workers spin briefly before they park or block. Two full runs agreed except
for the empty 1000-task batches, which vary by up to 1.5x between runs, `QThreadPool`'s as much as
the executor's. This is the second run.

| workers | tasks | task µs | pool         |  submitted |  processed |  delivered | per task |
|--------:|------:|--------:|--------------|-----------:|-----------:|-----------:|---------:|
|       1 |     1 |     0.0 | executor     |        0.6 |        3.3 |        7.1 |     7.12 |
|       1 |     1 |     0.0 | QThreadPool  |        0.7 |        3.5 |        7.5 |     7.50 |
|       1 |     1 |     0.0 | QtConcurrent |        1.3 |        4.1 |        8.5 |     8.46 |
|       1 |  1000 |     0.0 | executor     |       50.2 |      230.2 |      245.2 |     0.25 |
|       1 |  1000 |     0.0 | QThreadPool  |       74.4 |      299.9 |      315.2 |     0.32 |
|       1 |  1000 |     0.0 | QtConcurrent |      531.6 |      538.8 |     1019.2 |     1.02 |
|       4 |     1 |     0.0 | executor     |        0.6 |        2.7 |        6.9 |     6.88 |
|       4 |     1 |     0.0 | QThreadPool  |        0.6 |        3.5 |        7.9 |     7.88 |
|       4 |     1 |     0.0 | QtConcurrent |        1.3 |        4.4 |        7.8 |     7.83 |
|       4 |  1000 |     0.0 | executor     |       41.5 |      341.5 |      365.0 |     0.36 |
|       4 |  1000 |     0.0 | QThreadPool  |       82.1 |      375.8 |      410.1 |     0.41 |
|       4 |  1000 |     0.0 | QtConcurrent |     1887.1 |     1882.7 |     2302.0 |     2.30 |
|       1 |     1 |    10.0 | executor     |        0.6 |       16.5 |       20.2 |    20.21 |
|       1 |     1 |    10.0 | QThreadPool  |        0.7 |       15.4 |       18.1 |    18.12 |
|       1 |     1 |    10.0 | QtConcurrent |        0.9 |       16.1 |       19.8 |    19.79 |
|       1 |  1000 |    10.0 | executor     |       24.5 |    10834.8 |    10839.9 |    10.84 |
|       1 |  1000 |    10.0 | QThreadPool  |       21.7 |    10867.5 |    10870.5 |    10.87 |
|       1 |  1000 |    10.0 | QtConcurrent |      239.4 |    11028.1 |    11034.1 |    11.03 |
|       4 |     1 |    10.0 | executor     |        0.5 |       16.0 |       19.0 |    19.00 |
|       4 |     1 |    10.0 | QThreadPool  |        0.6 |       15.1 |       18.4 |    18.42 |
|       4 |     1 |    10.0 | QtConcurrent |        1.0 |       15.2 |       18.9 |    18.88 |
|       4 |  1000 |    10.0 | executor     |       43.8 |     3141.1 |     3146.6 |     3.15 |
|       4 |  1000 |    10.0 | QThreadPool  |       31.2 |     3196.3 |     3198.0 |     3.20 |
|       4 |  1000 |    10.0 | QtConcurrent |      880.9 |     3949.3 |     3967.1 |     3.97 |

- **With real work, the executor and `QThreadPool` are equal** (within 2%). Both are bound by the
  work. QtConcurrent is 2–25% slower, which is the cost of its futures.
- **A single task makes the round trip in 7–9 µs on every pool:** waking a worker plus one event-loop
  hop. With 10 µs of work it takes 18–20 µs; there the executor is up to 2 µs behind `QThreadPool`,
  the price of its worker spinning briefly before it parks, paid once per wake-up.
- **On empty tasks, where only overhead counts, the executor is now ahead.** It takes 0.25–0.36 µs
  per task against `QThreadPool`'s 0.32–0.41, and keeps the main thread busy for less: 1000 tasks
  are submitted in 50 µs against 74 with 1 worker, and in 42 against 82 with 4.
- **QtConcurrent is the slowest to submit by far** (0.5–1.9 ms per 1000 tasks), because it builds a
  future and a continuation per task.
- **Four workers run 1000 × 10 µs in 3.1 ms**, 3.5x one worker's 10.8 ms.
- **Delivery adds little:** with 1000 tasks the last result reaches the main thread 2–34 µs after the
  last task finished, except with QtConcurrent, whose continuations add 0.4–0.5 ms.

### How the executor got here

The first runs put the executor behind `QThreadPool` on empty batches, by up to 5x in submitting.
Three changes closed it, each found by profiling this benchmark:

1. **Step 28** (`e9ef8ea`): the pool's lock spins before it blocks. The main thread and the workers
   had been handing a `std::mutex` back and forth through the kernel.
2. **Async steps 51 and 52** (`11aa5a8`): an idle worker spins briefly before it parks and is
   notified only when parked, its queue lock spins before it blocks, and an execution can say how
   much work waits in it.
3. **Step 31** (`d1245d7`): no shared queue in the pool. A task goes to a worker at submission and is
   sealed once, by `execution::add_task()`, instead of waiting in the pool and being handed over one
   at a time, wrapped a second time.

1000 empty tasks, medians of each run, µs:

| | before (`f844e17`) | step 28 (`e9ef8ea`) | step 31 (`d1245d7`) | QThreadPool, same runs |
|---|---|---|---|---|
| 1 worker, `submitted` | 468, 114 | 86, 59, 61 | **72, 50** | 68–137 |
| 1 worker, `delivered` | 571, 397 | 543, 406, 351 | **363, 245** | 260–502 |
| 4 workers, `submitted` | 524, 473 | 206, 185, 149 | **42, 42** | 69–91 |
| 4 workers, `delivered` | 668, 604 | 487, 480, 435 | **405, 365** | 383–450 |

### A trap found on the way

`QFuture::then(context, ...)` runs the continuation **at once, on the calling thread**, when the
future has already finished by the time `.then()` is attached. So a fast task can deliver its result
while the main thread is still submitting. Code that starts an event loop to wait for the last
result, as this benchmark first did, then calls `quit()` before `exec()` and hangs. The harness now
enters the loop only while results are missing.

## Not covered

One hop per batch instead of per result, results delivered to a real UI (painting), several pools at
once (one per store), and memory.
