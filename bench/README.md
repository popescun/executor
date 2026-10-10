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
2026-10-10, in µs, with the executor at `23ce729` - a task copied or moved into its worker's queue
once (fix plan step 34), the pool's lock a plain `std::mutex` (step 41) - on async `1f8a02d` and
actuator `f21a58b`, and the benchmark at `260d984`, which gives `QThreadPool` the same work per task
(below) and coalesces delivery (step 40). Three runs on that code; this is the third. They agree
except for single tasks, where a run can be up to 2x slow on every pool at once, and one worker's
1000 × 10 µs batch, which ranged 10.7–12.6 ms on every pool alike - the machine, not the code.

| workers | tasks | task us | pool         |  submitted |  processed |  delivered | per task |
|--------:|------:|--------:|--------------|-----------:|-----------:|-----------:|---------:|
|       1 |     1 |     0.0 | executor     |        0.7 |        3.6 |        8.3 |     8.29 |
|       1 |     1 |     0.0 | QThreadPool  |        0.8 |        3.7 |        7.8 |     7.79 |
|       1 |     1 |     0.0 | QtConcurrent |        1.4 |        4.3 |        9.2 |     9.17 |
|       1 |  1000 |     0.0 | executor     |       39.1 |       47.6 |       48.8 |     0.05 |
|       1 |  1000 |     0.0 | QThreadPool  |      117.9 |      127.0 |      127.8 |     0.13 |
|       1 |  1000 |     0.0 | QtConcurrent |      499.3 |      507.1 |      945.2 |     0.95 |
|       4 |     1 |     0.0 | executor     |        0.7 |        3.4 |        8.1 |     8.12 |
|       4 |     1 |     0.0 | QThreadPool  |        0.7 |        3.4 |        7.5 |     7.50 |
|       4 |     1 |     0.0 | QtConcurrent |        1.2 |        4.0 |        8.8 |     8.83 |
|       4 |  1000 |     0.0 | executor     |       34.2 |       74.5 |       77.2 |     0.08 |
|       4 |  1000 |     0.0 | QThreadPool  |      155.0 |      203.9 |      214.6 |     0.21 |
|       4 |  1000 |     0.0 | QtConcurrent |     1825.2 |     1830.4 |     2236.1 |     2.24 |
|       1 |     1 |    10.0 | executor     |        0.7 |       15.0 |       17.7 |    17.67 |
|       1 |     1 |    10.0 | QThreadPool  |        0.5 |       16.0 |       19.5 |    19.46 |
|       1 |     1 |    10.0 | QtConcurrent |        1.1 |       16.1 |       20.0 |    19.96 |
|       1 |  1000 |    10.0 | executor     |       18.2 |    10723.1 |    10728.4 |    10.73 |
|       1 |  1000 |    10.0 | QThreadPool  |       24.3 |    10752.8 |    10756.1 |    10.76 |
|       1 |  1000 |    10.0 | QtConcurrent |      258.5 |    10962.5 |    10968.0 |    10.97 |
|       4 |     1 |    10.0 | executor     |        0.7 |       15.0 |       17.8 |    17.75 |
|       4 |     1 |    10.0 | QThreadPool  |        0.5 |       16.0 |       19.5 |    19.46 |
|       4 |     1 |    10.0 | QtConcurrent |        1.2 |       16.2 |       20.1 |    20.12 |
|       4 |  1000 |    10.0 | executor     |       30.6 |     3064.1 |     3069.4 |     3.07 |
|       4 |  1000 |    10.0 | QThreadPool  |       33.0 |     3038.6 |     3041.6 |     3.04 |
|       4 |  1000 |    10.0 | QtConcurrent |      867.7 |     3588.4 |     3593.8 |     3.59 |

Across the three runs:

- **The executor submits fastest of the three.** 1000 tasks with real work in 18.2–23.2 µs on one
  worker against `QThreadPool`'s 22.0–25.0, and 27.7–30.6 against 31.5–33.8 on four; empty, on four
  workers, 32.2–35.6 against 122–167. QtConcurrent, the one Qt pool doing the same work out of the
  box, takes 0.25–2.0 ms, building a future and a continuation per task.
- **With real work, delivery is level with `QThreadPool`.** All three are bound by the work: 1000 ×
  10 µs on four workers delivered in 2.98–3.15 ms against 2.95–3.13; on one worker each run's three
  pools are within 0.3%. QtConcurrent is ~17% slower on four workers, the cost of its futures.
- **A single task makes the round trip sooner on the executor:** with 10 µs of work, 17.4–18.3 µs
  against `QThreadPool`'s 18.0–19.7 and QtConcurrent's 18.6–20.3. Empty, 7.4–9.2 µs on every pool -
  waking a worker plus one event-loop hop.
