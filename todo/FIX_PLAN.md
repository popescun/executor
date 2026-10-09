# executor.hpp — fix plan

**Status (2026-09-21) — group 1 is done: every blocker closed or disproven.** The pool was imported
from `async/prototypes/executor.hpp` as it stood, and this is the audit of what has to change before
it can be called production ready. 19 items, in seven groups — item 19 was added after the audit
closed and is a decision rather than a finding; see group 7. Items 20 to 23 were added the same way
and are steps 23 to 26 — a rename, an open question about reaching the workers, `stop()`, and the
`start()` that should answer it. **Item 24 is step 27, and it is unlike all of them**: not a finding
from the audit and not a decision either, but the first entry here that overturns a step already
closed — step 22 ruled out task arguments on a reading of `async.hpp` that a probe has since
disproven. **Four were called blockers**: each was
read as a way the pool can hang or read freed memory. **None is open.** Steps 1, 2 and 4 were fixed;
step 3 was probed and does not reproduce, so it is closed as not a defect rather than fixed. What is
left is group 2 onwards — what the caller is told, placement, and the surface.
**2026-10-09 — step 28 (group 8) done: `add_task()` contended on the pool's lock.** Found with
`bench/qt_pool_vs_this` and profiled; `mutex_` is now an `untangle::adaptive_mutex`, which spins
before it blocks. Submitting 1000 empty tasks takes 40-60% less time on the main thread; 48 of 48
green on Debug, ASan and TSan; `doc/refman.pdf` at 31 pages.
**2026-10-09 — step 29 opened (group 8): destroying a pool always takes 50 ms.** Found while
checking the benchmark for sleeps, confirmed by a probe; nothing fixed yet.
**2026-10-09 — step 30 superseded, step 31 (group 8) done: per-worker queues.** Step 30's batching
was never committed; step 31 drops `pending_`: the pool picks a worker at submission and calls
`execution::add_task()`, so a task is sealed once and async's API stays as it is (async's steps 51 and
52, `11aa5a8`). On empty batches the executor now submits and delivers ahead of `QThreadPool`; 44 of
44 (and 5 smoke) on Debug, ASan and TSan; `doc/refman.pdf` at 31 pages.
**Tests:** 32 of 32 green on Debug, ASan and TSan, 2026-09-22; clang-format clean;
doxygen clean, `doc/refman.pdf` at 25 pages (was 19 at the import), rebuilt with
`tools/make_doc.sh`. **Steps 2 and 4 are closed** (`b68cc97`, `032da65`). The destructor waits on an
`async::execution_poll` until every worker has left its thread and clears them only then, and it
reports on stderr while either of its waits is stalled rather than parking in silence. The race TSan
named on CI no longer reproduces: 32 of 32 under TSan and under ASan on macOS/libc++, where the case
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
any task type, as of `9daec8b`, and any task type means its arguments too, as of step 27. That was
item 19, group 7, and what it pulls along with it — what a non-void return gives back — is still
items 5 and 18's to settle.

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

**Step 27 is done, and it is the first step that reopened a closed one.** Group 7 had ruled task
arguments out as unavailable, on a reading of `async.hpp` rather than on a probe; the reading was
wrong, and the two cases in the suite now run a pool on `std::function<int(int)>` from the free-worker
path and from the queue. What that says about method is the point of recording it: step 22's ruling
was the one conclusion in this plan reached by reading an interface instead of compiling against it,
and it is the one that had to be taken back. A probe costs less than the retraction does.

**NEXT: step 9, and it is the last of group 3.** Group 2 is done — 5 through async's step 38
rather than here, 6 once step 25 made its state reachable, 7 decided rather than fixed — and step 8
is closed by declining it: the reuse it called a defect is the behaviour worth having. Step 9 is the
unbounded queue, recorded against the API decision rather than planned, so it is a decision too
rather than a fix. Groups 1 and 7 are done, and step 22
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
| `5ff377b` | 27 — a task's arguments bound at the door, so the pool runs any signature |
| `11d6b12` | 21 — the README caught up with steps 4, 5, 7, and told about 13 |

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
| 6 ✅ | 6 | `add_action()`'s answer is dropped, so a refused task vanishes | `:163`, `:260`, `:278` | CONFIRMED (test) — fixed |
| 7 ✅ | 7 | work spawned by an in-flight task is refused once shutdown starts | `:85`, `:156`, `:205` | decided, documented, tested |
| **Group 3 — placement** |
| 8 ✅ | 8 | the free-worker scan always starts at worker 0 | `:152`, `:169` | CONFIRMED (probe) — declined, the reuse is intended |
| 9 | 9 | the queue is unbounded and nothing pushes back | `:119` | read-only, API decision |
| **Group 4 — what async.hpp imposes** |
| 10 | 10 | the lock order rests on an async.hpp detail that is not its contract | `:140-145`, `:171-179` | read-only |
| 11 ✅ | 11 | every worker prints to stdout on shutdown | `async.hpp:757` | CONFIRMED (14 lines of 38) — fixed upstream, async `2a06497` |
| 12 ✅ | 12 | the 10ms tick, once per worker | `async.hpp:744-747` | measured, not a defect — gone anyway, async `502650b` |
| **Group 5 — surface and hygiene** |
| 13 | 13 | worker names collide between pools | `:65` | read-only |
| 14 ✅ | 14 | `struct worker` is a one-field wrapper the prototype outgrew | `:119-121` | read-only |
| 15 | 15 | copy and move are suppressed by accident, not by statement | `:181` | read-only |
| 16 | 16 | a move-only task does not compile | `:41` | CONFIRMED (compile probe) |
| 17 | 17 | `pending()` is advisory and does not say so | `:126-129` | read-only |
| 18 | 18 | no way to wait for the pool to drain short of destroying it | `:79-94` | read-only, API decision |
| 23 ✅ | 20 | `submit()` does not match `execution::add_action()` | `:171` | naming decision — renamed `add_task()` |
| 24 | 21 | the workers are unreachable, and so is every seam on them | `:375` | read-only, surface decision |
| 25 ✅ | 22 | a pool cannot be stopped without destroying it | `:226` | surface addition — `stop()` |
| 26 ✅ | 23 | construction starts the workers, and nothing else can | `:187`, `:205`, `:89-99` | CONFIRMED (tests) — `start()` added |
| **Group 7 — the task type** |
| 22 ✅ | 19 | `task_t` is fixed at `std::function<void(void)>` | `:45`, `:51`, `:202` | CONFIRMED (probe, tests) — fixed `9daec8b` |
| 27 ✅ | 24 | a task type that takes arguments is refused, and step 22 ruled that unfixable | `:42`, `:265`, `:371` | CONFIRMED (tests, probe) — fixed (`5ff377b`) |
| **Group 6 — the suite** |
| 19 | — | the sanitizers have never been run against the suite | `.github/workflows/ci.yml` | — |
| 20 ✅ | — | the suite has no case that runs the pool hard | `test/executor_tests.cpp:457`, `:1008` | extended 2026-09-25 for the task door |
| 21 ✅ | — | README and the reference state behaviour the fixes will change | `README.md` | audited and written 2026-09-25 — **DONE** (`11d6b12`) |
| **Group 8 — cost** |
| 28 ✅ | 25 | `add_task()` keeps the submitter waiting on the pool's lock | `:345-364`, `:380-399`, `:459` | CONFIRMED (profile, `bench/qt_pool_vs_this`) — fixed `e9ef8ea` |
| 29 | 26 | destroying a pool always takes 50 ms | `:159-161`, `:504` | CONFIRMED (probe) |
| 30 | 27 | each task reaches its worker alone, wrapped twice | `:396-414`, `:422-426`, `:431-450`, `:457-465` | CONFIRMED (profile, scratchpad variants) — SUPERSEDED by step 31, never committed |
| 31 ✅ | 28 | the pool keeps a queue of its own between the door and the workers | `:197-237`, `:258-284`, `:286-289`, `:387-465`, `:521` | CONFIRMED (prototype, scratchpad) — fixed (uncommitted) |

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

