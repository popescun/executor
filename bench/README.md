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
| **QThreadPool** | `start(lambda)` | the lambda runs the task and posts the same way, through the same code |
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
`456ac6c`, which coalesces delivery (step 40). Five runs on that code; this is the third. They
agree except for one worker's empty batch, which falls into one of two modes per run (below), and
single tasks, where a run can be up to 2x slow on every pool at once. One worker's 1000 × 10 µs
batch ran ~10% slower than in the previous results here, on every pool alike, which is the
machine, not the code.

| workers | tasks | task µs | pool         |  submitted |  processed |  delivered | per task |
|--------:|------:|--------:|--------------|-----------:|-----------:|-----------:|---------:|
|       1 |     1 |     0.0 | executor     |        0.7 |        3.5 |        8.2 |     8.25 |
|       1 |     1 |     0.0 | QThreadPool  |        0.7 |        3.5 |        7.5 |     7.50 |
|       1 |     1 |     0.0 | QtConcurrent |        1.3 |        4.1 |        8.8 |     8.79 |
|       1 |  1000 |     0.0 | executor     |       41.4 |       49.5 |       50.9 |     0.05 |
|       1 |  1000 |     0.0 | QThreadPool  |       86.7 |      102.5 |      102.8 |     0.10 |
|       1 |  1000 |     0.0 | QtConcurrent |      501.0 |      507.5 |      918.0 |     0.92 |
|       4 |     1 |     0.0 | executor     |        0.7 |        3.4 |        8.1 |     8.12 |
|       4 |     1 |     0.0 | QThreadPool  |        0.7 |        3.4 |        7.5 |     7.50 |
|       4 |     1 |     0.0 | QtConcurrent |        1.2 |        3.9 |        8.5 |     8.54 |
|       4 |  1000 |     0.0 | executor     |       36.7 |       73.3 |       75.1 |     0.08 |
|       4 |  1000 |     0.0 | QThreadPool  |      127.3 |      189.4 |      194.2 |     0.19 |
|       4 |  1000 |     0.0 | QtConcurrent |     1799.3 |     1804.2 |     2223.2 |     2.22 |
|       1 |     1 |    10.0 | executor     |        0.7 |       15.0 |       17.8 |    17.79 |
|       1 |     1 |    10.0 | QThreadPool  |        0.5 |       15.9 |       19.7 |    19.67 |
|       1 |     1 |    10.0 | QtConcurrent |        1.3 |       16.1 |       20.2 |    20.17 |
|       1 |  1000 |    10.0 | executor     |       22.5 |    11428.1 |    11434.1 |    11.43 |
|       1 |  1000 |    10.0 | QThreadPool  |       20.5 |    11644.6 |    11647.5 |    11.65 |
|       1 |  1000 |    10.0 | QtConcurrent |      269.0 |    11439.0 |    11443.0 |    11.44 |
|       4 |     1 |    10.0 | executor     |        0.7 |       15.0 |       17.7 |    17.67 |
|       4 |     1 |    10.0 | QThreadPool  |        0.5 |       15.9 |       19.5 |    19.54 |
|       4 |     1 |    10.0 | QtConcurrent |        1.2 |       16.1 |       20.2 |    20.17 |
|       4 |  1000 |    10.0 | executor     |       27.8 |     3085.2 |     3092.8 |     3.09 |
|       4 |  1000 |    10.0 | QThreadPool  |       26.8 |     3032.6 |     3034.9 |     3.03 |
|       4 |  1000 |    10.0 | QtConcurrent |      654.0 |     3552.2 |     3561.9 |     3.56 |

- **With real work, the executor and `QThreadPool` are equal.** Both are bound by the work: 1000 ×
  10 µs delivered in 10.99–11.56 ms against 11.25–12.09 on one worker, and 2.94–3.20 against
  2.92–3.11 on four, across the five runs. QtConcurrent is 15–20% slower on four workers, the cost
  of its futures.
- **The executor now submits as fast as `QThreadPool` with real work:** 1000 tasks in 19.6–24.6 µs
  against 20.5–22.2 on one worker, and 27.8–30.0 against 26.8–31.4 on four. Before step 34 it took
  24–26 and 35–37.
- **A single task makes the round trip in 7.5–15 µs empty on every pool:** waking a worker plus one
  event-loop hop. With 10 µs of work it takes 17.7–18.7 µs on the executor against `QThreadPool`'s
  18.5–19.7.
