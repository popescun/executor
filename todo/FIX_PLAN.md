# executor.hpp — fix plan

**Status (2026-09-21) — group 1 is done: every blocker closed or disproven.** The pool was imported
from `async/prototypes/executor.hpp` as it stood, and this is the audit of what has to change before
it can be called production ready. 19 items, in seven groups — item 19 was added after the audit
closed and is a decision rather than a finding; see group 7. Items 20 and 21 were added the same
way and are steps 23 and 24, a rename and an open question about reaching the workers. **Four were called blockers**: each was
read as a way the pool can hang or read freed memory. **None is open.** Steps 1, 2 and 4 were fixed;
step 3 was probed and does not reproduce, so it is closed as not a defect rather than fixed. What is
left is group 2 onwards — what the caller is told, placement, and the surface.
**Tests:** 24 of 24 green on Debug, ASan and TSan, 2026-09-22; clang-format clean;
doxygen clean, `doc/refman.pdf` at 25 pages (was 19 at the import), rebuilt with
`tools/make_doc.sh`. **Steps 2 and 4 are closed** (`b68cc97`, `032da65`). The destructor waits on an
`async::execution_poll` until every worker has left its thread and clears them only then, and it
reports on stderr while either of its waits is stalled rather than parking in silence. The race TSan
named on CI no longer reproduces: 24 of 24 under TSan and under ASan on macOS/libc++, where the case
that provokes it aborted before the fix. **That is one platform, not both** — the Linux/libstdc++
run this plan insists on has not been made since either fix, which is why step 19 stays open.
**Sites** are line numbers in `executor.hpp` as of `032da65`, and they move with every fix that
lands — they were remapped after steps 14 and 1, and steps 2 and 4 moved them again. Re-read them
before trusting them. **Names move too:** `032da65` replaced "drain" with "finish" throughout, so
`drained_cv_` is `finished_cv_` and entries written before it may still say drain.
**Source:** read of `executor.hpp` against `async.hpp` at `22c5892`, 2026-09-21. Five findings are
confirmed by a probe or by a test that failed before it was corrected; the rest are read-only and
say so.

**The four gaps the prototype's own README raised are not in this plan.** They were about
`async.hpp`, and `async.hpp` has since closed all four: a throwing action is caught in three arms
(async step 28), `std::cout` is gone (18, 19), `is_busy()` exists (the prototype asked for it), and
`add_action()` returns `bool` (21). What is left of them here is item 5 — the pool is what runs
arbitrary caller code, so *the submitter* still learns nothing when a task throws — and item 11,
which is upstream's to fix.

**The API is fixed by a decision, not by this plan — with one part of it now reversed and landed.**
The pool keeps the prototype's surface, under one new name: `add_task()` — `submit()` until step
23 — and `pending()`. No futures, no priorities, no results. Items 9, 16 and 18 are recorded against that decision rather than argued with; if the
decision changes, they are where to start. **The task type is no longer part of it:** the pool runs
any task type, as of `9daec8b`. That was item 19, group 7, and what it pulls along with it — what a
non-void return gives back — is still items 5 and 18's to settle.

## Progress

Done — steps 14, 1, 2 and 20. Step 14 was a refactor: nothing it touched was defective. **Step 1
was the first blocker closed**, and the cheapest of the four — a guard in the constructor, no change
to how a working pool behaves. **Step 2 is the second, and the one that mattered**: it is the only
use-after-free in the audit, and the only finding a sanitizer had named. Step 20's stress case is
what made it reproducible on a second platform, and it landed in step 2's own commit because the
case is that fix's evidence. **Step 3 is closed without a code change**: a probe disproved its
premise — a constructor that throws already stops and waits for every worker it started, because
member destruction unwinds `workers_` and `~execution()` does both. **Step 4 keeps its unbounded
wait and reports instead**: bounding it was never available, since giving up early is step 2's
use-after-free and a bounded wait only moves the hang into `~execution()`. **No blocker remains.**

Step 19 is still under way. The "before" half ran on 2026-09-21, locally and then on CI, and paid
for itself immediately: CI's TSan named step 2, which had been the one blocker resting on a reading
rather than on a report. The "after" half has now run on macOS/libc++ and is clean, through the
presets `cfd5aea` added. What it still owes is Linux/libstdc++ — the platform that produced the
report in the first place. Until that run, step 2 is fixed on the evidence of the toolchain that
was never the one complaining.

**Working method, from 2026-09-21.** Each step is a test first: a case that shows the defect, put up
for review on its own, and the fix written only once that case is agreed. The test is the unit of
review, not the fix.

**One step per commit**, and a step's case travels with its own fix. `4c1cba6` is the counter-example
to avoid: it is step 14's refactor, and step 1's case rode along in it because the case had been
written while the refactor was still unstaged. Nothing was wrong with either change, but the commit
says one thing in its subject and does two, and a reader chasing step 1 finds half of it under step
14. `ba4eb82` is the shape to keep — `executor.hpp` alone, 16 lines, one step.

The plan's own updates are their own commit too, and always a later one: a commit cannot record its
own hash, so the table below is filled by the `chore: update fix plan` that follows the fix. That is
also why an amended fix needs a second look here — `bf7739b` became `4c1cba6` under an amend and left
three dangling references behind it.

**NEXT: steps 6 and 7, the rest of group 2.** Step 5 is done — through async's step 38 rather than
here, see below. Groups 1 and 7 are done, and step 22
sharpened what group 2 has to answer: a pool can now be built on `std::function<int(void)>`, and
what it does with the `int` is nothing. That is steps 5 and 18's question arriving by a third door,
which is what this plan predicted. The couplings below say to settle 5 and 6 together rather than in
two passes: both ask what happened to the task I gave you, and the async plan's own steps 21 and 28
answered that question twice and badly by splitting it. Step 13 comes before step 5, because a
report needs a pool name to put in it.

Step 4 left two things behind that are neither group 2's nor this repo's. The report names the
worker but not the task: `taskT` is a callable and carries no label, so naming one needs a task type
that has somewhere to put it — step 22 makes that the caller's to choose, but nothing here reads it.
Ending a hang rather than explaining it needs cancellation in `async::execution`. Both are at the
foot of step 4.

Step 22 (any task type) comes after all four, not before: a hang and a use-after-free outrank a
surface change, and templating the class is a whole-file diff that is far easier to review against
a baseline already known correct.