### Step 6 ✅ · item 6 — `add_action()`'s answer is dropped — DONE
`executor.hpp:301` (the return), `:199` (`add_task()`), `:325` (`take_next_task()`) · CONFIRMED by
test: `a_task_refused_by_a_worker_is_not_reported_as_taken`

`give_to_worker()` returns what `add_action()` answered, and both callers read it. It is
`[[nodiscard]]`, so dropping it again is a compile error rather than a regression.

**The recovery this step asked for cannot be written, and that is the finding.** "A refusal puts the
task back at the front of `pending_`" assumes the task still exists when the answer arrives. It does
not: `add_action(actionT action, ...)` takes it by value and moves it into `std::bind` before
`add_queued_action()` (`async.hpp:668`) looks at `stopped_` and destroys it. By the time `false`
comes back there is nothing left to put anywhere. So each caller does what it can:

- **`add_task()` answers `false`.** Not "try the next worker": a worker refuses only once stopped,
  and a stopped worker is one `stop()` stopped, so the others are stopped too.
- **`take_next_task()` reports.** The task is gone and its submitter was told it had been taken, so
  the loss is named on stderr rather than passed over.

**It stopped being unreachable while this step was open.** The plan said it could not happen today,
and that was true: a worker refuses only once stopped, and only the destructor stopped one, after
the queue was empty. Step 25 made `stop()` public, which is what turned an invariant held by the
order of two functions into a state a caller can ask for. The case is the proof — before the fix,
five tasks into a stopped one-worker pool were all accepted, `pending()` said 0, and none ran.

Verified after: the same five return `false`. **25 of 25** on `debug`, `asan` and `tsan`;
clang-format clean; `doc/refman.pdf` at 25 pages. README now says both refusal reasons.

> **async warns on a refusal, and it does not come through `on_error`.**
> `add_queued_action()` prints `warning: execution '...' is stopped, action not added` directly.
> Step 5's handler is for throws only, so a caller that has taken over error reporting still does
> not hear about a refused action. Recorded here rather than fixed: it is async's warning, and
> whether refusal belongs on the same seam as a throw is that repo's question.

### Step 7 ✅ · item 7 — work spawned by an in-flight task is refused once shutdown starts — DONE
`executor.hpp:85` (the destructor), `:205` (`stop()`), `:156` (`add_task()`) · decided, documented
and tested; no behaviour changed

**The refusal stands, and is now chosen rather than incidental.** A task still running while the
pool shuts down is refused if it adds more work, even though the destructor is waiting for that very
task. The alternative — letting in-flight work through, with the finish ending once the queue is
empty and stays empty — was rejected: a task that re-adds itself would keep a shutdown from ever
ending, and telling a worker-thread caller from an external one needs thread identity the pool does
not track.

**One rule, and step 26 is why it is one.** `running_` is a single flag, cleared by `stop()` and by
the destructor alike, so "a pool that is not running takes no work" holds whoever is asking. The
rejected answer would have had to reintroduce a special case for who is calling, right after step 26
removed the last one.

**One argument for the refusal has got weaker, and is recorded as such.** This step said a shutdown
that accepted new work "may never end", as though silently. Since step 4 it does not: a stalled
finish reports itself on stderr every second and names the queue depth. The unbounded hazard remains;
the silence does not.

**What changed is documentation.** The destructor carries an `@attention` saying the refusal is
deliberate and why, `stop()` says it holds for a running task too, `add_task()`'s `@return` lists all
four ways it answers false, and the README says the same in a paragraph of its own. The code is
untouched.

**Cases.** `add_task_is_refused_once_the_pool_is_shutting_down` already covered the destructor.
`a_running_task_cannot_add_work_once_the_pool_stops` is new and covers `stop()`, which step 26 made a
second way to reach this: a gated task is released after the pool is stopped, and its `add_task()`
answers false. It passed on its first run, which is the shape of this step — nothing was broken, it
had simply never been stated.

**Verified:** 30 of 30 on `debug`, `asan` and `tsan`; clang-format clean.

## Group 3 — placement

### Step 8 ✅ · item 8 — the free-worker scan always starts at worker 0 — CLOSED, DECLINED
`executor.hpp:152` (the remark), `:169` (the scan) · CONFIRMED by probe, and kept anyway

The scan restarts at 0 on every call, so a pool that is not busy hands every task to the same
worker. A probe measures both load shapes on one 4-worker pool:

```
400 tasks, added as fast as possible -> 4 worker(s): 110 106 86 98
 20 tasks, each awaited before the next -> 1 worker(s): 20
```

The even spread is the *queue* path, which runs only when every worker is busy; the one-worker
result is the hand-to-a-free-worker path, and it is what this step was opened about.