- **On empty tasks, where only overhead counts, the executor is well ahead.** 1000 tasks are
  delivered in 75–81 µs on four workers against `QThreadPool`'s 194–224, and submitted in 32–38
  against 125–167. On one worker: 48–118 against 103–120 - see the next point.
- **One worker's empty batch has two modes.** With delivery cheap, one worker keeps up with the
  submitter. In three runs of five it stayed out of the submitter's way - submitted in 40–43 µs,
  delivered in 48–53. In the other two the worker and the submitter took turns on the worker's queue
  lock, task by task, and delivery followed a 90–116 µs submit: 92 and 118. The code before step 34
  fell into that mode in four runs of five, at 166–231 µs. Fix plan step 35 is about it.
- **QtConcurrent is the slowest to submit by far** (0.26–1.9 ms per 1000 tasks), because it builds a
  future and a continuation per task, and its per-result continuations add 0.41 ms on empty tasks.
- **Four workers run 1000 × 10 µs in 3.1 ms**, 3.7x one worker's 11.4 ms.
- **Delivery adds little:** with 1000 tasks the last result reaches the main thread 1–8 µs after the
  last task finished on the executor and `QThreadPool`.

### Mixed lengths

Five runs on the same code, delivered, in ms:

| workers | pool         | run 1  | run 2  | run 3  | run 4  | run 5  |
|--------:|--------------|-------:|-------:|-------:|-------:|-------:|
|       1 | executor     | 115.53 | 110.27 | 111.12 | 113.15 | 112.11 |
|       1 | QThreadPool  | 115.51 | 110.11 | 111.48 | 112.75 | 113.26 |
|       1 | QtConcurrent | 115.79 | 109.90 | 111.06 | 112.77 | 114.00 |
|       4 | executor     |  35.51 |  33.46 |  33.51 |  34.16 |  32.80 |
|       4 | QThreadPool  |  33.05 |  31.10 |  31.11 |  31.85 |  30.35 |
|       4 | QtConcurrent |  33.63 |  31.60 |  31.73 |  32.15 |  30.95 |

- **With uneven tasks the executor is 7–8% behind on 4 workers:** 2.3–2.5 ms in every run. With one
  worker the pools are equal, as they must be: one queue cannot be unbalanced.
- **`QThreadPool` is at the machine's limit.** Four workers ran uniform 1 ms tasks 3.57x faster
  than one, on every pool.
- **The cause is where a task goes.** The batch is submitted in about 40 µs, before any worker
  drains, so the executor deals it out round-robin and each worker's share of long tasks is the
  draw's. About 26 of the 105 is fair; the worker that got more finishes last while the others
  idle. `QThreadPool`'s workers take from one shared queue, so an idle one takes the next task.
- **The gap repeats because the batch does.** Another seed, another share of long tasks or more
  workers would move it; fewer, longer tasks make it larger. Closing it means work stealing, fix
  plan step 33.
- **Submitting is as for uniform tasks:** 20–47 µs per 1000 on the executor and `QThreadPool` alike.

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

1000 empty tasks, medians of each run, µs. **The last two columns deliver coalesced**, so their
`delivered` is not comparable with the columns before them; `submitted` is.

| | before (`f844e17`) | step 28 (`e9ef8ea`) | step 31 (`d1245d7`, `6cc4350`) | async step 53 (`c3dc143`) | `5c40e91` | step 34 (`ce6fc83`) | step 40, coalesced (`456ac6c`) | QThreadPool, coalesced |
|---|---|---|---|---|---|---|---|---|
| 1 worker, `submitted` | 468, 114 | 86, 59, 61 | 72, 50, 50 | 49, 39, 40 | 38, 37, 43 | 41, 29, 39 | **43, 90, 41, 116, 40** | 87–100 |
| 1 worker, `delivered` | 571, 397 | 543, 406, 351 | 363, 245, 175 | 174, 191, 174 | 170, 175, 193 | ⚠ 277, 248, 213 | **53, 92, 51, 118, 48** | 103–120 |
| 4 workers, `submitted` | 524, 473 | 206, 185, 149 | 42, 42, 40 | 41, 38, 42 | 40, 40, 43 | 30, 30, 30 | **38, 38, 37, 35, 32** | 125–167 |
| 4 workers, `delivered` | 668, 604 | 487, 480, 435 | 405, 365, 335 | 494, 411, 362 | 318, 315, 411 | 324, 306, 324 | **81, 79, 75, 76, 76** | 194–224 |

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