**Read the couplings before picking an order.** Steps 1 and 4 are both about a destructor that
waits for something that will never come, and the guard for one is the place to put the other.
Steps 5 and 6 ask the same question — what happened to the task I gave you — and the async plan's
experience with its own steps 21 and 28 was that answering it in two passes answers it twice and
badly; settle them together. Step 14 changed what a worker *is* here, and steps 2 and 8 both
rewrite the lines that name one — that coupling is discharged, and both now start from
`workers_[i]->` rather than `workers_[i].exec->`. Step 13 gives
the pool a name and step 5 needs something to put in a report, so 13 comes first of those two.
Step 22 touches every use of `worker_execution`, which step 14 has now settled the spelling of; and
it asks step 5's and step 18's question in a third form — what does a task that
returns something give back — so read those two before deciding what it means for a non-void task.

| Commit | Step |
|---|---|
| `4c1cba6` | 14 — `struct worker` replaced by `std::vector<std::shared_ptr<worker_execution>>` |
| `ba4eb82` | 1 — `executor(0)` refused with `std::invalid_argument` |
| `cfd5aea` | 19 (part) — `debug`/`release`/`asan`/`tsan` presets, so a sanitizer run is one command |
| `b68cc97` | 2 — `~executor()` waits on an `execution_poll` before clearing; 20 — its stress case |
| `2814742` | — `async` submodule to `22c5892`, which removed `execution_poll::get()` |
| `032da65` | 4 — both destructor waits report the workers they are still waiting on |
| `9daec8b` | 22 — `executor` templated on its task type, `async::execution` written once |

## Step index

| # | Item | Concern | Sites | Verified |
|---|---|---|---|---|
| **Group 1 — lifetime (blockers)** |
| 1 ✅ | 1 | a pool with no workers takes work nothing can run, and never dies | `:57-74`, `:79-94` | CONFIRMED (hangs; probe) |
| 2 ✅ | 2 | `~executor()` mutates `workers_` outside the mutex every reader takes | `:95-116` | CONFIRMED (TSan, CI) — fixed `b68cc97` |
| 3 ✅ | 3 | a constructor that throws leaves started workers holding `this` | `:59-84` | DISPROVEN (probe) — not a defect |
| 4 ✅ | 4 | the destructor waits forever on a task that never returns | `:112-129`, `:144-160` | CONFIRMED (test) — fixed `032da65` |
| **Group 2 — what the caller is told** |
| 5 ✅ | 5 | a task that throws is reported to stderr and to nobody else | `:90-92`, `:233`, `:243` | CONFIRMED (test) — fixed via async `on_error` |
| 6 | 6 | `add_action()`'s answer is dropped, so a refused task vanishes | `:140-145` | read-only |
| 7 | 7 | work spawned by an in-flight task is refused once shutdown starts | `:82`, `:101-121` | CONFIRMED (test) |
| **Group 3 — placement** |
| 8 | 8 | the free-worker scan always starts at worker 0 | `:110-117` | CONFIRMED (20 tasks, 1 of 4; probe) |
| 9 | 9 | the queue is unbounded and nothing pushes back | `:119` | read-only, API decision |
| **Group 4 — what async.hpp imposes** |
| 10 | 10 | the lock order rests on an async.hpp detail that is not its contract | `:140-145`, `:171-179` | read-only |
| 11 | 11 | every worker prints to stdout on shutdown | `async.hpp:757` | CONFIRMED (14 lines of 38) |
| 12 | 12 | the 10ms tick, once per worker | `async.hpp:744-747` | measured, not a defect |
| **Group 5 — surface and hygiene** |
| 13 | 13 | worker names collide between pools | `:65` | read-only |
| 14 ✅ | 14 | `struct worker` is a one-field wrapper the prototype outgrew | `:119-121` | read-only |
| 15 | 15 | copy and move are suppressed by accident, not by statement | `:181` | read-only |
| 16 | 16 | a move-only task does not compile | `:41` | CONFIRMED (compile probe) |
| 17 | 17 | `pending()` is advisory and does not say so | `:126-129` | read-only |
| 18 | 18 | no way to wait for the pool to drain short of destroying it | `:79-94` | read-only, API decision |
| 23 ✅ | 20 | `submit()` does not match `execution::add_action()` | `:171` | naming decision — renamed `add_task()` |
| 24 | 21 | the workers are unreachable, and so is every seam on them | `:375` | read-only, surface decision |
| **Group 7 — the task type** |
| 22 ✅ | 19 | `task_t` is fixed at `std::function<void(void)>` | `:45`, `:51`, `:202` | CONFIRMED (probe, tests) — fixed `9daec8b` |
| **Group 6 — the suite** |
| 19 | — | the sanitizers have never been run against the suite | `.github/workflows/ci.yml` | — |
| 20 ✅ | — | the suite has no case that runs the pool hard | `test/executor_tests.cpp:457` | — |
| 21 | — | README and the reference state behaviour the fixes will change | `README.md` | — |

---

## Group 1 — lifetime (blockers)

Four ways the pool was read as outliving or under-living itself. Every one is in the constructor or
the destructor, and none needs a caller to do anything unusual. All four are settled: 1, 2 and 4 by
a fix, 3 by a probe that disproved it.

### Step 1 ✅ · item 1 — a pool with no workers takes work nothing can run
`executor.hpp:45-59`, `:79-94` · CONFIRMED by probe: the destructor does not return

`executor(0)` is accepted. `workers_` is empty, so `add_task()` finds no free worker, queues the task
and answers true; `pending()` says 1. The destructor then waits for `pending_.empty()`, which no
worker will ever make true, and `nothing_running()` — which is vacuously true over no workers —
never brings the predicate up with it. Nothing notifies `finished_cv_` either, because only
`take_next_task()` does and it runs on a worker thread. The probe hung at its 3-second watchdog:

```
submit() returned true
pending() = 1
leaving scope ...
HUNG: ~executor did not return within 3s
```

An empty pool that is never given work destructs cleanly, which is what makes this quiet: the hang
needs a `add_task()`, and `add_task()` is the one thing every caller does.

`std::thread::hardware_concurrency()` returns 0 when it cannot tell, and a caller passing it
straight through is the likely way in.