**The rotation this step asked for was written, measured, and declined.** The case that shows it —
20 sporadic tasks landing on 1 of 4 workers — was written and passed as a red test, and then
dropped. What broke the step was reading its own harms back:

- *"what a cache holds"* argues the other way. Reusing the worker that just ran keeps one stack and
  one set of cache lines warm; rotating cools four and pays a wake-up and a migration per task.
- *"what a profile looks like"* is cosmetic. Three idle threads under light load is an accurate
  picture of a pool that is not busy.
- *"anything per-worker a task touches"* is real in principle and empty in fact: nothing here keeps
  per-worker state, and a caller cannot reach a worker at all (step 24).

The step's own first line was the giveaway — *"not a correctness defect: each task still runs,
promptly, on a free worker"*. Pools that care about locality prefer the thread that just ran for
exactly this reason.

**Where rotation would win** is narrower than the step implies: tasks long enough to overlap, arriving
just far enough apart that worker 0 is free each time, so one worker does everything while three
idle and the queue path never engages. Real, specific, and handled correctly today — unevenly, but
correctly.

**What landed is the decision, on `add_task()`**: the first free worker takes it, counting from the
start, because the worker that just ran is the warm one. Said there so it is not rediscovered as a
bug.

> Reopen it if a caller appears who needs the spread — per-worker connections, or anything else
> pinned to a thread. That caller would need step 24 first, to reach a worker at all.

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

### Step 11 ✅ · item 11 — every worker prints to stdout on shutdown — DONE upstream (async `2a06497`)
`async.hpp:757` · CONFIRMED: 14 lines of 38 in one smoke-test run

`loop()` ends with an unconditional `std::println("execution '{}' thread finished", name)`. One
line per worker, per shutdown, on stdout. A five-case smoke run printed 14 of them against 24 lines
of its own output — the pool's own test output is outnumbered by a library's chatter.

This is what is left of the prototype's gap 2. The races are gone; the printing is not, and a pool
multiplies it by the worker count.

> Not this repo's to fix: `async.hpp` should not print at all on a clean shutdown, and the async
> plan is where that belongs. Carry it there. Nothing changes in `executor.hpp`.
>
> Done upstream 2026-10-05: async `2a06497` deletes both per-thread prints, so a worker's thread
> ends silently; warnings about dropped work stay on stderr. Reached here through the async pin.

### Step 12 · item 12 — the 10ms tick, once per worker — no action
`async.hpp:744-747` · measured

`loop()` waits on its condition variable with a 10ms bound, so an idle worker wakes 100 times a
second and a pool of N does N×100. The prototype measured it: eight idle workers burned 8ms of CPU
over a second of wall clock.

Recorded so it is not rediscovered as a suspicion. It is not a defect and there is nothing here to
do; if a pool is ever wanted at a hundred workers, remeasure before assuming it still holds.

> Gone anyway, 2026-10-05: async `502650b` (fluxcpp plan step 6) blocks an idle worker that has
> nothing attached instead of polling, so an idle pool no longer wakes at all. The 10 ms bound now
> applies only while an execution has something attached.

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

### Step 25 ✅ · item 22 — a pool cannot be stopped without destroying it — DONE
`executor.hpp:226` · not a finding; a surface addition, and step 6's prerequisite

`stop()` is public. It stops every worker: what they are running they finish, what is still queued
stays there unrun, and a task added afterwards is handed to a worker that refuses it. The destructor
calls it rather than keeping its own loop, after the wait that leaves the workers nothing to do —
which is the difference between stopping a pool and finishing one.

**It was added for step 6, and that is worth being straight about.** A worker refuses only once it
is stopped, and the pool stopped its own only in the destructor, after the queue was empty and
nothing was left to hand over. So the refusal step 6 is about could not be reached through the
pool's own surface at all: the case for it was red on a missing accessor rather than on the defect,
and the only evidence was a probe against a doctored header. `stop()` makes the state a caller can
ask for, so step 6's case now fails on what it is actually about.

It is not only a test fixture, though — a pool that can only be stopped by being destroyed is a real
gap, and step 18 is the neighbouring one, wanting a way to wait for a pool to finish short of
destroying it.

> **It does not wait**, and the doc comment says so. Only the destructor waits for the workers to
> leave their threads, and that wait is what makes destruction safe; a `stop()` that waited would
> read as though it made the pool safe to destroy, which it does not. Whether a caller wants a
> waiting form is step 18's question, not this one's.

### Step 26 ✅ · item 23 — construction starts the workers, and nothing else can — DONE
`executor.hpp:187` (`start()`), `:205` (`stop()`), `:89-99` (the destructor) · CONFIRMED by four
cases

`start()` is public and the constructor no longer starts anything: it builds and wires the workers,
`start()` runs them. `stop()` has an opposite, a stopped pool can be restarted, and a caller has a
moment — before `start()` — in which to assign `on_task_error` with nothing running to race.

**A pool refuses work until it is started.** The alternative was allowed by the layer below —
`execution` documents that actions may be queued before `start()` — but it does not survive the
destructor here: `is_busy()` is true for a worker holding a queued action, so tasks sitting on
unstarted workers make `nothing_running()` false forever and the finish wait never returns. Refusing
keeps that state unreachable, and it is step 1's rule again: a pool that cannot run a task must not
take it.

**Two things the cases caught that the instruction did not foresee.**

*`stop()` has to stop the pool accepting, not just the workers.* The restart case found
`add_task()` answering **true** after `stop()`: the worker was still finishing, so no free worker was
found, and the task fell through to `pending_` — then was lost when a worker refused it. A pool that
can run nothing must accept nothing, so `stop()` clears `running_` first and stops the workers
after.

*The destructor cannot wait for a queue that no longer drains.* With the above in place, `stop()`
with work still queued **hung**: `pending_` only shrinks through `take_next_task()`, which only
`on_finished` calls, which a stopped worker no longer raises. So the wait is conditional on whether
the pool was still running, and what is left is reported rather than dropped in silence:

```c++
const bool was_running = running_.exchange(false);
...
return nothing_running() && (pending_.empty() || !was_running);
```

**The simpler predicate was tried and does not hold.** `nothing_running() && pending_.empty()` looks
sufficient — `stop()` clears `running_` before stopping the workers, so nothing new arrives — but it
hangs `a_queued_task_a_worker_refuses_is_reported`. Stopping prevents new tasks; it does nothing
about the ones already queued, and `nothing_running()` becomes true while `pending_.empty()` never
does. Measured, not reasoned: the case times out.

