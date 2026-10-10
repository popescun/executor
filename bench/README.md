# qt_pool_vs_this — the executor against Qt's thread pool

The main thread submits a batch of tasks to a pool. The pool runs them, and every result is moved to
the main thread, as a UI update would be. That is the path of a flux store's answer to its presenter.

```sh
cmake -S bench -B bench/build -G Ninja     # Release unless told otherwise; -DQT_PATH=<Qt>/<ver>/<platform>
cmake --build bench/build
bench/build/qt_pool_vs_this [--reps N]
```

It prints a markdown table, then how many tasks of the mixed batch are long, and exits non-zero if
any check failed.

## What is compared

| pool | submit | result to the main thread |
|---|---|---|
| **executor** | `add_task(task, i, callback)` | the callback runs on the worker and posts with `QMetaObject::invokeMethod(context, ..., Qt::QueuedConnection)`, coalesced (below) |
| **QThreadPool** | `start(lambda)` | the lambda holds what the executor's sealed task holds - a copy of the task, the argument and a callback - and hands the callback the result, which posts the same way, through the same code |
| **QtConcurrent** | `QtConcurrent::run(&pool, task, i)` | `.then(context, ...)`, Qt's own idiom, which runs the continuation on the context object's thread, one per result |

- **Same pools, same task:** every pool is its own instance with the same worker count, so there is
  no global pool. Each runs the same `std::function<int(int)>` task, which spins a linear
  congruential generator for the nominal time. "0 µs" means an empty task, which measures overhead
  alone.
- **Coalesced delivery:** a result finding others still waiting for the main thread joins them, and
  only the one that finds the list empty posts; the main thread takes the whole list in that one
  event. One wake-up of the main thread per burst rather than per result - what a presenter
  answering many tasks would do. A lone result still posts at once. The executor and `QThreadPool`
  both go through it; QtConcurrent keeps its own continuation per result.
- **Cores:** each worker asks once for a performance core, as does the main thread. Otherwise macOS
  could place one pool's threads on efficiency cores and not the other's.
- **Cases:** 1 or 4 workers; batches of 1 task (latency) or 1000 (throughput); 21 runs per case, or
  210 for a single task. The pools take turns, after an untimed run each.
- **Mixed lengths:** one more batch of 1000 tasks, on 1 or 4 workers, where one task in ten takes
  1 ms and the rest 10 µs. Which tasks are long is drawn from a fixed seed (105 of the 1000), so
  every pool and every run gets the same batch. It is not a fixed stride: the executor spreads a
  batch nearly round-robin, and a stride matching the worker count would put every long task on one
  worker.
- **What is recorded:** each run records three times from the first submit, as medians:
  - **submitted:** when the main thread finished submitting, which is how long the UI thread was
    busy;
  - **processed:** when the last task finished on a worker;
  - **delivered:** when the last result arrived on the main thread.
- **Checks:** the results are summed on the main thread and checked. A run that has not delivered
  everything after 10 s fails.

## Results

Apple M4 (4 performance and 6 efficiency cores), macOS 26.6.2, Apple clang 17, Qt 6.11.2, Release,
2026-10-10, in µs, with the executor at `ce6fc83` - a task is copied or moved into its worker's
queue once (fix plan step 34) - on async `1f8a02d` and actuator `f21a58b`, and the benchmark at
`260d984`, which gives `QThreadPool` the same work per task (below) and coalesces delivery (step
40). Three runs on that code; this is the first. They agree except for one worker's empty batch,
which falls into one of two modes per run - this run is in the slow one - and single tasks, where a
run can be up to 2x slow on every pool at once.