> Rejected in the constructor — `std::invalid_argument`, thrown before any worker is started, so a
> pool that cannot run anything never exists rather than existing and hanging. `<stdexcept>` joined
> the includes. A default worker count is a separate question and was not taken here; the
> constructor's `@remark` says so, and points at `hardware_concurrency()` returning 0 as the way a
> zero most often arrives.
>
> `executor_tests.a_pool_with_no_workers_is_refused` pins it: one line,
> `EXPECT_THROW(executor(0), std::invalid_argument)`, reporting *"it throws nothing"* before the
> guard and green after. It states the guard rather than the hang — an empty pool that is never
> given work destructs cleanly, so the case never reaches the wait and fails in milliseconds
> instead of wedging the suite for three seconds. The hang's standing evidence is the probe above.
> A first draft built the pool on its own thread behind a three-second watchdog and did reproduce
> the hang as a failure; it was dropped once the answer was settled as the throw, because it left a
> thread parked in `~executor` that cannot be joined and that a leak checker would report.

### Step 2 ✅ · item 2 — `~executor()` mutates `workers_` outside the mutex — DONE (`b68cc97`)
`executor.hpp:95-116` · **CONFIRMED by TSan on CI, 2026-09-21**: data race in
`executor_smoke_test.concurrent_submission`, GCC 14 / libstdc++ / Linux

**What landed, and why it is not the fix this step prescribed.** The blockquote at the end of this
section asked for the destructor to hold `mutex_` across the parts that touch `workers_`, with the
workers moved out under the lock and a companion early-return in `take_next_task()`. That is not
what was written. `~executor()` drains as before, calls `stop()` on each worker outside the lock,
and then waits on an `async::execution_poll` — a member, declared before `workers_` — until no
worker is inside its thread at all, clearing the vector only after that:

```c++
while (poll_.is_running()) {  // time of check
  std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval_ms));
}

// time of use: no worker thread is left to reach workers_, so clearing it races nothing.
workers_.clear();
```

The poll is the better answer because it closes **both** mechanisms this section names with one
wait, rather than the buffer race alone. A worker is detached and cannot be joined, so asking
whether it is still running is the only way to wait for one, and the poll answers that for any
number at once — which is exactly the gap `~execution()` cannot close, since each one waits only
for itself. Once the poll reports idle there is no thread left to read any element, so the
element-0-freed-while-element-2-runs case below disappears too. The lock dance and the
`take_next_task()` guard are both unnecessary against a vector nobody can still reach, and neither
was written.

**Verified after the fix:** 20 of 20 on `debug`, `asan` and `tsan`, Apple clang 21.0.0 /
arm64-apple-darwin25.6.0. `destroying_a_pool_under_load_does_not_race_its_workers` aborted under
TSan before this change and passes after. **Not yet re-run on Linux/libstdc++**, which is the
toolchain that reported the race — see step 19.

The finding as it was diagnosed follows, unchanged.

`mutex_` guards `workers_` for every reader: `add_task()` scans it, `nothing_running()` iterates it,
`take_next_task()` indexes into it. The destructor is the one writer, and it takes no lock at all:

```c++
}  // the lock from the drain wait is released here
for (auto& one : workers_) {
  one.exec->stop();
}
workers_.clear();
```

`workers_.clear()` destroys every element and frees the vector's buffer. A worker thread can be
inside `take_next_task()` at that moment, holding `mutex_` — which the destructor is not holding —
and iterating the same vector from `nothing_running()`.

The window is real rather than theoretical. The drain predicate is satisfied by whichever worker
calls `take_next_task()` last, and it reads the *other* workers through `is_busy()`. A worker that
has just cleared `executing_action_` and has not yet reached its own `on_finished` reads as idle
there, so the destructor can be released by one worker while another is one instruction away from
entering `take_next_task()`. The second worker then reaches a vector being cleared.

What happens next is not a clean crash: `clear()` releases the last `shared_ptr` to each execution,
and `~execution()` waits for its worker to leave — the worker that is at that moment reading the
vector being destroyed.

#### What TSan named — and where the paragraphs above were one level off

The first CI run reported it, in `concurrent_submission` (`executor_smoke_test.cpp:121-145`: three
workers, 400 tasks from four threads, destructor at the closing brace). The two ends:

```
Write of size 8 by main thread:
  operator delete
  _Sp_counted_ptr_inplace<execution<function<void()>>>::_M_destroy()
  ~shared_ptr() -> untangle::executor::worker::~worker()
  std::vector<worker>::clear()
  untangle::executor::~executor()                       executor_smoke_test.cpp:139

Previous atomic read of size 1 by thread T3 (mutexes: write M0):
  pthread_mutex_lock -> std::mutex::lock()
  untangle::async::execution<function<void()>>::is_busy() const
  untangle::executor::nothing_running() const
  untangle::executor::take_next_task(unsigned long)
  executor::executor(...)::{lambda()#1}   (on_finished)
  execution::notify_finished() -> execute_actions() -> loop()
```

M0 is the pool's own `mutex_` — T3 holds it, the destructor does not. That is the finding, exactly
as stated above.

**It now reproduces locally too, and the suite has a case for it.**
`executor_tests.destroying_a_pool_under_load_does_not_race_its_workers` — 50 rounds of build a
4-worker pool, submit 64 tasks, leave the scope without waiting — aborts under TSan on macOS/libc++
as well as on CI's Linux/libstdc++. Not waiting is the mechanism: the queue is still backed up when
the destructor starts, so workers are inside `take_next_task()` when it reaches `clear()`. The local
report names the same race one field over — the execution's `action_queue_` deque, which `is_busy()`
reads through `empty()` (`async.hpp:333`), rather than the `action_mutex_` CI named. Both are inside
the block being freed.

**Only TSan can see it**, and that is the defect's nature rather than the case's weakness. Pushed to
16 workers and 200 rounds, a plain build crashed 0 times in 10 and ASan reported nothing in 5: the
race is a free against a read with nothing ordering them, so ASan faults only if the read actually
lands after the free, while TSan reports the missing order itself. Do not read an ASan pass as an
acquittal here.

**The earlier claim that this was Linux-only was wrong.** The suite's other eighteen cases were
TSan-clean on macOS while CI reported the race, and the difference was never the platform — it was
that nothing in the suite destroyed a pool often enough to open the window.

**The racing object is not the one this step predicted.** The paragraphs above say the second worker
"reaches a vector being cleared", i.e. the race is on the `workers_` buffer. What TSan names is one
level deeper: the freed block is the *execution* that `make_shared` allocated, and the read is
`is_busy()` taking that execution's `action_mutex_`. The route runs through the vector; the memory
in the report is the pointee.