Dropping the queue inside `stop()` would make that predicate work, and was not taken: a stopped pool
that is started again drains what it was holding, because its workers raise `on_finished` once more.
Keeping the queue is what makes a restart mean something.

**`running_` is a `std::atomic_bool`, read without the mutex.** `start()` and `stop()` set it with no
lock at all, and `add_task()` checks it before taking one, so a refusal costs nothing. The
destructor reads-and-clears it in one `exchange()` rather than two steps that relied on the mutex to
be indivisible. `mutex_` guards `pending_` and `workers_`, which is what it was for.

> **A refusal can now race a `stop()`**, where the mutex used to make that impossible: `add_task()`
> checks `running_` outside the lock, so a `stop()` landing before `give_to_worker()` leaves the task
> with a worker that has just stopped. It is refused and `add_task()` answers false — step 6's guard
> doing what it exists for, and the right answer either way.

**Cases:** `an_unstarted_pool_refuses_work`, `an_unstarted_pool_shuts_down`,
`a_started_pool_runs_what_it_is_given`, `a_stopped_pool_can_be_started_again`. Every other case in
the suite gained a `start()`, as did the smoke test and the README example.

**One repair.** Step 6's case passed after `stop()` stopped accepting, but vacuously: `add_task()`
returned false without reaching the guard it was written for. It is now
`a_queued_task_a_worker_refuses_is_reported`, which fills the queue behind a gated worker, stops the
pool, and reads the stderr line as the queued task is refused — the path that is still reachable.

**Verified:** 29 of 29 on `debug`, `asan` and `tsan`; clang-format clean.

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

### Step 20 ✅ — the suite has no case that runs the pool hard — DONE (`b68cc97`, extended 2026-09-25)
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

**Extended 2026-09-25, for the second door.** The tasks feature gave the pool an `add_task()`
beside `add_action()`, both feeding one queue, so this case covered half of what it used to cover
whole. **`destroying_a_pool_with_tasks_queued_does_not_race_their_callbacks`** is the same shape
through the other door: 50 rounds, 4 workers, 64 tasks each, scope left without waiting — and now
every task is also notifying a callback on its own thread while the destructor runs, several at
once.

**It asserts three things, and the two extra ones are why it exists:**

| Assertion | Catches |
|---|---|
| `ran == rounds * tasks_per_round` | what the older case catches |
| `notified == ran` | a dropped notification — a task that ran and never called back is invisible to a count of runs |
| `mismatched == 0` | crossed wiring: each task is bound to its index and each callback checks it got `i * 2`, so a callback paired with another task's result fails loudly |

The third is the one a shared queue running several tasks at once could actually produce, and
nothing else in the suite would see it.

**TSan, macOS/libc++, 2026-09-25: 33 of 33, zero warnings, zero data races**, suite at 10.4s. That
run is also what exposed the gap — the feature's own five cases are single- or dual-worker and none
of them destroys a pool with tasks still queued, so the new notification path had never been under
churn. **The gap was found after the feature plan closed**, which is the argument for running the
sanitizer as a step rather than as an afterthought; see step 19, still open for the Linux/libstdc++
half.

**The construction half of this step is no longer owed.** It was kept partly for step 3, a pool that
throws while constructing, and step 3 has since been closed as not a defect — member destruction
already stops and waits for every worker started before the throw, and a probe shows it. There is no
remaining finding that repeated *construction* failure would observe.

### Step 21 ✅ — README and the reference state behaviour the fixes will change — DONE (`11d6b12`)
`README.md` · audited 2026-09-25

The README describes the pool as it is today, including the drain and the refusal. Steps 4, 5, 7
and 13 change what is true. The reference is generated, so it follows the header; the README does
not follow anything.

> Last, once group 1 and group 2 have landed. Rewriting it per step is how it goes stale in the
> middle.

**Audited 2026-09-25**, against each of the four steps named above. The tasks feature rewrote the
same file for its own reasons, which is what prompted the check — a different question about the
same file, and not an answer to this one.

| Step | README | |
|---|---|---|
| 5 — a task that throws reaches the caller | **current** | *lifetime and failure* carries `on_task_error` in full: the example, why it is a `std::exception_ptr`, the worker-thread warning, and the stderr fallback |
| 7 — work spawned by an in-flight task is refused | **current** | "A task that is still running is refused too", with the reasoning rather than only the rule |
| 4 — the destructor waits, and reports while it waits | **now current** | it was the one real gap: the README said it waits and stopped there, omitting the reports, which are step 4's actual deliverable. *lifetime and failure* now carries both, with sample output, the 1s-doubling-to-30s interval, why the two waits are kept apart, and why bounding either was never available |
| 13 — worker names collide between pools | **unfixed, and now said so** | the README leaned on those names — "the pool warns on stderr instead, **naming the worker**" — while pointing a reader with two pools at an ambiguous identifier. *lifetime and failure* now states the collision outright, names the reports it makes ambiguous, and gives the one thing a caller can do about it today: assign `on_task_error`, which replaces the warning with a handler that is the pool's own |

**Both paragraphs are written, and this step is done.** Every one of the four it named is now
stated where a reader meets it. Step 13 is documented rather than fixed — deliberately: a caveat
costs a paragraph and helps today, while the fix costs a constructor parameter and is that step's
own to make. When it lands, this paragraph is what it deletes.

**One stale claim was found and corrected while auditing**: "a task with something to report
carries its own channel" was written when the pool had no such channel. It now points at
`add_task()`. That line was missed by the tasks feature's own documentation pass, which rewrote the
sections around it — a reminder that a file rewritten for one reason does not get audited for the
others by accident.

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

**Arguments are refused, and the header decided that rather than this step.** ⚠️ **Overturned by
step 27** — read the paragraph below as the record of a wrong conclusion, not as the surface. The
open question was
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

> **What step 27 found in that paragraph.** Every sentence about `async.hpp` in it is true:
> `add_action()` is the only public door, and `add_queued_action()` is private. The conclusion does
> not follow from them. `add_action(actionT, Args&&...)` binds its arguments on the spot and queues
> the *bound, nullary* form (`async.hpp:496`, into `queued_action_t` at `:660`) — so arguments do
> not survive into async's queue either, and "nowhere to keep them" describes async as much as the
> pool. What the pool could not do, it could not do because of its own `:202` (`:265` by the time
> step 27 reached it) — one line choosing
> `execution<taskT>`, which is what made `add_action()` reachable only with an empty pack. The step
> read an interface where it should have compiled against one.

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

