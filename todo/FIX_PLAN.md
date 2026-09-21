# executor.hpp — fix plan

**Status (2026-09-21) — one step landed, no blocker yet.** The pool was imported from
`async/prototypes/executor.hpp` as it stood, and this is the audit of what has to change before it
can be called production ready. 19 items, in seven groups — item 19 was added after the audit
closed and is a decision rather than a finding; see group 7. **Four are blockers**: each one is a way
the pool can hang or read freed memory, and three of the four are reachable without any misuse.
**Tests:** 19 cases, 18 green and 1 deliberately red — step 1's guard, written before its fix;
clang-format clean;
doxygen clean, `doc/refman.pdf` at 19 pages. **Both sanitizers have now been run, and TSan has
named step 2.** Locally (macOS, libc++) both came back clean; on CI (Linux, GCC 14, libstdc++) TSan
reported a data race in `concurrent_submission` between `~executor()` and a worker in
`take_next_task()`. Step 2 moves from read-only to CONFIRMED, and the blocker count stands. See step
19 for both runs and why the local clean sheet was platform luck rather than evidence. (An earlier
draft of this line called the sanitizers step 17; they are step 19.)
**Source:** read of `executor.hpp` against `async.hpp` at `4acb06d`, 2026-09-21. Five findings are
confirmed by a probe or by a test that failed before it was corrected; the rest are read-only and
say so.

**The four gaps the prototype's own README raised are not in this plan.** They were about
`async.hpp`, and `async.hpp` has since closed all four: a throwing action is caught in three arms
(async step 28), `std::cout` is gone (18, 19), `is_busy()` exists (the prototype asked for it), and
`add_action()` returns `bool` (21). What is left of them here is item 5 — the pool is what runs
arbitrary caller code, so *the submitter* still learns nothing when a task throws — and item 11,
which is upstream's to fix.

**The API is fixed by a decision, not by this plan — with one part of it now reversed.** The pool
keeps the prototype's surface: `submit()` and `pending()`. No futures, no priorities, no results.
Items 9, 16 and 18 are recorded against that decision rather than argued with; if the decision
changes, they are where to start. **The task type is no longer part of it:** the pool is to run any
task type rather than the `std::function<void(void)>` fixed at `:40`. That is item 19, group 7, and
it pulls items 5, 16 and 18 along with it.

## Progress

Done — step 14, and nothing else. It is the only change to `executor.hpp` so far and it is a
refactor: nothing it touched was defective. **No blocker has landed.**

Step 19 is under way rather than done: its first sanitizer pass ran on 2026-09-21, locally and then
on CI, and produced no code change to commit — the "before" half is done, the "after" half waits on
step 2. It paid for itself immediately: CI's TSan named step 2, which had been the one blocker
resting on a reading rather than on a report.

**Working method, from 2026-09-21.** Each step is a test first: a case that shows the defect, put up
for review on its own, and the fix written only once that case is agreed. The test is the unit of
review, not the fix.

**NEXT: group 1 in order — steps 1, 2, 3, 4.** Step 14 is done (`bf7739b`), which is what these
were waiting on: it rewrote the lines step 2 would otherwise have touched twice. The four blockers
come together, because they are all in the constructor and the destructor and that is one pass over
those lines rather than four. Step 2 is the one to read first of them: it is the only finding here
that is a use-after-free, and the shape of its fix — the destructor taking `mutex_` for the parts
that touch `workers_` — decides how steps 1 and 4 are written. Step 1's case is already written and
red; its fix is the next thing to write.

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
| `bf7739b` | 14 — `struct worker` replaced by `std::vector<std::shared_ptr<worker_execution>>` |

## Step index