That matters, because it names a second mechanism the paragraphs above do not cover. `clear()`
destroys elements **in order**, and `~execution()` waits only on **its own** `running_`
(`async.hpp:208-231`). So destroying worker 0 frees worker 0's `action_mutex_` while workers 1 and 2
are still alive and still calling `nothing_running()`, which iterates *every* worker and locks
*every* `action_mutex_`. The destructor does not have to be racing the buffer; element 0 being gone
while element 2's thread still runs is enough, and no amount of waiting inside `~execution()` closes
it, because each one waits only for itself.

**Superseded — the shape below is not what landed; see the top of this step.** Kept because it
records what the fix had to beat, and because its last paragraph is a live question step 7 inherits:
if a running task may still submit, `pending_` is not empty at teardown.

> The destructor holds `mutex_` across the parts that touch `workers_`, and the parts that must not
> hold it — `stop()`, which wakes a worker that will want the mutex, and `clear()`, which waits for
> one — need the workers moved out of the member first, under the lock, and stopped through the
> local copy. That ordering is the step; a reader-writer split or an `std::atomic` flag is not,
> because the invariant being protected is "the vector still exists".
>
> **The TSan report adds one requirement the sketch above is missing.** Moving the workers out under
> the lock leaves `workers_` empty, which is what makes it safe — a worker that then enters
> `take_next_task()` takes `mutex_`, finds an empty vector, and locks nobody's freed
> `action_mutex_`. But `take_next_task()` also does `give_to_worker(index, ...)`, which indexes
> `workers_[index]`, and an empty vector makes that an out-of-bounds write rather than a no-op. So
> the move-out needs a companion: `take_next_task()` must return early once the pool is tearing
> down, rather than relying on `pending_` being empty by then. It is empty today, after the drain —
> but step 7 is the open question of whether a running task may still submit, and if that answer is
> "yes" it is no longer empty. Write the guard now; the two steps meet here.

### Step 3 ✅ · item 3 — a throwing constructor leaves started workers holding `this` — CLOSED, NOT A DEFECT
`executor.hpp:59-84` · **DISPROVEN by probe, 2026-09-21** · no code change

**The premise was wrong, and a probe is what showed it.** The finding reasoned that because
`~executor()` is never called for an object that never finished constructing, nothing cleans up.
Member destructors still run. `workers_` is a `std::vector<std::shared_ptr<worker_execution>>`, so
unwinding destroys it, dropping the last reference to each execution, and `~execution()`
(`async.hpp:266`) sets `stopped_`, notifies `action_cv_`, and spins on `running_` until that worker
has left `loop()`. Stopping and waiting for every started worker is exactly what the scope guard
below was to be written for, and it already happens.

**The probe.** A scratch copy of the header with `throw std::runtime_error(...)` injected straight
after `workers_.back()->start()` at `index == 2`, constructing `executor(4)` so two workers are
started and running when the body throws:

```
== constructing executor(4), throw injected at worker 2 ==
execution 'pool_worker_2' thread finished
execution 'pool_worker_1' thread finished
execution 'pool_worker_0' thread finished
caught: injected: thread creation failed
== 500ms after the unwind ==
== clean exit ==
```

Clean under ASan and under TSan, exit 0, Apple clang 21.0.0. The ordering is the result: every
worker reports `thread finished` **before** the catch, so all three are stopped and waited for
during the unwind rather than left running.

**The dangling callback cannot fire either.** `on_finished` does capture `this`, but
`notify_finished()` (`async.hpp:781`) returns early on `actions_run_ == 0 || !on_finished`. A worker
that has never run an action never calls back, and during construction no task can exist — nothing
can reach `add_task()` on a pool that has not finished constructing. That is also what keeps step 2's
race off this path: no worker is ever inside `take_next_task()` iterating a `workers_` being
destroyed.

**The other two triggers do not survive either.** `push_back` cannot reallocate, because
`workers_.reserve(worker_count)` runs before the loop. A `bad_alloc` out of `create_instance`
unwinds identically to the injected throw.

> **Nothing to do.** The function-try-block the step asked for would re-implement what member
> destruction already does. What remains is not this step's: the unwind is correct because
> `~execution()` stops *and* waits, which is async.hpp's behaviour rather than a stated contract of
> it — the same exposure step 10 records about the lock order. If that is ever to be pinned down,
> pin it there, once, for both.
>
> The original prescription, kept for the record: *a function-try-block, or a scope guard around the
> loop: on the way out, stop and release whatever was started, then rethrow.*

### Step 4 ✅ · item 4 — the destructor waits forever on a task that never returns — DONE (`032da65`)
`executor.hpp:112-129`, `:144-160` · **CONFIRMED (test)** · the wait is unchanged; the silence is what was fixed

**The wait is still unbounded, and that is the answer rather than a compromise.** Abandoning a
running task would be worse, and it is not even available: returning early from `~executor()` while
a worker is still in a task is exactly the use-after-free step 2 closed, and bounding the wait would
only move the hang three lines down into `~execution()`, which spins on `running_`
(`async.hpp:282`) with no timeout of its own. Workers are detached and `async::execution` has no
cancellation, so a pool cannot outlive a task it cannot interrupt. What was fixed is the silence.

**Both waits report, and they are kept apart because they answer different questions.** Step 2 split
one wait into two, and they are not interchangeable:

| Wait | Asks | Reports |
|---|---|---|
| `finished_cv_`, `:112-129` | who is still inside a task | queue depth, and `is_busy()` workers by name |
| the poll loop, `:144-160` | whose thread has not left | `is_running()` workers by name |

The second is "which worker does not stop", and it can be true of a worker that is idle — which is
why a single combined report would have been the wrong shape.

**It names workers rather than counting them.** `count_workers()` and `name_workers()` take a
member-function pointer, so one pair of helpers serves both waits through `is_busy()` and
`is_running()`. The name is `execution::name`, so one worker reads the same here and in async's own
warnings:

```
executor: still waiting to finish after 1s - 1 queued, 4 in a task: pool_worker_0, pool_worker_1, pool_worker_2, pool_worker_3
executor: still waiting to finish after 3s - 1 queued, 4 in a task: pool_worker_0, pool_worker_1, pool_worker_2, pool_worker_3
executor: still waiting to finish after 7s - 0 queued, 1 in a task: pool_worker_1
```

**The interval starts at 1s and doubles to a 30s ceiling** (`report_first_ms`, `report_max_ms`). A
fixed interval was what this step originally asked for; backoff was taken instead because the two
things wanted pull opposite ways — a stuck teardown should say something almost at once, and a long
one should not become a log flood. It also keeps the case that tests it near two seconds rather than
past the interval.