### Step 27 ✅ · item 24 — a task type that takes arguments is refused — DONE
`executor.hpp:42` (the assert), `:265` (`worker_execution`), `:371` (`pending_`) · CONFIRMED by two
cases and a compile probe · raised 2026-09-22, after step 22 had been closed for a day

`executor<std::function<int(int)>>` did not compile. `static_assert(std::is_invocable_v<taskT>)` at
`:42` refused any task type with an argument list, and step 22 had recorded that refusal as
permanent — see the ⚠️ above, which is where this step came from.

**What the probe showed.** Deleting the assert and compiling moves the error one level down, to
`async.hpp:496`: `no viable conversion from '__bind<std::function<int (int)>>' to 'queued_action_t'`.
That is `add_action()` doing `std::bind(action)` with an empty pack — a bind expression that calls
its target with no arguments, against a target needing one. The requirement was never async's
contract. It was manufactured by `:265`, `using worker_execution = async::execution<taskT>`, which
made the caller's task type and the worker's action type the same type and so left `add_action()`'s
own `Args&&...` unreachable. A second probe confirmed the way out from the other side: an
`execution<std::function<int(void)>>` accepts `std::bind(task, 21)` through the public door, with a
`std::function<int(int)>` as the task. `add_queued_action()` being private costs nothing once the
worker's action type is already nullary.

**The fix separates the two types that `:265` had made one.** `taskT` stays the caller's signature,
arguments and result both. What the pool queues and hands a worker is new — `task_call`, a task with
its arguments already bound — and `worker_execution` is `execution<task_call>`. `pending_` holds
`task_call`, `add_task()` is `template <typename... Args> bool add_task(taskT, Args&&...)`, and the
binding happens at the door where the caller still holds the arguments. This is async's own shape,
one level up: `actionT` public, `queued_action_t` internal. The "second type in the class" step 22
warned against is exactly that, and it was never the smell the step took it for.

**Four decisions inside the fix, each of which could have gone the other way:**

- **`task_call` is a struct, not `using task_call = std::function<result_type(void)>`.** The alias is
  shorter, but then async reads `result_type` off a `std::function` — the typedef C++20 removed and
  both implementations still supply by grace. Step 22 went out of its way to keep the functor pool
  clear of it, and the alias would have quietly surrendered that for *every* pool, functor ones
  included. The struct names its own `result_type` from `taskT::result_type`, so nothing changes.
- **Binding is a capture pack, not `std::bind`.** async uses both — `add_action()` binds (`:496`),
  `bind_action_and_method()` captures a pack (`:336`) — and the pack avoids `std::bind`'s
  placeholder and nested-bind quirks. The lambda is `mutable`, which is what preserves calling the
  task as a non-const lvalue; a non-mutable capture would have broken a functor whose `operator()`
  is not `const`.
- **`bind_task()` short-circuits on an empty pack.** Without it every existing zero-argument pool
  would pay for a lambda layer it has no arguments to put in.
- **The assert is per call, and asks about the call that happens.**
  `is_invocable_v<taskT, std::decay_t<Args>&...>`, not `Args...`: the task runs later, from the
  pool's copies, as lvalues. Checking `Args...` would admit `add_task(f, std::move(x))` against a
  task taking `int&&` and then fail inside the binder with a worse message than the assert gives.

**What it costs.** A queued task now crosses two `std::function` layers rather than one: `task_call`
holds the bound callable, and `add_action()` wraps it again on the way in. The second wrap is the
one async always did; the first is new, and unavoidable while `add_queued_action()` is private.
Zero-argument pools do not pay it, by the short-circuit above. Not measured — if it ever matters the
lever is upstream, not here.

**Tested, not just asserted.** `executor_tests.a_pool_runs_a_task_type_that_takes_arguments_and_returns_a_value`
runs 50 tasks on a pool of 4 with every worker free, so each goes straight to one;
`executor_tests.an_argument_survives_the_queue` holds the single worker at the gate so all 50 wait in
`pending_` with their arguments and run from there, in order. The second is the one aimed at step
22's sentence about having "nowhere to keep them". Both were written first and reviewed red — the
red being a build failure rather than a failing assertion, since the defect was a refusal to compile.

**Verified:** 27 of 27 in `executor_tests` and 5 of 5 in `executor_smoke_test`, on `debug`, `asan`
and `tsan`; clang-format clean; `doc/refman.pdf` rebuilt with `tools/make_doc.sh`, 25 pages. Every
pre-existing case is unchanged and none needed touching: an `add_task()` call with no arguments
deduces an empty pack and takes the short-circuit.

> **The method note this step is really about.** Step 22's ruling is the only conclusion in this plan
> that was reached by reading an interface rather than compiling against one, and it is the only one
> that has had to be taken back. Everything else closed here carries a probe, a test, or a sanitizer
> run. `9daec8b` was not wrong about what it did; it was wrong about what it said could not be done.

## Group 8 — cost

### Step 28 ✅ · item 25 — `add_task()` keeps the submitter waiting on the pool's lock — DONE
`executor.hpp:345-364` (`queue_task`), `:380-399` (`take_next_task`), `:459` (`mutex_`) ·
CONFIRMED by profile, 2026-10-09

**Found by** `bench/qt_pool_vs_this`, against Qt's thread pool: with empty tasks the main thread spent
1.5-5x as long submitting as with `QThreadPool`. The cost is in submitting, not running.

| 1000 empty tasks, `submitted` column, median µs (two runs) | executor | QThreadPool |
|---|---|---|
| 1 worker | 467.9, 114.2 | 100.0, 74.0 |
| 4 workers | 524.1, 473.3 | 90.5, 87.8 |

**Reproduced without Qt** by a scratchpad harness (1000 empty tasks submitted, then waited for):
1/2/4 workers took 118/227/386 µs, so the cost grows with the worker count.

**Profile** (`sample` on that harness, 4 workers): of the submitting thread's time, 62% waits for `mutex_` in the
kernel (`__psynch_mutexwait`), 19% releases it to a waiting worker (`__psynch_mutexdrop`), 10% wakes a
worker (`notify_one`); the task itself is the rest. The workers wait on the same lock in
`take_next_task()`, which every worker takes after every batch, queued work or not. `std::mutex` on
macOS blocks at once instead of spinning first, so every meeting is a pair of syscalls.