| # | Item | Concern | Sites | Verified |
|---|---|---|---|---|
| **Group 1 — lifetime (blockers)** |
| 1 | 1 | a pool with no workers takes work nothing can run, and never dies | `:45-59`, `:64-79` | CONFIRMED (hangs; probe) |
| 2 | 2 | `~executor()` mutates `workers_` outside the mutex every reader takes | `:73-78`, `:139-157`, `:160-167` | **CONFIRMED (TSan, CI)** |
| 3 | 3 | a constructor that throws leaves started workers holding `this` | `:45-59` | read-only |
| 4 | 4 | the destructor waits forever on a task that never returns | `:64-70` | read-only |
| **Group 2 — what the caller is told** |
| 5 | 5 | a task that throws is reported to stderr and to nobody else | `:129-134` | CONFIRMED (test) |
| 6 | 6 | `add_action()`'s answer is dropped, so a refused task vanishes | `:129-134` | read-only |
| 7 | 7 | work spawned by an in-flight task is refused once shutdown starts | `:67`, `:86-106` | CONFIRMED (test) |
| **Group 3 — placement** |
| 8 | 8 | the free-worker scan always starts at worker 0 | `:95-101` | CONFIRMED (20 tasks, 1 of 4; probe) |
| 9 | 9 | the queue is unbounded and nothing pushes back | `:104` | read-only, API decision |
| **Group 4 — what async.hpp imposes** |
| 10 | 10 | the lock order rests on an async.hpp detail that is not its contract | `:129-134`, `:160-167` | read-only |
| 11 | 11 | every worker prints to stdout on shutdown | `async.hpp:757` | CONFIRMED (14 lines of 38) |
| 12 | 12 | the 10ms tick, once per worker | `async.hpp:744-747` | measured, not a defect |
| **Group 5 — surface and hygiene** |
| 13 | 13 | worker names collide between pools | `:50` | read-only |
| 14 ✅ | 14 | `struct worker` is a one-field wrapper the prototype outgrew | `:119-121` | read-only |
| 15 | 15 | copy and move are suppressed by accident, not by statement | `:170` | read-only |
| 16 | 16 | a move-only task does not compile | `:40` | CONFIRMED (compile probe) |
| 17 | 17 | `pending()` is advisory and does not say so | `:111-114` | read-only |
| 18 | 18 | no way to wait for the pool to drain short of destroying it | `:64-79` | read-only, API decision |
| **Group 7 — the task type** |
| 22 | 19 | `task_t` is fixed at `std::function<void(void)>` | `:40`, `:117` | CONFIRMED (compile probe) |
| **Group 6 — the suite** |
| 19 | — | the sanitizers have never been run against the suite | `.github/workflows/ci.yml` | — |
| 20 | — | the suite has no case that runs the pool hard | `test/executor_tests.cpp` | — |
| 21 | — | README and the reference state behaviour the fixes will change | `README.md` | — |

---

## Group 1 — lifetime (blockers)

Four ways the pool outlives or under-lives itself. Every one of them is in the constructor or the
destructor, and none of them needs a caller to do anything unusual.

### Step 1 · item 1 — a pool with no workers takes work nothing can run — OPEN
`executor.hpp:45-59`, `:64-79` · CONFIRMED by probe: the destructor does not return

`executor(0)` is accepted. `workers_` is empty, so `submit()` finds no free worker, queues the task
and answers true; `pending()` says 1. The destructor then waits for `pending_.empty()`, which no
worker will ever make true, and `nothing_running()` — which is vacuously true over no workers —
never brings the predicate up with it. Nothing notifies `drained_cv_` either, because only
`take_next_task()` does and it runs on a worker thread. The probe hung at its 3-second watchdog:

```
submit() returned true
pending() = 1
leaving scope ...
HUNG: ~executor did not return within 3s
```

An empty pool that is never given work destructs cleanly, which is what makes this quiet: the hang
needs a `submit()`, and `submit()` is the one thing every caller does.

`std::thread::hardware_concurrency()` returns 0 when it cannot tell, and a caller passing it
straight through is the likely way in.

> Reject it in the constructor — `std::invalid_argument`, thrown before any worker is started, so a
> pool that cannot run anything never exists rather than existing and hanging. `<stdexcept>` joins
> the includes. A default worker count is a separate question and is not this step's.

**The case is written and red** (2026-09-21): `executor_tests.a_pool_with_no_workers_is_refused`,
one line — `EXPECT_THROW(executor(0), std::invalid_argument)` — reporting *"it throws nothing"*. It
states the guard rather than the hang: an empty pool that is never given work destructs cleanly, so
the case never reaches the wait, and it fails in milliseconds instead of wedging the suite for
three. The hang's standing evidence is the probe above. A first draft built the pool on its own
thread behind a three-second watchdog, which did reproduce the hang as a failure; it was dropped
once the answer was settled as the throw, because it left a thread parked in `~executor` that could
not be joined and that a leak checker would report. Fix not written: awaiting review of the case.