- **On empty tasks the executor delivers 2.5–3x sooner:** 1000 of them in 77–86 µs on four workers
  against `QThreadPool`'s 198–241, and in 48–54 on one worker against 122–128.
- **One worker's empty batch stayed in its fast mode in all three runs**, submitted in 35–39 µs and
  delivered in 48–54. Before step 41 it often fell into a slower mode, in which the submitter and the
  worker took turns on the worker's queue lock task by task, and delivery followed a submit of up to
  ~160 µs. It has not shown in six runs since - three here, three more at `23ce729` - but why is not
  known: the pool's lock step 41 changed is not taken in this benchmark at all, so that change
  cannot have acted on the hand-off. Code layout, or chance, remain; the mode may come back.
- **Four workers run 1000 × 10 µs in 3.1 ms**, 3.5x one worker's 10.7–11.5.
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
|       1 | executor     | 114.76 | 107.03 | 115.46 |
|       1 | QThreadPool  | 114.75 | 107.03 | 116.17 |
|       1 | QtConcurrent | 115.01 | 107.12 | 115.88 |
|       4 | executor     |  35.36 |  33.21 |  35.47 |
|       4 | QThreadPool  |  32.92 |  30.99 |  32.88 |
|       4 | QtConcurrent |  33.54 |  31.44 |  33.33 |

- **With uneven tasks the executor is 7–8% behind on 4 workers:** 2.2–2.6 ms in every run. With one
  worker the pools are equal, as they must be: one queue cannot be unbalanced.
- **The cause is where a task goes.** The executor places each task on a worker at submission, ~250
  each, before any has finished; a worker that drew more of the long ones finishes last while the
  others idle. `QThreadPool` places nothing: all tasks wait in one queue and a worker that frees up
  takes the next - not a balancing scheme, but what a single queue does.
- **Accepted (fix plan step 33).** Work stealing closed the gap only with small batches, which cost
  the executor its lead on submitting and on empty batches. The executor competes on per-task cost,
  not on balancing.
- **Submitting is as for uniform tasks:** 20–46 µs per 1000 on the executor, 25–47 on `QThreadPool`.

### How the executor got here

The first runs put the executor behind `QThreadPool` on empty batches, by up to 5x in submitting.
These changes closed it, each found with this benchmark:

1. **Step 28** (`e9ef8ea`): the pool's lock spins before it blocks. The main thread and the workers
   had been handing a `std::mutex` back and forth through the kernel. Undone by step 41, below.
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
7. **Step 41** (`23ce729`): the pool's lock is a plain `std::mutex` again. Since step 31 no
   submission takes it - only `wait()` and a drain with a `wait()` counted do - and with step 28's
   spin turned off no case moved. The spin in async's queue lock stays: without it four workers
   slowed down by up to 2x.

1000 empty tasks, medians of each run, µs. **The step 40 and step 41 columns and the two
`QThreadPool` ones deliver coalesced**, so their `delivered` is not comparable with the columns
before them; `submitted` is. `QThreadPool` is shown before and after it was given the same work per
task (`260d984`).

| | before (`f844e17`) | step 28 (`e9ef8ea`) | step 31 (`d1245d7`, `6cc4350`) | async step 53 (`c3dc143`) | `5c40e91` | step 34 (`ce6fc83`) | step 40, coalesced (`456ac6c`) | step 41 (`23ce729`) | QThreadPool, lighter task (`456ac6c`) | QThreadPool, same work (`23ce729`) |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 worker, `submitted` | 468, 114 | 86, 59, 61 | 72, 50, 50 | 49, 39, 40 | 38, 37, 43 | 41, 29, 39 | 43, 90, 41, 116, 40 | **35, 38, 39** | 87–100 | 109–118 |
| 1 worker, `delivered` | 571, 397 | 543, 406, 351 | 363, 245, 175 | 174, 191, 174 | 170, 175, 193 | ⚠ 277, 248, 213 | 53, 92, 51, 118, 48 | **49, 54, 49** | 103–120 | 122–128 |
| 4 workers, `submitted` | 524, 473 | 206, 185, 149 | 42, 42, 40 | 41, 38, 42 | 40, 40, 43 | 30, 30, 30 | 38, 38, 37, 35, 32 | **36, 32, 34** | 125–167 | 123–167 |
| 4 workers, `delivered` | 668, 604 | 487, 480, 435 | 405, 365, 335 | 494, 411, 362 | 318, 315, 411 | 324, 306, 324 | 81, 79, 75, 76, 76 | **86, 77, 77** | 194–224 | 198–241 |

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