| workers | tasks | task µs | pool         |  submitted |  processed |  delivered | per task |
|--------:|------:|--------:|--------------|-----------:|-----------:|-----------:|---------:|
|       1 |     1 |     0.0 | executor     |        1.0 |        2.8 |        5.4 |     5.42 |
|       1 |     1 |     0.0 | QThreadPool  |        1.1 |        3.0 |        8.2 |     8.25 |
|       1 |     1 |     0.0 | QtConcurrent |        2.0 |        3.1 |        4.8 |     4.79 |
|       1 |  1000 |     0.0 | executor     |      146.7 |      146.8 |      149.3 |     0.15 |
|       1 |  1000 |     0.0 | QThreadPool  |      128.9 |      140.4 |      144.2 |     0.14 |
|       1 |  1000 |     0.0 | QtConcurrent |      671.5 |      680.2 |     1204.0 |     1.20 |
|       4 |     1 |     0.0 | executor     |        0.6 |        3.2 |        7.0 |     7.04 |
|       4 |     1 |     0.0 | QThreadPool  |        0.6 |        3.3 |        7.2 |     7.17 |
|       4 |     1 |     0.0 | QtConcurrent |        1.1 |        3.7 |        7.6 |     7.58 |
|       4 |  1000 |     0.0 | executor     |       34.9 |       74.5 |       75.4 |     0.08 |
|       4 |  1000 |     0.0 | QThreadPool  |      167.5 |      220.7 |      237.2 |     0.24 |
|       4 |  1000 |     0.0 | QtConcurrent |     1787.3 |     1793.0 |     2213.0 |     2.21 |
|       1 |     1 |    10.0 | executor     |        0.7 |       15.5 |       18.1 |    18.12 |
|       1 |     1 |    10.0 | QThreadPool  |        0.6 |       16.2 |       19.6 |    19.62 |
|       1 |     1 |    10.0 | QtConcurrent |        1.2 |       16.4 |       20.4 |    20.38 |
|       1 |  1000 |    10.0 | executor     |       19.1 |    10924.2 |    10929.5 |    10.93 |
|       1 |  1000 |    10.0 | QThreadPool  |       25.2 |    10973.8 |    10979.0 |    10.98 |
|       1 |  1000 |    10.0 | QtConcurrent |      255.2 |    11057.7 |    11060.8 |    11.06 |
|       4 |     1 |    10.0 | executor     |        0.6 |       15.5 |       18.1 |    18.12 |
|       4 |     1 |    10.0 | QThreadPool  |        0.5 |       16.0 |       19.4 |    19.38 |
|       4 |     1 |    10.0 | QtConcurrent |        1.1 |       16.3 |       20.0 |    20.04 |
|       4 |  1000 |    10.0 | executor     |       29.8 |     3106.5 |     3112.7 |     3.11 |
|       4 |  1000 |    10.0 | QThreadPool  |       34.1 |     3093.0 |     3099.4 |     3.10 |
|       4 |  1000 |    10.0 | QtConcurrent |      562.7 |     3601.5 |     3610.8 |     3.61 |

Across the three runs:

- **The executor submits fastest of the three.** 1000 tasks with real work in 18.2–19.1 µs on one
  worker against `QThreadPool`'s 23.6–25.2, and 29.5–30.4 against 32.2–34.1 on four; empty, on
  four workers, 34.9–36.8 against 157–179. QtConcurrent, the one Qt pool doing the same work out
  of the box, takes 0.26–1.8 ms, building a future and a continuation per task.
- **With real work, delivery is level with `QThreadPool`.** All three are bound by the work: 1000 ×
  10 µs delivered in 10.93–11.37 ms against 10.98–11.30 on one worker, and 3.11–3.16 against
  3.10–3.13 on four. QtConcurrent is ~16% slower on four workers, the cost of its futures.
- **A single task makes the round trip sooner on the executor:** with 10 µs of work, 17.7–18.2 µs
  against `QThreadPool`'s 19.3–19.7 and QtConcurrent's 20.0–20.4. Empty, 5–8 µs on every pool -
  waking a worker plus one event-loop hop.
- **On empty tasks with four workers the executor is three times faster to deliver:** 75–82 µs
  against 227–237.
- **One worker's empty batch has two modes.** In one run of three it stayed out of the submitter's
  way - submitted in 47 µs, delivered in 62. In the other two the worker and the submitter took
  turns on the worker's queue lock, task by task, and delivery followed a 147–157 µs submit:
  149–159, level with `QThreadPool`'s 106–144. Fix plan steps 35 and 38 tried and failed to remove
  that mode without slowing another case.
- **Four workers run 1000 × 10 µs in 3.1 ms**, 3.5x one worker's 10.9 ms.
- **Delivery adds little:** with 1000 tasks the last result reaches the main thread 1–6 µs after the
  last task finished on the executor and `QThreadPool`; QtConcurrent's continuations add ~0.42 ms on
  empty tasks.

### Doing the same work

Until `260d984` `QThreadPool` was measured doing less than the executor. Its lambda was
`[this, i] { common.post(common.task(i)); }`: a pointer and an int, calling the shared task by
reference and posting directly. The executor's `add_task(task, i, callback)` copies the task, keeps
the argument beside it and seals both with a callback it hands the result - as QtConcurrent does,
storing a copy of the task and its argument. `QThreadPool`'s lambda now holds the same: a copy of
the task, the argument and the callback. It still wraps it its own way - `QRunnable::create()`
allocates a `QGenericRunnable` and a `Helper` for each task, against the executor's one sealed task
- which is part of what is compared.

Doing the same work, `QThreadPool` submits 1000 tasks with real work in 23.6–25.2 µs on one worker
against 20.4–21.8 before, and empty tasks on four workers in 157–179 against 103–158; its delivery
with real work did not move. The figures before `260d984` - the rows above in earlier versions of
this file, and the `QThreadPool` column of the history table below - flattered it.

### Mixed lengths

Three runs on the same code, delivered, in ms:

| workers | pool         | run 1  | run 2  | run 3  |
|--------:|--------------|-------:|-------:|-------:|
|       1 | executor     | 115.56 | 122.60 | 113.73 |
|       1 | QThreadPool  | 115.64 | 120.73 | 113.71 |
|       1 | QtConcurrent | 115.88 | 127.11 | 114.04 |
|       4 | executor     |  35.62 |  35.38 |  34.94 |
|       4 | QThreadPool  |  33.18 |  32.73 |  32.54 |
|       4 | QtConcurrent |  34.12 |  33.44 |  33.04 |

- **With uneven tasks the executor is 7–8% behind on 4 workers:** 2.4–2.7 ms in every run. With one
  worker the pools are equal, as they must be: one queue cannot be unbalanced.
- **The cause is where a task goes.** The executor places each task on a worker at submission, ~250
  each, before any has finished; a worker that drew more of the long ones finishes last while the
  others idle. `QThreadPool` places nothing: all tasks wait in one queue and a worker that frees up
  takes the next - not a balancing scheme, but what a single queue does.
- **Accepted (fix plan step 33).** Work stealing closed the gap only with small batches, which cost
  the executor its lead on submitting and on empty batches. The executor competes on per-task cost,
  not on balancing.
- **Submitting is as for uniform tasks:** 20–45 µs per 1000 on the executor, 25–46 on `QThreadPool`.

### How the executor got here

The first runs put the executor behind `QThreadPool` on empty batches, by up to 5x in submitting.
These changes closed it, each found with this benchmark:

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
   against `QThreadPool`'s 17.5–18.5 - and the batches here never reached it.
5. **Step 34** (`ce6fc83`, with actuator `f21a58b` and async `1f8a02d`): a task is copied or moved
   into its worker's queue once, where it had been moved up to five times. Submitting got 17–25%
   faster in every case, level with `QThreadPool` with real work.
6. **Step 40** (`456ac6c`, this benchmark): delivery coalesced, as above. **Step 34 had caused a
   regression** that this resolved: with one post per result, 1000 empty tasks on one worker were
   delivered in 204–277 µs against 155–193 before it. Step 34's faster submit got the main thread to
   its event loop sooner, and a worker posting to a main thread that is waiting there posts 3–6x
   slower - one wake-up per result. With the main thread held back for the same time after
   submitting, step 34's code delivered sooner than the code before it at every hold: the cost was
   per-result delivery, which every pool paid. Coalesced, the same batch is delivered in 48–118 µs.

1000 empty tasks, medians of each run, µs. **The step 40 column and the two `QThreadPool` ones
deliver coalesced**, so their `delivered` is not comparable with the columns before them;
`submitted` is. `QThreadPool` is shown before and after it was given the same work per task
(`260d984`).

| | before (`f844e17`) | step 28 (`e9ef8ea`) | step 31 (`d1245d7`, `6cc4350`) | async step 53 (`c3dc143`) | `5c40e91` | step 34 (`ce6fc83`) | step 40, coalesced (`456ac6c`) | QThreadPool, lighter task (`456ac6c`) | QThreadPool, same work (`260d984`) |
|---|---|---|---|---|---|---|---|---|---|
| 1 worker, `submitted` | 468, 114 | 86, 59, 61 | 72, 50, 50 | 49, 39, 40 | 38, 37, 43 | 41, 29, 39 | **43, 90, 41, 116, 40** | 87–100 | 84–129 |
| 1 worker, `delivered` | 571, 397 | 543, 406, 351 | 363, 245, 175 | 174, 191, 174 | 170, 175, 193 | ⚠ 277, 248, 213 | **53, 92, 51, 118, 48** | 103–120 | 106–144 |
| 4 workers, `submitted` | 524, 473 | 206, 185, 149 | 42, 42, 40 | 41, 38, 42 | 40, 40, 43 | 30, 30, 30 | **38, 38, 37, 35, 32** | 125–167 | 157–179 |
| 4 workers, `delivered` | 668, 604 | 487, 480, 435 | 405, 365, 335 | 494, 411, 362 | 318, 315, 411 | 324, 306, 324 | **81, 79, 75, 76, 76** | 194–224 | 227–237 |

### A trap found on the way

`QFuture::then(context, ...)` runs the continuation **at once, on the calling thread**, when the
future has already finished by the time `.then()` is attached. So a fast task can deliver its result
while the main thread is still submitting. Code that starts an event loop to wait for the last
result, as this benchmark first did, then calls `quit()` before `exec()` and hangs. The harness now
enters the loop only while results are missing.

## Not covered

Results delivered to a real UI (painting), several pools at once (one per store), memory, mixed
lengths beyond one seed and one share of long tasks, and - since delivery is coalesced - one post
per result for the executor and `QThreadPool`, which only QtConcurrent still measures.