### Step 2 · item 2 — `~executor()` mutates `workers_` outside the mutex — OPEN, CONFIRMED
`executor.hpp:73-78`, `:139-157`, `:160-167` · **CONFIRMED by TSan on CI, 2026-09-21**: data race in
`executor_smoke_test.concurrent_submission`, GCC 14 / libstdc++ / Linux

`mutex_` guards `workers_` for every reader: `submit()` scans it, `nothing_running()` iterates it,
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

### Step 3 · item 3 — a throwing constructor leaves started workers holding `this` — OPEN
`executor.hpp:45-59` · read-only

The constructor starts each worker as it builds it:

```c++
workers_.push_back(std::move(next));
workers_.back().exec->start();
```

Worker 0 is running, with `on_finished` capturing `this`, before worker 1 is created. If creating
worker 1 throws — `create_instance` allocates, `push_back` reallocates, `std::thread` construction
can throw `std::system_error` when a thread cannot be started, which is the plausible one at a
large worker count — then the object never finishes construction, **so `~executor()` is never
called**. Every worker already started keeps running, keeps a callback into a pool whose storage is
about to be reused, and is detached, so nothing joins it.

`std::system_error` from `std::thread` is the realistic trigger, and it arrives exactly when a
caller asks for more workers than the system will give.

> A function-try-block, or a scope guard around the loop: on the way out, stop and release whatever
> was started, then rethrow. What it has to do is the destructor's job minus the drain, so write it
> where step 2 leaves the destructor rather than before.

### Step 4 · item 4 — the destructor waits forever on a task that never returns — OPEN
`executor.hpp:64-70` · read-only

```c++
drained_cv_.wait(lock, [this] { return pending_.empty() && nothing_running(); });
```

An unbounded wait. A task that blocks on something that never arrives takes the destructor with it,
and the process shows a thread parked in `~executor` with nothing said about which task or which
worker. That is the correct default — the alternative, abandoning a running task, is worse — but it
is a hang, and a hang that says nothing is a support call.

Distinct from step 1: there the wait is unsatisfiable by construction, here it is a task's fault
and the pool is behaving as designed.

> Keep the wait unbounded, and say something: `wait_for` on a long interval, and a
> `std::println(stderr, ...)` naming the pool, the queue depth and how many workers are still busy,
> repeated while it waits. The destructor still does not give up; it stops being silent about why.
> Whether the interval is 5s or 30s is worth one measurement, not a debate.

## Group 2 — what the caller is told

### Step 5 · item 5 — a task that throws is reported to stderr and to nobody else — OPEN
`executor.hpp:129-134` · CONFIRMED by test: `a_throwing_task_does_not_stop_the_worker`

`execute_actions()` catches everything an action throws, in three arms, and prints a warning naming
the execution. The worker survives and the next task runs — the test states exactly that, and it
passes.

What the submitter gets is nothing. Not an exception, not a return value, not a callback: a task
that failed and a task that succeeded are indistinguishable from outside the pool, and the only
record is a line on stderr naming `pool_worker_2`, which is not a name the caller chose or knows.

This is the half of the prototype's gap 1 that `async.hpp` could not close, because it is about the
pool's caller and `async.hpp` has never met them. It was tolerable for the prototype. It is the
thing most likely to be asked for first by anything real.

> The API decision rules out futures, so the remaining shape is a reporting seam the pool owns: one
> handler, settable once, called with the exception and whatever identifies the task. Decide it
> with step 6 rather than separately - both are "what happened to the task I gave you", and the
> async plan's steps 21 and 28 are the record of what answering that in two passes costs.

### Step 6 · item 6 — `add_action()`'s answer is dropped — OPEN
`executor.hpp:129-134` · read-only

```c++
workers_[index].exec->add_action(std::move(task));
```

`add_action()` returns `bool` — async's step 21 made it do so precisely so a refusal is not silent
— and `give_to_worker()` ignores it. A refused task is destroyed inside `add_action()` and the pool
goes on believing it placed it. `submit()` has already answered true by then, or
`take_next_task()` has already popped it off `pending_`, so the task is gone from both ends.

It cannot happen today: a worker refuses only after `stop()`, and `stop()` is only called by the
destructor, after the drain. That is an invariant held in place by the order of two functions and
written down nowhere, which is what makes an unchecked return worth this step rather than a shrug.

