# executor.hpp — fix plan

**Status (2026-09-21) — opened, nothing landed.** The pool was imported from
`async/prototypes/executor.hpp` as it stood, and this is the audit of what has to change before it
can be called production ready. 18 items, in six groups. **Four are blockers**: each one is a way
the pool can hang or read freed memory, and three of the four are reachable without any misuse.
**Tests:** 18 of 18 green in Debug, 20 runs in a row, measured on 2026-09-21; clang-format clean;
doxygen clean, `doc/refman.pdf` at 19 pages. Neither sanitizer has been run yet — that is step 17,
and two findings below are waiting on it to be named rather than argued.
**Source:** read of `executor.hpp` against `async.hpp` at `4acb06d`, 2026-09-21. Five findings are
confirmed by a probe or by a test that failed before it was corrected; the rest are read-only and
say so.

**The four gaps the prototype's own README raised are not in this plan.** They were about
`async.hpp`, and `async.hpp` has since closed all four: a throwing action is caught in three arms
(async step 28), `std::cout` is gone (18, 19), `is_busy()` exists (the prototype asked for it), and
`add_action()` returns `bool` (21). What is left of them here is item 5 — the pool is what runs
arbitrary caller code, so *the submitter* still learns nothing when a task throws — and item 11,
which is upstream's to fix.

**The API is fixed by a decision, not by this plan.** The pool keeps the prototype's surface:
`submit()` and `pending()`. No futures, no priorities, no results. Items 9, 16 and 18 are recorded
against that decision rather than argued with; if the decision changes, they are where to start.

## Progress

**Nothing done.** No step has landed and no commit is listed below.

**NEXT: step 14, then group 1 in order — steps 1, 2, 3, 4.** 14 is three lines and it rewrites the
lines step 2 would otherwise touch twice, which is the only reason it goes first. The four blockers
then come together, because they are all in the constructor and the destructor and that is one pass
over those lines rather than four. Step 2 is the one to read first of them: it is the only finding
here that is a use-after-free, and the shape of its fix — the destructor taking `mutex_` for the
parts that touch `workers_` — decides how steps 1 and 4 are written.

**Read the couplings before picking an order.** Steps 1 and 4 are both about a destructor that
waits for something that will never come, and the guard for one is the place to put the other.
Steps 5 and 6 ask the same question — what happened to the task I gave you — and the async plan's
experience with its own steps 21 and 28 was that answering it in two passes answers it twice and
badly; settle them together. Step 14 changes what a worker *is* here, and steps 2 and 8 both
rewrite the lines that name one, so 14 goes before both or gets written three times. Step 13 gives
the pool a name and step 5 needs something to put in a report, so 13 comes first of those two.

| Commit | Step |
|---|---|
| *(none yet)* | — |

## Step index

| # | Item | Concern | Sites | Verified |
|---|---|---|---|---|
| **Group 1 — lifetime (blockers)** |
| 1 | 1 | a pool with no workers takes work nothing can run, and never dies | `:45-59`, `:64-79` | CONFIRMED (hangs; probe) |
| 2 | 2 | `~executor()` mutates `workers_` outside the mutex every reader takes | `:73-78`, `:139-157`, `:160-167` | read-only (TSan: step 17) |
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
| 14 | 14 | `struct worker` is a one-field wrapper the prototype outgrew | `:119-121` | read-only |
| 15 | 15 | copy and move are suppressed by accident, not by statement | `:170` | read-only |
| 16 | 16 | a move-only task does not compile | `:40` | CONFIRMED (compile probe) |
| 17 | 17 | `pending()` is advisory and does not say so | `:111-114` | read-only |
| 18 | 18 | no way to wait for the pool to drain short of destroying it | `:64-79` | read-only, API decision |
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

### Step 2 · item 2 — `~executor()` mutates `workers_` outside the mutex — OPEN
`executor.hpp:73-78`, `:139-157`, `:160-167` · read-only; to be named under TSan at step 17

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

> The destructor holds `mutex_` across the parts that touch `workers_`, and the parts that must not
> hold it — `stop()`, which wakes a worker that will want the mutex, and `clear()`, which waits for
> one — need the workers moved out of the member first, under the lock, and stopped through the
> local copy. That ordering is the step; a reader-writer split or an `std::atomic` flag is not,
> because the invariant being protected is "the vector still exists".

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
> Land it after step 14, which changes what is being scanned.

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

### Step 14 · item 14 — `struct worker` is a one-field wrapper — OPEN
`executor.hpp:119-121` · read-only

```c++
struct worker {
  std::shared_ptr<worker_execution> exec;
};
```

It held the `busy` flag the pool kept before `is_busy()` existed. The flag is gone and the struct
stayed, so every use reads `workers_[index].exec->` for no benefit.

> `std::vector<std::shared_ptr<worker_execution>>`. Land it before steps 2 and 8, both of which
> rewrite the lines that would otherwise be touched twice.

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

### Step 19 — run the suite under both sanitizers — OPEN
`.github/workflows/ci.yml` · —

The CI job exists and has never run, and neither sanitizer has been run locally. Step 2 is a
suspected use-after-free waiting for TSan to name it rather than for this plan to argue it, and a
pool is the shape of program the two sanitizers were built for.

> Build the suite under `address` and under `thread`, in separate directories, and record what each
> one says here. Do it before step 2's fix as well as after: a fix for a race that was never
> observed is a fix that cannot be shown to have worked.

### Step 20 — the suite has no case that runs the pool hard — OPEN
`test/executor_tests.cpp` · —

Every case is deterministic and short, which is what makes them reportable. None of them puts the
pool under the sustained churn that steps 2 and 3 live in — construction and destruction under
load, many workers, tasks finishing while the destructor runs.

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