**Measured on a scratchpad copy** (4 workers, median µs): spinning ~100 tries before blocking on
`mutex_` 388 -> 260; skipping `nothing_running()` unless `wait()` waits 388 -> 357; executor-owned busy
flags instead of locking each worker 388 -> 345; spin + flags 236. Reaching `QThreadPool`'s ~90 needs
a shared queue the workers pull from - a redesign, not this step.

**Decided (user, 2026-10-09): spin, then block** - option 1 of four. A lock of the pool's own,
`untangle::adaptive_mutex`, replaces `std::mutex` for `mutex_`: `lock()` tries a bounded number of
times with a CPU pause between tries, then blocks. `finished_cv_` becomes `std::condition_variable_any`
to wait on it. Nothing else changes: same lock order, same critical sections.

**Tests (written first, fail to compile until the type exists):** `adaptive_mutex_tests` in
`test/executor_tests.cpp` - exclusion under contention, `try_lock()` never waits, `lock()` waits out a
holder longer than the spin, and `std::condition_variable_any` waits on it. The rest of the suite
covers the pool with the new lock (`wait()`, the destructor, the hard-run cases of step 20).

**Done when:** the suite is green on Debug, ASan and TSan, and `bench/qt_pool_vs_this`, run again,
shows the executor's `submitted` time for empty batches down by about the drop measured above.

**Landed 2026-10-09.** `untangle::adaptive_mutex` in `executor.hpp`: `lock()` tries 100 times with a
CPU pause (`pause` on x86, `yield` on ARM) before it blocks. `mutex_` uses it and `finished_cv_` is a
`std::condition_variable_any`; nothing else changed. 48 of 48 on Debug, ASan and TSan (44 + the 4
new cases). `bench/qt_pool_vs_this` again, two runs, `submitted` for 1000 empty tasks, median µs:

| | executor before | executor after | QThreadPool |
|---|---|---|---|
| 1 worker | 467.9, 114.2 | 85.5, 59.3 | 99.9, 75.7 |
| 4 workers | 524.1, 473.3 | 205.5, 185.2 | 75.4, 78.7 |

With one worker - flux's default - the executor now submits as fast as `QThreadPool` or faster;
with four it still takes about 2.5x as long, which is what is left for a shared-queue design. The
`delivered` times move less (4 workers: 604 -> 480-487 µs), and with real work nothing changed:
the cost was always the submit, and the work hides it.

### Step 29 · item 26 — destroying a pool always takes 50 ms — OPEN
`executor.hpp:159-161` (the poll in `~executor()`), `:504` (`poll_interval_ms`) · CONFIRMED by
probe, 2026-10-09

**Found while** checking whether `bench/qt_pool_vs_this` timed any sleep. It does not: the pools are
built and destroyed outside the timed runs, idle workers block on a condition variable, and
`wait()` waits on `finished_cv_`. The benchmark is unaffected.

**What happens:** after `wait()` and `stop()`, `~executor()` polls `poll_.is_running()` until every
worker has left its thread, sleeping `poll_interval_ms` (50) between checks. A worker cannot leave
in the instant between `stop()` and the first check, so that check always fails and the destructor
always sleeps one full tick.

**Probe** (scratchpad, Release, 20 destructions per case):

| workers | pool when destroyed | `~executor()`, ms |
|---|---|---|
| 1 | idle | 53.4 (50.1–55.0) |
| 1 | 100 tasks just added | 53.7 (50.3–55.1) |
| 4 | idle | 54.3 (50.1–55.0) |
| 4 | 100 tasks just added | 53.3 (50.3–55.1) |

Never under 50 ms, whatever the pool held.

**Why it matters:** flux gives every store its own pool, so an app that destroys its stores one after
another pays about 50 ms per store on exit, and so does any pool destroyed while the app runs.
`~execution()` polls the same way at 1 ms (`async.hpp:281-284`), which is small enough not to matter.

**To decide:** have the worker signal that it left - a condition variable notified as its last act,
which `~executor()` waits on with the same report interval - or keep the poll with a short first
tick (1 ms, doubling to 50). The second is local to `executor.hpp`; the first reaches into
`async::execution_poll` and has to respect the rule that `running_` is the last thing a worker
touches.

### Step 30 · item 27 — each task reaches its worker alone, wrapped twice — SUPERSEDED by step 31
`executor.hpp:396-414` (`queue_task`), `:422-426` (`give_to_worker`), `:431-450` (`take_next_task`),
`:457-465` (`nothing_running`); `async.hpp:796` (`add_queued_task`, private) · CONFIRMED by profile
and by scratchpad variants, 2026-10-09

**Why the pool still trails `QThreadPool` after step 28.** `bench/qt_pool_vs_this` is fair - both
pools allocate a task per submit and post each result the same way - and the gap shows only on
batches of empty tasks, where nothing hides the pool's own overhead. Per task:

1. The submitter hands it to an idle worker, or appends it to `pending_`.
2. `give_to_worker()` adds it through `execution::add_action()`, which wraps the `task_t` in a second
   `std::function` (`std::bind`) and stores that with two list nodes (`owned`, `actions`): three
   allocations on top of the task's own.
3. The worker wakes, moves its actuator into a one-task batch, runs it, and frees all four - memory
   another thread allocated.
4. `on_finished` -> `take_next_task()` takes `mutex_` and hands the worker **one** queued task,
   then `nothing_running()` locks every worker's `action_mutex_`.

**Profile** (`sample`, 1 worker, 1000 empty tasks in a loop): the worker spends 27% re-wrapping
and re-adding tasks to itself, 33% destroying batches, ~12% on `mutex_`, ~10% on the rest of
`execute_actions()` - and ~2% running the tasks. With one worker it hardly sleeps: the submitter
outruns it, so `pending_` fills and every task takes the one-at-a-time path.

**Measured** (scratchpad copies of `executor.hpp` and `async.hpp`, a Qt-free harness: 1000 empty
tasks submitted, then waited for; median µs, submit / all done, two runs):

| variant | 1 worker | 4 workers |
|---|---|---|
| current (`e9ef8ea`) | 76-80 / 139-141 | 258-286 / 285-324 |
| **A** - `give_to_worker()` uses `add_queued_task()`: no bind, one list node | 97-104 / 135-138 | 228-263 / 254-296 |
| **B** - A, and `take_next_task()` hands over `ceil(pending / workers)` tasks per trip | 71-72 / 73 | 217-222 / 228-233 |
| **C** - B, executor-owned busy flags (under `mutex_`) replace `is_busy()` in `queue_task()` and `nothing_running()`, which runs only while a `wait()` waits | 66-68 / 71-73 | 208-211 / 216-221 |