**Verified by `executor_tests.a_destructor_stuck_on_a_task_reports_why`**, which failed before this
with an empty capture after two seconds. 23 of 23 on `debug`, `asan` and `tsan`.

> **What is left is not this step's, and not this repo's.** The report names the worker and not the
> task, because a `taskT` is a callable and carries no label. Step 22 has since made the task type
> the caller's to choose, so a type with somewhere to put a name is now possible — but nothing here
> reads one, and a pool that required it would stop being generic. Ending the hang at all — rather
> than explaining it — needs cancellation in `async::execution`, a stop token an action can poll.
> That one is async's, in the way step 11 is.

## Group 2 — what the caller is told

### Step 5 ✅ · item 5 — a task that throws is reported to stderr and to nobody else — DONE
`executor.hpp:90-92` (the wiring), `:233` (the member), `:243` (`report_task_error()`) · CONFIRMED
by test: `what_a_task_throws_reaches_the_caller`

The pool has an `on_task_error`, assigned by the caller and called with a `std::exception_ptr`:

```c++
pool.on_task_error = [](std::exception_ptr thrown) { ... };
```

**It could not be built here, and that is the finding this step turned on.** `execute_actions()`
catches what a task throws, prints, and drops it; nothing readable survives, and `actions_run_`
counts a throw as run. For the pool to see an exception it would have to wrap every task in its own
try/catch — which means constructing a `taskT` from a lambda. That works for `std::function` and not
for the bare functor step 22 had just made legal and tested. **Three routes were weighed:**

| Route | Cost |
|---|---|
| Constrain `taskT` to what a lambda converts into | walks back step 22 a day after it landed; `a_pool_runs_a_task_type_that_is_not_a_std_function` stops compiling; every task pays a wrap |
| Wrap only where it compiles, handler gated by `static_assert` | two classes of pool with different capabilities, split by the template argument; the handler must become a method, so it stops mirroring `on_finished` |
| **Taken:** a seam upstream, in `execution` | a two-repo change — async's step 38, then the submodule bump |

The exception belongs to whoever queued the action, and `execution` is the one holding it. async
step 38 added `on_error` there; this step is the pool wiring it through. No wrapping, so `taskT` is
never constructed from a lambda and step 22 is untouched — a functor pool reports errors like any
other.

**The pool takes `on_error` whether or not a caller has set `on_task_error`**, because it cannot be
assigned later: a worker thread reads it, and construction is the last moment nothing is running.
That would have made a pool *worse* than the execution it wraps — async prints only when no handler
is set, and the pool having taken the handler would have silenced it. So `report_task_error()` keeps
the floor, and names the worker while it is at it:

```
warning: executor task on 'pool_worker_0' threw: nobody is listening
warning: executor task on 'pool_worker_0' threw an unknown type
```

**Results are still not collected**, and this does not reopen that. A throw is not a return value;
steps 18 and 22 keep that question.

**Verified:** 24 of 24 on `debug`, `asan` and `tsan`; clang-format clean; `doc/refman.pdf` at 25
pages. README gains the handler and the warning it replaces.

> **Step 13 is no longer a prerequisite, and is still worth having.** This step was sequenced after
> it because a report needs a name the caller recognises. What the handler delivers is the exception
> itself, which needs no name at all — so 13 became optional here. It still matters for the stderr
> floor above and for step 4's stall report, both of which name a worker that two pools would share.

### Step 6 · item 6 — `add_action()`'s answer is dropped — OPEN
`executor.hpp:129-134` · read-only

```c++
workers_[index].exec->add_action(std::move(task));
```

`add_action()` returns `bool` — async's step 21 made it do so precisely so a refusal is not silent
— and `give_to_worker()` ignores it. A refused task is destroyed inside `add_action()` and the pool
goes on believing it placed it. `add_task()` has already answered true by then, or
`take_next_task()` has already popped it off `pending_`, so the task is gone from both ends.

It cannot happen today: a worker refuses only after `stop()`, and `stop()` is only called by the
destructor, after the drain. That is an invariant held in place by the order of two functions and
written down nowhere, which is what makes an unchecked return worth this step rather than a shrug.

> Check it. **The recovery this step asked for cannot be written, though — found while wiring step
> 5, 2026-09-22.** "A refusal puts the task back at the front of `pending_`" assumes the task still
> exists when the answer arrives, and it does not: `add_action(actionT action, ...)` takes it by
> value and `std::move`s it into `std::bind` before `add_queued_action()` (`async.hpp:668`) sees
> `stopped_` and destroys it. By the time `false` comes back there is nothing left to put anywhere.
>
> So what is available is the check and a report, not a recovery. In `take_next_task()`, say on
> stderr that a queued task was refused and is lost — the caller was told it had been accepted, and
> that is now untrue. In `add_task()`, a refusal means this worker is stopped, so the pool is going
> away: answer `false` rather than trying the next worker or queueing a task that no longer exists.
> Neither can loop, because a refusal means the destructor is running and the finish is what ends
> it. It is unreachable today, so say that in a comment and keep the check.

### Step 7 · item 7 — work spawned by an in-flight task is refused once shutdown starts — OPEN
`executor.hpp:67`, `:101-121` · CONFIRMED by test, which had to be written around it

`~executor()` sets `accepting_ = false` before it waits for the drain. A task already running when
that happens is still running — the destructor is waiting for it — but the work it submits is
refused. The pool is waiting for a task and rejecting what that task asks for at the same time.

The test `a_task_can_submit_more_work` has to wait for the inner submission before leaving the
scope, and without that wait it fails with the inner task never running. That wait is the finding.

> Two answers, and the choice is a design decision rather than a bug fix. Either the refusal stands
> and the destructor documents it - a shutdown that accepts new work may never end, which is a real
> argument - or `accepting_` is about *external* callers and a task on a worker thread may still
> submit, with the drain ending when the queue is empty and stays empty. Decide it here and write
> whichever into the destructor's documentation; today the behaviour is neither stated nor chosen.

## Group 3 — placement

### Step 8 · item 8 — the free-worker scan always starts at worker 0 — OPEN
`executor.hpp:95-101` · CONFIRMED by probe: 20 tasks, 1 of 4 workers

```c++
for (std::size_t index = 0; index < workers_.size(); ++index) {
  if (!workers_[index].exec->is_busy()) {
```

The scan restarts at 0 for every submission, so with the queue empty the first idle worker is
always the same one. The probe submitted 20 tasks, each after the last had finished, and every one
of them ran on the same thread:

```
20 sporadic tasks landed on 1 of 4 worker thread(s)
```