> Check it. In `take_next_task()`, a refusal puts the task back at the front of `pending_`, which
> is where it came from. In `submit()`, a refusal falls through to the queue. Neither can loop,
> because a refusal means the pool is shutting down and the drain is what ends it. If the answer
> turns out to be unreachable after steps 1 to 4, say so in a comment and keep the check.

### Step 7 · item 7 — work spawned by an in-flight task is refused once shutdown starts — OPEN
`executor.hpp:67`, `:86-106` · CONFIRMED by test, which had to be written around it

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
> Land it after step 14, which changed what is being scanned — done at `bf7739b`, so the scan now
> reads `workers_[index]->is_busy()` and this step is unblocked.

### Step 9 · item 9 — the queue is unbounded and nothing pushes back — OPEN, API decision
`executor.hpp:104` · read-only

`pending_.push_back()` always succeeds. A producer faster than the pool grows the deque until the
process runs out of memory, and `submit()` answers true the whole way down. The only thing a caller
can do about it is read `pending()` and decide for itself.

Recorded, not planned: a capacity is a change to the interface, and the interface is decided. The
argument for taking it anyway is that an unbounded queue is not a smaller interface, it is a policy
- unbounded - chosen silently.

> If this is taken: a capacity given at construction, `submit()` answering false when it is full,
> and the existing false-means-refused contract already carries it. Zero means unbounded, which
> keeps every current caller.

## Group 4 — what async.hpp imposes

Three findings whose cause is upstream. Two are read-only here by nature: the fix, if any, belongs
in `async.hpp` and lands in that repo's plan, not this one.

### Step 10 · item 10 — the lock order rests on a detail that is not async.hpp's contract — OPEN
`executor.hpp:129-134`, `:160-167` · read-only

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
`executor.hpp:119-121` · read-only

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
`executor.hpp:40` · CONFIRMED by compile probe

`task_t` is `std::function<void(void)>`, which requires a copy-constructible target. A lambda
owning a `std::unique_ptr` is rejected:

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

## Group 6 — the suite

### Step 19 — run the suite under both sanitizers — OPEN (the "before" pass has run)
`.github/workflows/ci.yml` · both run locally 2026-09-21, both clean

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

> Three things still open. **The "after" pass**, once step 2 lands — and it now has a named report to
> be measured against rather than a clean sheet, which is the whole reason this step said to run the
> sanitizers before the fix. **Both platforms, every time**: this repo has now seen the same code
> come back clean on one and racy on the other, so a fix is not demonstrated until Linux/libstdc++
> says so. **Step 20 is still worth having**, but its justification has changed: not to make step 2
> observable — it already is — but to shorten the odds of catching what a three-worker case with one
> destructor at the end can still miss.

### Step 20 — the suite has no case that runs the pool hard — OPEN
`test/executor_tests.cpp` · —

Every case is deterministic and short, which is what makes them reportable. None of them puts the
pool under the sustained churn that steps 2 and 3 live in — construction and destruction under
load, many workers, tasks finishing while the destructor runs.

**Amended 2026-09-21.** "None of them" was too strong: `concurrent_submission` reaches step 2 on
Linux under TSan, with three workers and one destruction. What the suite lacks is *repeated*
construction and destruction under load — which is where step 3 lives, and step 3 is still the
finding with no observation behind it.

> A stress case, off by default or bounded to a few seconds: construct and destroy repeatedly while
> submitting, at a worker count above the core count. It is the case most likely to make step 2
> reproducible under TSan.

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
`std::function<void(void)>` fixed at `:40`. It is recorded as its own group rather than folded into
group 5 because it reverses half of the API decision the rest of this plan is written against, and
because it is not a defect — nothing here is wrong today, it is a surface that was decided narrowly
and is now decided wider.

### Step 22 · item 19 — `task_t` is fixed at `std::function<void(void)>` — OPEN
`executor.hpp:40`, `:117` · CONFIRMED by compile probe: `execution` instantiates on other action types

```c++
using task_t = std::function<void(void)>;                                          // :40
using worker_execution = untangle::async::execution<std::function<void(void)>>;    // :117
```

One signature, hard-coded twice — and the second spells it out again instead of saying `task_t`, so
the two can drift. A caller whose task takes an argument or returns a value has to erase it into
`void()` and carry the rest itself, which is exactly what the class comment at `:31-35` tells them
to do.

Nothing upstream requires this. `execution` is already a template on its action type, and a probe
built both of the shapes the pool refuses:

```
execution<std::function<int(void)>>   - instantiates, runs, action returns 42
execution<std::function<void(int)>>   - instantiates, runs, add_action() binds the argument
```

So the shape is `template <typename task_t = std::function<void(void)>> class executor`, with
`worker_execution = async::execution<task_t>` and the default keeping every current caller
source-compatible.

**Three constraints and one open question come with it. They are what this step has to decide,
rather than discover halfway through.**

*`task_t` is not restricted to `std::function` — but it must name its own `result_type`.*
`execution` uses `typename actionT::result_type` in five places (`async.hpp:257`, `:268`, `:566`,
`:602`, `:819`), and it is **read from `actionT`, not deduced from `operator()`**. Three
instantiations, each built:

```
execution<decltype([]{})>                          - error: no type named 'result_type' in '(lambda ...)'   async.hpp:566
execution<struct{ void operator()() const; }>      - error: no type named 'result_type' in 'fn'             async.hpp:566
execution<struct{ using result_type = void;        - COMPILES; add_action() -> true; the action runs
                  void operator()() const; }>
```

So the contract is **any callable type that declares a nested `result_type`**. A hand-written
functor qualifies and the third case proves it end to end; a bare lambda and a plain function
pointer do not, because neither has anywhere to put the typedef. `std::function<R(Args...)>`
qualifies only because it still supplies `result_type` — see the portability note below.

The async suite is consistent with this and does not contradict it: every `execution<...>` there is
on a `std::function` (`async_tests.cpp:28-30`, `:113`, `async_smoke_test.cpp:38-41`,
`other_async.hpp:16`). `counting_action` at `async_tests.cpp:97` is a custom `operator()` type, but
it is the *target carried inside* `counting_action_t = std::function<void(const copy_counter&)>`,
not the template argument — which is the distinction to keep hold of here: what `add_action()`
accepts has always been wider than what `execution` can be instantiated on.

That gap is also the choice this step has to make. With `task_t = std::function<void(void)>`,
`submit()` already takes any lambda, because the conversion happens at the parameter. With
`task_t = fn`, `submit()` takes an `fn` and nothing else. A `submit()` templated on the callable,
wrapping into `task_t`, restores the wider door for every instantiation — a separate and smaller
change that can be had with or without this one; say which of the two is meant before writing
either.

*A non-void `R` reopens the results question.* The API decision ruled out results — but a pool
templated on `std::function<int(void)>` is a pool whose tasks return something, and `execution`
already collects those. Either the pool discards `R` and says so in one line of documentation, or it
grows a way to read it, which is step 18's `wait_idle()` and step 5's reporting seam arriving
together by a different door. Decide it with 5 and 18, not on its own.

*Arguments have to be forwarded or refused.* `add_action(actionT, Args&&...)` binds its arguments
(`async.hpp:422-425`), so a pool on `std::function<void(int)>` needs a `submit()` that takes and
forwards them — and `pending_` then holds bound callables rather than `task_t`, a second type in the
class and the place where a template makes the most mess. Refusing arguments (`R(void)` only) is a
defensible narrowing and costs one `static_assert` with a readable message.

*Portability of the default — SETTLED, 2026-09-21.* `std::function::result_type` was removed from
the standard in C++20, so this step was opened with an open question against it: libc++ still
provides the typedef, which is why the probes compile here, but nothing had been built against
libstdc++. The first CI run answers it — the Linux thread job compiled `async.hpp` and ran
`execution<std::function<void()>>` under GCC 14 against `/usr/include/c++/14`, as the TSan stacks in
step 2 show frame by frame. libstdc++ still supplies `result_type` at `-std=c++23`. Nothing to
carry to the async plan and nothing for this step to work around.

What survives the answer is the shape of the risk: the default `task_t` depends on a typedef the
standard removed, on both implementations tested, by grace rather than by contract. The widened
contract above is the escape hatch if that ever changes — a caller supplying a functor with its own
`result_type` never touches `std::function` — which is an argument for this step rather than
against it.

> Template the class on `task_t`, default `std::function<void(void)>`, and write `worker_execution`
> in terms of it so the signature appears once. Sequence it **after group 1**; step 14 is already
> done, so that half of the sequencing is discharged.
> Step 16 does not dissolve into this: the move-only gap lives in `queued_action_t` upstream, and a
> template parameter on the pool does not reach it.
