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
2026-10-10, in µs, with the executor at `5c40e91`: each worker keeps its own queue (fix plan step 31)
and pools name their workers (step 13), on async `e0878eb` and actuator `fccaad1`, whose workers park
as soon as their queue is empty (async step 53). Three runs on that code agreed except for the empty
1000-task batches, which vary by up to 1.4x between runs, `QThreadPool`'s as much as the executor's,
and single tasks, where a run can be 30–65% slow on every pool at once. This is the second. The
1000 × 10 µs batches ran 10–15% faster than on 2026-10-09 on every pool, which is the machine, not
the code.

| workers | tasks | task µs | pool         |  submitted |  processed |  delivered | per task |
|--------:|------:|--------:|--------------|-----------:|-----------:|-----------:|---------:|
|       1 |     1 |     0.0 | executor     |        0.7 |        3.4 |        7.4 |     7.42 |
|       1 |     1 |     0.0 | QThreadPool  |        0.7 |        3.5 |        7.4 |     7.42 |
|       1 |     1 |     0.0 | QtConcurrent |        1.3 |        4.1 |        7.7 |     7.67 |
|       1 |  1000 |     0.0 | executor     |       37.3 |      160.5 |      174.8 |     0.17 |
|       1 |  1000 |     0.0 | QThreadPool  |       65.2 |      307.3 |      315.9 |     0.32 |
|       1 |  1000 |     0.0 | QtConcurrent |      534.3 |      536.3 |      956.1 |     0.96 |
|       4 |     1 |     0.0 | executor     |        0.7 |        3.5 |        7.8 |     7.75 |
|       4 |     1 |     0.0 | QThreadPool  |        0.6 |        3.4 |        7.3 |     7.29 |
|       4 |     1 |     0.0 | QtConcurrent |        1.1 |        3.8 |        8.5 |     8.46 |
|       4 |  1000 |     0.0 | executor     |       39.9 |      290.8 |      314.8 |     0.31 |
|       4 |  1000 |     0.0 | QThreadPool  |       64.6 |      385.8 |      427.3 |     0.43 |
|       4 |  1000 |     0.0 | QtConcurrent |     1772.6 |     1778.6 |     2184.6 |     2.18 |
|       1 |     1 |    10.0 | executor     |        0.7 |       15.3 |       18.7 |    18.67 |
|       1 |     1 |    10.0 | QThreadPool  |        0.7 |       14.9 |       17.9 |    17.88 |
|       1 |     1 |    10.0 | QtConcurrent |        1.2 |       15.6 |       19.1 |    19.12 |
|       1 |  1000 |    10.0 | executor     |       26.7 |    10385.5 |    10390.8 |    10.39 |
|       1 |  1000 |    10.0 | QThreadPool  |       20.6 |    10452.0 |    10456.2 |    10.46 |
|       1 |  1000 |    10.0 | QtConcurrent |      258.1 |    10612.7 |    10616.1 |    10.62 |
|       4 |     1 |    10.0 | executor     |        0.7 |       15.4 |       18.0 |    18.04 |
|       4 |     1 |    10.0 | QThreadPool  |        0.5 |       15.5 |       18.9 |    18.88 |
|       4 |     1 |    10.0 | QtConcurrent |        1.1 |       15.7 |       19.5 |    19.46 |
|       4 |  1000 |    10.0 | executor     |       44.2 |     3158.2 |     3165.0 |     3.17 |
|       4 |  1000 |    10.0 | QThreadPool  |       57.8 |     3263.4 |     3274.6 |     3.27 |
|       4 |  1000 |    10.0 | QtConcurrent |     1232.2 |     3934.8 |     3946.3 |     3.95 |

- **With real work, the executor and `QThreadPool` are equal.** Both are bound by the work.
  QtConcurrent is 2–21% slower, which is the cost of its futures.
- **A single task makes the round trip in 3–9 µs on every pool:** waking a worker plus one event-loop
  hop. With 10 µs of work it takes 18–19 µs on both the executor and `QThreadPool`.
- **On empty tasks, where only overhead counts, the executor is ahead.** It takes 0.17–0.31 µs per
  task against `QThreadPool`'s 0.32–0.43, and keeps the main thread busy for less: 1000 tasks are
  submitted in 37 µs against 65 with 1 worker, and in 40 against 65 with 4.
- **With real work, `QThreadPool` usually submits faster:** 21–33 µs per 1000 tasks against the
  executor's 27–43, in five of the six cases across the three runs. The sixth is this run's 4 workers,
  58 against 44. Delivery is unaffected.
- **QtConcurrent is the slowest to submit by far** (0.3–1.8 ms per 1000 tasks), because it builds a
  future and a continuation per task.
- **Four workers run 1000 × 10 µs in 3.2 ms**, 3.3x one worker's 10.4 ms.
- **Delivery adds little:** with 1000 tasks the last result reaches the main thread 4–42 µs after the
  last task finished, except with QtConcurrent, whose continuations add 0.41 ms on empty tasks.

### How the executor got here

The first runs put the executor behind `QThreadPool` on empty batches, by up to 5x in submitting.
Four changes closed it, each found with this benchmark:

1. **Step 28** (`e9ef8ea`): the pool's lock spins before it blocks. The main thread and the workers
   had been handing a `std::mutex` back and forth through the kernel.
2. **Async steps 51 and 52** (`11aa5a8`): an idle worker spins briefly before it parks and is
   notified only when parked, its queue lock spins before it blocks, and an execution can say how
   much work waits in it.
3. **Step 31** (`d1245d7`): no shared queue in the pool. A task goes to a worker at submission and is
   sealed once, by `execution::add_task()`, instead of waiting in the pool and being handed over one
   at a time, wrapped a second time.
4. **Async step 53** (`be5986c`, pinned by `c3dc143`): the worker's spin before parking, from step
   51, is gone. It cost a single task with work ~2 µs - one task of 10 µs arrived in 19.5–20.5 µs
   against `QThreadPool`'s 17.5–18.5 - and the batches here never reached it. Now 14.4–18.1 against
   14.5–18.2.

1000 empty tasks, medians of each run, µs:

| | before (`f844e17`) | step 28 (`e9ef8ea`) | step 31 (`d1245d7`, `6cc4350`) | async step 53 (`c3dc143`) | now (`5c40e91`) | QThreadPool, same runs |
|---|---|---|---|---|---|---|
| 1 worker, `submitted` | 468, 114 | 86, 59, 61 | 72, 50, 50 | 49, 39, 40 | **38, 37, 43** | 63–137 |
| 1 worker, `delivered` | 571, 397 | 543, 406, 351 | 363, 245, 175 | 174, 191, 174 | **170, 175, 193** | 252–502 |
| 4 workers, `submitted` | 524, 473 | 206, 185, 149 | 42, 42, 40 | 41, 38, 42 | **40, 40, 43** | 65–99 |
| 4 workers, `delivered` | 668, 604 | 487, 480, 435 | 405, 365, 335 | 494, 411, 362 | **318, 315, 411** | 383–614 |

### A trap found on the way

`QFuture::then(context, ...)` runs the continuation **at once, on the calling thread**, when the
future has already finished by the time `.then()` is attached. So a fast task can deliver its result
while the main thread is still submitting. Code that starts an event loop to wait for the last
result, as this benchmark first did, then calls `quit()` before `exec()` and hangs. The harness now
enters the loop only while results are missing.

## Not covered

One hop per batch instead of per result, results delivered to a real UI (painting), several pools at
once (one per store), and memory.