The even spread the prototype measured — 400 tasks, 100/100/100/100 — is the *queue* path, which
only runs when every worker is busy. Under a load that never saturates the pool, three of four
workers are decoration.

Not a correctness defect: each task still runs, promptly, on a free worker. It matters for what a
cache holds, for what a profile of the pool looks like, and for anything per-worker a task touches.

> Start the scan where the last one stopped. One `std::size_t` member, incremented past the worker
> that took the task; the placement rule does not change, only which free worker is found first.
> Land it after step 14, which changed what is being scanned — done at `4c1cba6`, so the scan now
> reads `workers_[index]->is_busy()` and this step is unblocked.

### Step 9 · item 9 — the queue is unbounded and nothing pushes back — OPEN, API decision
`executor.hpp:104` · read-only

`pending_.push_back()` always succeeds. A producer faster than the pool grows the deque until the
process runs out of memory, and `add_task()` answers true the whole way down. The only thing a caller
can do about it is read `pending()` and decide for itself.

Recorded, not planned: a capacity is a change to the interface, and the interface is decided. The
argument for taking it anyway is that an unbounded queue is not a smaller interface, it is a policy
- unbounded - chosen silently.

> If this is taken: a capacity given at construction, `add_task()` answering false when it is full,
> and the existing false-means-refused contract already carries it. Zero means unbounded, which
> keeps every current caller.

## Group 4 — what async.hpp imposes

Three findings whose cause is upstream. Two are read-only here by nature: the fix, if any, belongs
in `async.hpp` and lands in that repo's plan, not this one.

### Step 10 · item 10 — the lock order rests on a detail that is not async.hpp's contract — OPEN
`executor.hpp:129-134`, `:171-179` · read-only

The pool takes `mutex_` and then, inside it, each worker's `action_mutex_` — through `add_action()`
and through `is_busy()`. The reverse order would deadlock, and the only thing keeping it from
happening is that `execution::notify_finished()` raises `on_finished` with `action_mutex_`
released. The comment at `:130` says so and is correct.

It is correct about the implementation. It is not quoting a contract: nothing in `async.hpp`'s
documentation of `on_finished` promises which locks are held when it fires, so an upstream change
that raised the callback under the lock would deadlock this pool, in a way no test here would
predict.

> Two halves. Here: name the order — pool first, worker second — in one place rather than in a
> comment inside one function. Upstream: `on_finished` should say that it is raised with the
> execution's own lock released, because that is what makes it usable from a caller that holds a
> lock of its own. The second half is a note to carry to the async plan.

### Step 11 · item 11 — every worker prints to stdout on shutdown — OPEN (upstream)
`async.hpp:757` · CONFIRMED: 14 lines of 38 in one smoke-test run

`loop()` ends with an unconditional `std::println("execution '{}' thread finished", name)`. One
line per worker, per shutdown, on stdout. A five-case smoke run printed 14 of them against 24 lines
of its own output — the pool's own test output is outnumbered by a library's chatter.

This is what is left of the prototype's gap 2. The races are gone; the printing is not, and a pool
multiplies it by the worker count.

> Not this repo's to fix: `async.hpp` should not print at all on a clean shutdown, and the async
> plan is where that belongs. Carry it there. Nothing changes in `executor.hpp`.

### Step 12 · item 12 — the 10ms tick, once per worker — no action
`async.hpp:744-747` · measured

`loop()` waits on its condition variable with a 10ms bound, so an idle worker wakes 100 times a
second and a pool of N does N×100. The prototype measured it: eight idle workers burned 8ms of CPU
over a second of wall clock.

Recorded so it is not rediscovered as a suspicion. It is not a defect and there is nothing here to
do; if a pool is ever wanted at a hundred workers, remeasure before assuming it still holds.

## Group 5 — surface and hygiene

### Step 13 · item 13 — worker names collide between pools — OPEN
`executor.hpp:50` · read-only

```c++
worker_execution::create_instance("pool_worker_" + std::to_string(index))
```

The name is per-index, not per-pool, so two pools in one process both have a `pool_worker_0`. That
name is not decoration: it is what `async.hpp` prints in every warning it raises, including the one
that reports a task that threw. With two pools running, the warning names a worker that could be
either.

> The pool takes an optional name and prefixes its workers with it, defaulting to something unique
> per instance. It costs one constructor parameter and makes step 5's reporting legible; land them
> in that order.

### Step 14 ✅ · item 14 — `struct worker` is a one-field wrapper
`executor.hpp:119-121` as imported; the struct is gone, so the site is historical · read-only

```c++
struct worker {
  std::shared_ptr<worker_execution> exec;
};
```

It held the `busy` flag the pool kept before `is_busy()` existed. The flag is gone and the struct
stayed, so every use reads `workers_[index].exec->` for no benefit.

> `std::vector<std::shared_ptr<worker_execution>>`. The struct is gone and the six
> `workers_[index].exec->` sites read `workers_[index]->`. No behaviour change; the gate was the
> existing suite, 18 of 18 still green. Landed before steps 2 and 8, which rewrite the same lines.
>
> One consequence for step 2: the TSan report recorded there names
> `untangle::executor::worker::~worker()` at frame #9, and that symbol no longer exists — the same
> race comes back pointing at `~shared_ptr` / `_M_destroy`.

### Step 15 · item 15 — copy and move are suppressed by accident — OPEN
`executor.hpp:170` · read-only

`executor` must not be copied or moved: the workers' callbacks capture `this`, so a moved-from pool
leaves every worker calling into the wrong object. Today that is enforced by the `std::mutex`
member, which is neither copyable nor movable, and by the user-declared destructor, which
suppresses the implicit moves. Both are side effects.

A reader has to work out that the pool is pinned, and the compiler's error names `std::mutex`
rather than the reason.

> `= delete` both, with one line saying why: the workers hold `this`. The async plan's step 20 is
> the counter-case worth reading first — there, writing the rule of five out explicitly was tried,
> measured and reverted. This is the opposite situation, a type that must not move, so the deletion
> states an invariant rather than restating a default.

### Step 16 · item 16 — a move-only task does not compile — OPEN, API decision
`executor.hpp:45` · CONFIRMED by compile probe · **restated after step 22**

The default a caller reaches for is `std::function<void(void)>`, which requires a copy-constructible
target. A lambda owning a `std::unique_ptr` is rejected:

```
error: call to implicitly-deleted copy constructor of '(lambda ...)'
```

The caller's way round it is a `shared_ptr` capture, which is an allocation and a refcount to work
around a type choice.