Handing everything pending to one worker (instead of a share) measured the same at 1 worker and
slightly worse at 4.

**Proposed: C.** It halves the 1-worker time to the last task (140 -> 72 µs, about as fast as the
submitter can add them) and cuts 4 workers by about a quarter.
- `async`: `add_queued_task()` public, or a public twin taking a sealed `task_t` - the executor
  already holds one, from `bind_task()`. An `async` change first, then the pin bump here.
- `executor`: `give_to_worker()` through it; `take_next_task()` hands a fair share per trip; a
  `std::vector<char> busy_` set in `give_to_worker()` and cleared when `take_next_task()` finds
  nothing pending; an atomic count of waiting `wait()` calls gates `nothing_running()`.
- **Behaviour to accept:** a worker may take up to a fair share of the queue at once. Order stays
  FIFO. With uneven tasks, an idle worker cannot take back what another already holds - the share
  is what bounds that.
- **Step 10, half of it:** `queue_task()` and `nothing_running()` no longer lock a worker, so the
  `is_busy()` half of the lock-order dependency goes. `give_to_worker()` still takes a worker's
  `action_mutex_` under `mutex_` to add the task, so step 10 stays open.

**Tests first:** the suite already covers order, notification and draining; what C adds needs
cases for a share handed to one worker running in order, `wait()` still returning with the scan
gated, and the busy flags agreeing with the workers after a drain. ASan and TSan, as for step 28.

**Not reached by C:** at 4 workers about two-thirds of the submitter's time is still spent on
`mutex_` against the workers, and 17% on waking a worker per task (`notify_one`); `QThreadPool`
submits the same batch in ~70-90 µs. Closing that needs a queue the workers pull from directly - a
redesign of the pool, not this step.

**Done when:** the suite is green on Debug, ASan and TSan, and `bench/qt_pool_vs_this` shows the
executor's `processed` time for empty 1000-task batches near the drop measured above.

**Tests written 2026-10-09, failing as expected.** `executor_tests.a_freed_worker_takes_its_share_of_the_queue_in_one_trip`
(1 worker, 3 queued, the first stops at a gate: `pending()` is 2, expected 0) and
`executor_tests.a_freed_worker_takes_a_fair_share_of_the_queue` (2 workers, 4 queued, one worker
freed: `pending()` is 3, expected 2). Deterministic: every worker is held at a gate when `pending()`
is read. The async half is async's step 50, with its own two tests. Order (FIFO), draining and the
busy-flag bookkeeping stay covered by the existing cases.