Recorded against the API decision, and it is the same ground as async's step 35 B and C — a
move-only holder in place of `std::function`, which that plan measured and deferred. If it is taken
there it can be taken here; taking it only here would mean the pool's queue and the execution's
queue hold different things.

> Nothing, for now. Revisit with async step 35 B.

### Step 17 · item 17 — `pending()` is advisory and does not say so — OPEN
`executor.hpp:111-114` · read-only

It returns the queue's depth under the lock, which is true at the instant it is read and possibly
false by the time the caller uses it — and it does not count a task handed straight to a worker, so
a pool with every worker occupied reports 0.

Both are the right behaviours. Neither is written down, and "pending" reads like "not yet done".

> One `@remark`: it is the queue's depth, not the pool's occupancy, and it is a reading rather than
> a reservation. Documentation only.

### Step 18 · item 18 — no way to wait for the pool to drain short of destroying it — OPEN, API decision
`executor.hpp:64-79` · read-only

The drain is in the destructor and only there. A caller that wants to know its batch has finished
must destroy the pool and build another, or poll `pending()` — which, per step 17, does not see the
tasks that are actually running.

Recorded against the API decision. The machinery already exists — the destructor's wait is exactly
this, minus the refusal — so the cost is a few lines, and the reason it is not taken is scope
rather than difficulty.

> If taken: `wait_idle()`, the same predicate, no `accepting_ = false`, and the destructor calls
> it. Sequence it after step 7, which decides what "idle" means for a task that submits more work.

### Step 23 ✅ · item 20 — `submit()` does not match the interface it wraps — DONE
`executor.hpp:171` · not a finding; a naming decision taken after step 22

`submit()` is now `add_task()`. The pool is a template on its task type and a thin layer over
`async::execution`, which takes work through `add_action()`; one of the two verbs had to go, and the
pool is the side that should follow. A reader moving between the two headers meets `add_action` and
`add_task` rather than `add_action` and `submit`, and the pair says which is the layer above.

Nothing about the behaviour changed: it still queues a task or hands it straight to a free worker,
still answers `false` once the pool has stopped accepting. It is a rename and the call sites moved
with it — `test/executor_tests.cpp`, `test/executor_smoke_test.cpp` and the README example. Three
case names went with it, because they named the method rather than the behaviour:
`submit_is_refused_once_the_pool_is_shutting_down` and `submit_is_accepted_while_the_pool_is_up`
became `add_task_is_...`, and `a_task_can_submit_more_work` became `a_task_can_add_more_work`.

**The English is not the API.** "submitted", "submitter" and "submission" are left alone wherever
they are prose — a task is still submitted to a pool, and `executor_smoke_test.concurrent_submission`
keeps its name. Only the three places that read as the method were reworded.

**Verified:** 23 of 23 on `debug`, `asan` and `tsan`; clang-format clean; `doc/refman.pdf` at 25
pages. The probe transcript in step 1 still prints `submit() returned true`, and is left as it was
run rather than edited to match.

### Step 24 · item 21 — the workers are unreachable, and so is anything on them — OPEN
`executor.hpp:375` (`workers_`) · read-only · raised 2026-09-22, while wiring step 5

The pool builds its executions in the constructor and keeps them in a private `workers_`. Nothing a
caller holds can reach one, so every seam an `execution` has is reachable only if the pool chooses
to forward it. Step 5 forwards exactly one — `on_error`, and only as far as `on_task_error` —
and that is the whole of it. `is_busy()` per worker, a worker's `name`, `is_running()`, a second
`on_finished`: all present on the object, none reachable.

**This was found by building the wrong thing.** A first attempt at giving callers `execution`'s
`on_error` added a *second* handler on the pool that shadowed the pool's own. That is not access to
the execution's seam; it is a parallel one that happens to pre-empt it, so the pool then had two
members doing one job. It was reverted. The question the attempt was really asking is this step.

> Two shapes, and the choice is about what escapes. A `workers()` accessor hands out the vector and
> everything in it. `for_each_worker(f)` keeps the vector private and hands each execution to a
> callable, which is narrower but still hands out a mutable reference.
>
> **Either way the danger is the same, and it is not the access itself.** The destructor's teardown
> depends on these objects — the poll wait, `stop()`, then `clear()` — and a caller holding a
> reference past the pool's life, or assigning to `on_error` while tasks are running, is a
> use-after-free or a data race respectively. Step 2 is the record of how the first of those goes.
> So: `const` access is cheap and safe and answers the reading half (names, `is_busy()`); mutable
> access is the one that wants a rule about when it may be used, and that rule is "before the first
> `add_task()`", which nothing can enforce.
>
> Not urgent. Nothing has asked for it yet, and step 5's handler covers the case that prompted it.
> Take it when a second seam is actually wanted, and take the `const` half first.

## Group 6 — the suite

### Step 19 — run the suite under both sanitizers — OPEN (both passes run, one platform)
`.github/workflows/ci.yml` · before and after run locally 2026-09-21

The CI job still has never run. Both sanitizers have now been run **locally**, against the
unmodified header, before any fix — which is the half of this step that had to happen first:

| Build | Configure | Result |
|---|---|---|
| `test/build-address` | `-DCMAKE_BUILD_TYPE=Debug -DEXECUTOR_SANITIZE=address` | 18/18 passed, 1.36s, no report |
| `test/build-thread` | `-DCMAKE_BUILD_TYPE=Debug -DEXECUTOR_SANITIZE=thread` | 18/18 passed, 4.32s, no report |

Apple clang 21.0.0, arm64-apple-darwin25.6.0, `halt_on_error=1` set for both. Neither emitted a
single diagnostic. The `EXECUTOR_SANITIZE` plumbing in `test/CMakeLists.txt` works as written, and
`build-*/` is already ignored, so neither tree is a commit.

**Then CI ran, and TSan named step 2.** Same 18 cases, same `EXECUTOR_SANITIZE=thread`, different
platform — Linux, GCC 14, libstdc++ — and the thread job reported a data race in
`executor_smoke_test.concurrent_submission`: `~executor()` freeing an execution on the main thread
against a worker locking that execution's `action_mutex_` through `nothing_running()`. The report
and what it changes are recorded in step 2.

**The local clean sheet was platform luck, not a property of the suite.** This plan briefly held
that the suite could not observe step 2 and that step 20 had to come first to make it reproducible.
That was wrong, and CI is what corrected it: `concurrent_submission` — three workers, 400 tasks from
four submitting threads, destructor at the end of the scope — already opens the window wide enough.
What differs is the platform. Two sanitizer runs of the same source disagreed, so a clean run on one
toolchain says nothing about the other, and "clean under TSan" is only ever a statement about the
configuration that produced it.