**Landed 2026-10-09 (option C).** `give_to_worker()` queues the sealed task with
`execution::add_queued_task()` (public since async's step 50); `take_next_task()` hands a freed
worker `ceil(pending / workers)` tasks per trip; `busy_` (one flag per worker, under `mutex_`)
replaces `is_busy()` in `queue_task()` and `nothing_running()`, and a finished worker scans only
while `waiting_` counts a `wait()`. Two cases the scratchpad version got wrong, both caught by the
suite: a flag is set only when the worker takes the task, and a worker that takes nothing - nothing
queued, or stopped and refusing - is marked idle; otherwise a stopped pool's `wait()` hangs
(`a_queued_task_a_worker_refuses_is_reported` and
`the_destructor_reports_work_a_stopped_pool_could_not_drain` timed out). A refusal ends the share,
so a stopped worker still loses one task, as before, not a share. 50 of 50 on Debug, ASan and TSan.

`bench/qt_pool_vs_this`, two runs, median µs, 1000 tasks:

| | executor before (`33e2e2c`) | executor after | QThreadPool |
|---|---|---|---|
| empty, 1 worker, `delivered` | 351 | 235, 253 | 387, 344 |
| empty, 4 workers, `delivered` | 435 | 349, 322 | 409, 427 |
| empty, 4 workers, `submitted` | 149 | 93, 83 | 59, 74 |
| 10 µs, 1 worker, `submitted` | 19 | 38, 38 | 19, 20 |
| 10 µs, 4 workers, `submitted` | 37 | 72, 73 | 29, 30 |

The executor now finishes empty batches ahead of `QThreadPool` at 1 and 4 workers. **One cost
moved:** with real work, submitting takes about twice as long (0.04-0.07 µs per task), because a
freed worker now holds `mutex_` while it takes its whole share, and the submitter waits for it.
Total time is unchanged (10.45-10.92 ms and 3.00-3.11 ms against `QThreadPool`'s 10.43-10.87 and
3.01-3.13). Taking the share out of `pending_` under the lock and handing it over after releasing
it would shorten that hold; not done here.

**Superseded 2026-10-09, before it was committed.** Option C was implemented and measured as above,
50 of 50 on Debug, ASan and TSan - and then reconsidered (user): the improvement needed `async`'s
`add_queued_task()` public only because this pool keeps a queue of its own between sealing a task
and handing it to a worker, and that queue is what the user wants gone. Step 31 replaces it; the
working-tree changes are discarded with it, including the two share tests. The analysis above - one
task per trip, a second wrapper per task - stays the reason for step 31.

### Step 31 ✅ · item 28 — the pool keeps a queue of its own between the door and the workers — DONE
`executor.hpp:197-237` (`wait`), `:258-284` (`add_task`), `:286-289` (`pending`), `:387` (the worker
type), `:396-465` (`queue_task`, `give_to_worker`, `take_next_task`, `nothing_running`), `:521`
(`pending_`) · CONFIRMED by a prototype on scratchpad copies, 2026-10-09; sites at `33e2e2c`

**The problem.** `add_task()` seals a task with `bind_task()` and keeps it in `pending_` until a
worker is free, then hands the sealed task to the worker. The hand-off either re-wraps it (through
`add_action()`, today) or needs a public sealed-task door in async (step 30, withdrawn). And
`pending_` is what every worker meets on `mutex_` after every pass (steps 28 and 30).

**Decided (user, 2026-10-09): per-worker queues.** The pool does not wait for a free worker: it
picks one at submission and forwards to `execution::add_task(task, args...)`, the same call a
standalone caller makes. A task is sealed once, inside async, and async's API stays as it is.
- **Pick:** the first idle worker from worker 0 (the warmest, as step 8 keeps); otherwise the one
  given the fewest tasks since it last drained. The counts are per-worker atomics, read without a
  lock; a drain (`on_finished`) stores 0. They steer the choice only, so a stale count costs balance,
  never correctness.
- **`wait()`** asks the workers' `is_busy()`, as before step 30. It counts itself under `mutex_`
  before its first check, and a drain takes the lock only to notify while a wait counts.
- **Goes:** `pending_`, `queue_task()`, `give_to_worker()`, `take_next_task()`, the destructor's
  "dropped" report. The workers become `async::execution<actionT>`.

**Behaviour changes, accepted (user, 2026-10-09):**
1. Balancing is by count, not duration: a task waits behind its own worker's queue even if another
   worker goes idle. A smarter balancer (stealing) waits for a use case.
2. Tasks start in submission order per worker, not across the pool. With one worker - flux's default
   - nothing changes.
3. `stop()` runs what was already submitted: a stopped execution drains its queue before it leaves.
   A `forced_stop()` that drops queued work may come later.

**Decided (user, 2026-10-09): (a).** `pending()` keeps its meaning - work waiting, not the pool's
occupancy - and sums `execution::pending()` over the workers; async adds it as its step 52.
**Was to decide: what `pending()` means.** Today it is the shared queue's depth, "not the pool's
occupancy", and ten cases read it. Per-worker queues hold that depth inside the executions.
- (a) Async exposes each execution's queue depth (`execution::pending()`, under its lock) and the
  pool sums them: same meaning, a small async addition that is useful standalone too. **Recommended.**
- (b) `pending()` reports the pick counts - tasks given since each worker last drained, which
  includes finished ones: cheap, but no longer "waiting".
- (c) `pending()` goes.
fluxcpp does not call it.

**Measured** (scratchpad prototype with async's step 51; Qt-free harness, median µs until 1000 empty
tasks are done, two runs):

| | 1 worker | 2 workers | 4 workers |
|---|---|---|---|
| `e9ef8ea` (step 28) | 136-139 | - | 284-299 |
| step 30 (withdrawn) | 74-80 | 107-113 | 206-220 |
| per-worker, async as committed | 103-105 | - | 221-242 |
| **per-worker, async step 51, lock-free counts** | **47-49** | **113-134** | **131-134** |

Run-to-run noise is about ±15%. **The 2-worker case** was 168-170 µs until the counts went
lock-free: with two workers both keep up, so each drains after almost every task (~630 drains per
1000 tasks, against ~0.06 with one worker and ~290 with four), and each drain took `mutex_` against
every pick. What is left there is per-task: each submit locks its worker's queue and fairly often
wakes it.

**Tests first.**
- Changed to the decided behaviour, failing until the fix: `a_queued_task_a_worker_refuses_is_reported`
  and `the_destructor_reports_work_a_stopped_pool_could_not_drain` (`stop()` now runs what was
  submitted, and nothing is reported dropped); the `pending()` cases, per the decision above.
- New, failing until the fix: a task given to a busy worker waits for that worker even when another
  goes idle (2 workers, each held at a gate, 4 tasks: opening one gate runs exactly its 2); an idle
  pool spreads a burst over its idle workers.
- Kept: order on one worker, `wait()` draining, the hard-run cases of step 20, step 28's lock tests.

**Order:** async step 51 first (and step 50's revert), then the executor's async pin, then this.
**Done when:** Debug, ASan and TSan are green in both repos, and `bench/qt_pool_vs_this` shows the
executor at or below the prototype's numbers.

**Tests written 2026-10-09.** `a_queued_task_a_worker_refuses_is_reported` and
`the_destructor_reports_work_a_stopped_pool_could_not_drain` became one case,
`stop_runs_what_was_already_submitted` (1 worker held, 4 queued, `stop()`: all 4 run, nothing
"refused" or "dropped" on stderr) - failing, `ran` 0. New: `a_task_waits_for_the_worker_it_was_given`
(2 workers held at gates, 4 tasks, one gate opened: 2 run) - failing, `ran` 4;
`a_burst_is_spread_over_idle_workers` (two gate tasks back to back reach both workers) - a guard,
passing. Both gate cases hold an `open_on_exit` declared after the pool, so a failed check cannot
leave a worker held and the destructor waiting. The `pending()` cases are unchanged: with (a) their
meaning stays. The rest: 42 of 44 green.

**Landed 2026-10-09 (uncommitted).** As decided: workers are `async::execution<actionT>`;
`add_task()` picks with `pick_worker()` (first idle from worker 0, else the fewest in `given_`, per
worker atomics read unlocked) and forwards to the worker's `add_task()`; `on_finished` ->
`worker_drained()` stores 0 and takes `mutex_` only to notify while `waiting_` counts a `wait()`;
`wait()` waits for `nothing_running()` (the workers' `is_busy()`), a stopped pool included, since its
workers drain what they were given; `pending()` sums `execution::pending()`. Gone: `pending_`,
`queue_task()`, `give_to_worker()`, `take_next_task()`, the destructor's "dropped" report, `<deque>`.
44 of 44 and 5 of 5 smoke on Debug, ASan and TSan; docs clean, 31 pages.

`bench/qt_pool_vs_this`, two runs, median µs, 1000 tasks unless one:

| | executor `e9ef8ea` | executor step 31 | QThreadPool (same runs) |
|---|---|---|---|
| empty, 1 worker, `submitted` | 59-86 | 50-72 | 74-137 |
| empty, 1 worker, `delivered` | 351-543 | 245-363 | 315-502 |
| empty, 4 workers, `submitted` | 149-206 | 42 | 82-83 |
| empty, 4 workers, `delivered` | 435-487 | 365-405 | 410-450 |
| 10 µs, 1 worker, `delivered` (ms) | 10.79-12.54 | 10.84-10.86 | 10.87 |
| 10 µs, 4 workers, `delivered` (ms) | 3.13-3.19 | 3.13-3.15 | 3.13-3.20 |
| one 10 µs task, 1 worker, `delivered` | 18.1-18.2 | 20.2 | 18.1 |

The executor now submits faster than `QThreadPool` and delivers empty batches sooner at 1 and 4
workers; with real work they are equal. **One small cost:** a single task with work arrives ~2 µs
later than with `QThreadPool` (20.2 against 18.1 µs, `processed` 16.5 against 15.4) - the worker's
spin-before-park and lock path from async's step 51, paid once per wake-up.