**The "after" pass has now run, on macOS only.** Step 2 landed, and the suite — 20 cases, step 20's
stress case among them — comes back clean on both sanitizers through the presets `cfd5aea` added:

| Preset | Result, after step 2 |
|---|---|
| `asan` | 20/20 passed, 5.34s, no report |
| `tsan` | 20/20 passed, 8.61s, no report |

Measured against a named report rather than a clean sheet, which is the whole reason this step said
to run the sanitizers before the fix: `destroying_a_pool_under_load_does_not_race_its_workers`
aborted under `tsan` on this same toolchain before the change.

> **What is left is the platform that complained.** CI — Linux, GCC 14, libstdc++ — has not run
> since step 2, and that is the run that decides it. This repo has already seen the same source come
> back clean on macOS and racy on Linux, so a macOS clean sheet is not the fix being demonstrated;
> it is the weaker of the two runs repeating itself. Until CI is green on the thread job, step 2 is
> closed on evidence from the toolchain that never reported the race. **Then the job itself**: the
> CI workflow still has no sanitizer build, so both passes so far have been run by hand.

### Step 20 ✅ — the suite has no case that runs the pool hard — DONE (`b68cc97`)
`test/executor_tests.cpp:457` · —

Every case is deterministic and short, which is what makes them reportable. None of them puts the
pool under the sustained churn that steps 2 and 3 live in — construction and destruction under
load, many workers, tasks finishing while the destructor runs.

**Amended 2026-09-21.** "None of them" was too strong: `concurrent_submission` reaches step 2 on
Linux under TSan, with three workers and one destruction. What the suite lacked is *repeated*
construction and destruction under load.

**Landed as `destroying_a_pool_under_load_does_not_race_its_workers`**, in step 2's commit rather
than its own, because the case is that fix's evidence: 50 rounds of build a 4-worker pool, submit 64
tasks, leave the scope without waiting. Not waiting is the mechanism — the queue is still backed up
when the destructor starts, so workers are inside `take_next_task()` when it reaches `clear()`. It
aborted under TSan before step 2 and passes after, and it did what this step was for: it made step 2
reproducible on macOS, where the suite had been clean while CI was not.

It is bounded rather than off by default — ~2.8s under TSan, the slowest case in the suite — and it
runs at 4 workers, not above the core count. A probe at 16 workers and 200 rounds was used while
diagnosing step 2 and is not what was committed; those numbers are where to start if a later step
needs a wider window than this case opens.

**The construction half of this step is no longer owed.** It was kept partly for step 3, a pool that
throws while constructing, and step 3 has since been closed as not a defect — member destruction
already stops and waits for every worker started before the throw, and a probe shows it. There is no
remaining finding that repeated *construction* failure would observe.

### Step 21 — README and the reference state behaviour the fixes will change — OPEN
`README.md` · —

The README describes the pool as it is today, including the drain and the refusal. Steps 4, 5, 7
and 13 change what is true. The reference is generated, so it follows the header; the README does
not follow anything.

> Last, once group 1 and group 2 have landed. Rewriting it per step is how it goes stale in the
> middle.

---

## Group 7 — the task type

One decision, taken after the audit closed: the pool is to run any task type, not only the
`std::function<void(void)>` fixed at `:41`. It is recorded as its own group rather than folded into
group 5 because it reverses half of the API decision the rest of this plan is written against, and
because it is not a defect — nothing here is wrong today, it is a surface that was decided narrowly
and is now decided wider.

### Step 22 ✅ · item 19 — `task_t` is fixed at `std::function<void(void)>` — DONE (`9daec8b`)
`executor.hpp:45`, `:51`, `:202` · CONFIRMED by compile probe, and now by two cases in the suite

`executor` is `template <typename taskT>`, with no default — the same shape as
`async::execution<actionT>`, and for the same reason: a pool runs one signature and the caller is
the one who knows which. `untangle::executor pool(4)` no longer compiles; it is
`untangle::executor<std::function<void(void)>> pool(4)`.

**The two spellings are one.** `worker_execution` is `async::execution<taskT>` (`:202`), so the
signature appears once. `task_t` is gone rather than renamed: it only ever named the template
parameter the class did not have.

**Arguments are refused, and the header decided that rather than this step.** The open question was
whether to forward `Args&&...` the way `add_action()` does. It is not available:
`add_action(actionT, Args&&...)` is the only public way into an execution, `add_queued_action()`
being private (`async.hpp:668`), so what the pool can hand a worker is a `taskT` and nothing else. A
`add_task()` that bound arguments would have nowhere to keep them until a worker frees up — the "second
type in the class" this step warned about, now ruled out by the interface instead of by taste. What
it costs is one `static_assert` (`:51`), which turns the failure into a sentence:

```
error: static assertion failed due to requirement 'std::is_invocable_v<std::function<void (int)>>':
executor: the task type must be callable with no arguments
```

**`add_task()` stayed narrow**, taking `taskT`. The templated `add_task()` that would widen the door for
a custom `taskT` is still the separate, smaller change this step described, and is not in `9daec8b`.
It is worth having only if a caller is found who wants a functor pool and lambda submission at once.

**The widened contract is exercised, not just asserted.**
`executor_tests.a_pool_runs_a_task_type_that_is_not_a_std_function` builds a pool on a functor
declaring `result_type = void`, so the claim that any callable naming `result_type` qualifies is now
a passing case rather than a probe. That is also the escape hatch for the portability note above: a
pool on a caller's own functor never touches `std::function::result_type`, the typedef C++20 removed
and both implementations still supply by grace.

**Results were not decided here**, as this step asked. `executor_tests.a_pool_runs_tasks_that_return_a_value`
states the behaviour that exists — a non-void return is run and dropped, because a continuous worker
collects nothing — and the class comment says so in one line. Growing a way to read results is still
steps 5 and 18's to settle together.

**Verified:** 23 of 23 on `debug`, `asan` and `tsan`; clang-format clean; `doc/refman.pdf` at 25
pages. Every pre-existing case is unchanged — both test files alias `executor` to the void-returning
specialisation at the top, the way async's suite aliases `void_execution`.

> Step 16 did not dissolve into this, as predicted: the move-only gap lives in `queued_action_t`
> upstream, and a template parameter on the pool does not reach it.
