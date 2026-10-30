# executor.hpp — tasks on the pool, feature plan

**The pool gains a second door.** Today `add_task()` is fire-and-forget: the pool runs the task, and
what it returns is dropped. After this chain a caller can hand the pool a **task** — an action bound
to its arguments *and* to the callback it must notify — and be told when it finished, with its
result if it has one.

**A task without a callback does not exist.** Decided 2026-09-25 in the actuator, and it is what
separates a task from an action rather than a restriction laid on top. The callback is the **last
argument**, taken by position rather than recognised by type, and it is **not** forwarded to the
task's action.

**Status (2026-09-25) — CLOSED. Committed as `e784324`: steps 1, 2, 5 and 6 done, 32 of 32 and 5
of 5 green; steps 3 and 4 were merged into step 2. All three repos in the chain are closed.** This is a feature plan, not a fix plan. Claims marked **PROBED** were compiled and run on
2026-09-24/25; the rest are read-only and say so.

**2026-10-28 — steps 7 and 8 opened: one pool shared by many owners.** A second feature in this
file, planned with the user for fluxcpp's stores and aggregators (fluxcpp's `todo/FEATURE_PLAN.md`).
Step 7 gives tasks a type, light or heavy, and the pool workers of each type; step 8 gives an owner
an ordering key, so its tasks never overlap and finish in the order they were added, on whichever
worker their type picks. Nothing written yet; sites at `0d0ecb9`.
**2026-10-29 — step 7 done (`f0de732`).** 63 of 63 on Debug, ASan and TSan; `doc/refman.pdf` at 33
pages. A 4-5% submit gap in the bench was placement, not code: the bench now builds with
`-falign-functions=64`. Step 8 next.
**2026-10-30 — reworked with the user.** The first ordered queues, handed over from each callback,
were judged convoluted and kept as a POC (a local stash), not landed. Step 8 is now the split: an
`owned_executor`, identical to the pool before step 7, and a `shared_executor` with step 7's types.
Step 9 gives the shared one ordered queues, by a queue id, as a drain loop; its async door,
`add_queued_task()` made public, is in (`ab267f3`).

**Baseline.** `executor` at `c2bf9f0`, `async` at `985a991`, `actuator` at `a8b8b47`. Sites are line
numbers at those commits and move with every step that lands.

## The chain

| Repo | Plan | Holds |
|---|---|---|
| `actuator` | `async/actuator/todo/FEATURE_PLAN.md` | the mechanism: `task_callback_for`, `task<R>`, `bind_task()`, `add_task()`, `call_tasks()` |
| `async` | `async/todo/FEATURE_PLAN.md` | the queue: a tasks list beside the actions, reached through `execution::add_task()` |
| `executor` | this file | the pool: two doors, one queue, and the acceptance criteria |

Steps are numbered per repo, as in `FIX_PLAN.md`; cross-repo references are qualified ("async's
step 3"). Each repo is green and bumped before the next starts. **Nothing here can be written until
both of the others have landed** — which is the point of putting the acceptance criteria at this
end.

## Why — the pool has nothing to say about a task it ran

`add_task()` answers `true` when the task was accepted and `false` when it was refused (`:163-166`),
and that is the last the caller hears. A non-void return is "run and dropped" by the class comment's
own words (`:36`). `on_task_error` (`:225-236`) speaks only when the task threw. **Nothing reports
the ordinary case: it finished, and here is what it produced.**

The pool cannot build that itself. It queues nullary callables and hands them to
`execution::add_action()`, and neither layer carries a completion callback — so the notification has
to come from the actuator, through async, to here. That is the chain.

**A trailing callback passed to `add_task()` today is silently ignored. PROBED:**

| Path | What the actuator is invoked with | Callback |
|---|---|---|
| `connect(action)` then `a(21, cb)` | `(21, cb)` | **fired** — 42 |
| `execution::add_action(action, 21, cb)` | `()` — `std::bind` sealed the pack (`async.hpp:496`) | not fired |
| `executor::add_task(task, 21, cb)` | `()` — `bind_task` sealed it again (`executor.hpp:304`) | not fired |

Nothing warns, because under the old convention the callback was also a legitimate argument of the
task, so `add_task()`'s `static_assert` (`:170`) is satisfied. **Under the decision of 2026-09-25
that silence stops being possible**: a task's callback is a parameter, so a caller who wants one
cannot fail to pass one, and a trailing callable passed to the fire-and-forget door is necessarily a
real parameter of the action's own signature or the call does not compile.

## Two doors, and what they are called

`FIX_PLAN.md` step 23 renamed `submit()` to `add_task()` **specifically to match
`execution::add_action()`**. That match is worth keeping, and it is what decides the names:

| Door | Signature | Notifies |
|---|---|---|
| `add_action()` | `add_action(task, args...)` | no — fire and forget, exactly today's behaviour |
| `add_task()` | `add_task(task, args..., callback)` | yes, always, on a normal return |

So the existing method is **renamed** `add_action()` and `add_task()` becomes the new
callback-carrying one. It restores step 23's intent rather than reversing it: the pool's
fire-and-forget door is again spelled like async's, and the two repos now offer the same pair of
names.

**It costs one identifier at 52 call sites** — 46 in `executor_tests.cpp`, 6 in
`executor_smoke_test.cpp` — and none of them gains an argument. The alternative, making the existing
`add_task()` require a callback, costs an argument at all 52 and leaves the pool with no
fire-and-forget door at all.

> **Settled 2026-09-25 and done**, as step 1. Everything below assumes it.

## What this does not delete — a correction to an earlier draft

An earlier version of this plan had `task_call` (`:289-301`) and `executor::bind_task` (`:304-312`)
deleted outright, some 35 lines, on the premise that the queue would hold one kind of entry.
**Withdrawn.** With two doors the pool has two kinds of queued work, and `task_call` is still the
carrier for the fire-and-forget one. `executor::bind_task` still binds it. What changes is that the
notifying door builds an `untangle::task<R>` through `untangle::bind_task()` instead.

## Step index

| # | Step | Sites | Evidence |
|---|---|---|---|
| 1 ✅ | `add_task()` renamed `add_action()`, at 51 call sites | `:145-179`, tests, `README.md` | CONFIRMED (27 + 5 green) — **DONE** (`e784324`) |
| 2 ✅ | one FIFO queue, two kinds, `add_task()`, and the cases | `:193-229`, `:326-344`, `:377-395`, `:403-414` | CONFIRMED (5 cases) — **DONE** (`e784324`) |
| 3 | — merged into step 2, see below | — | — |
| 4 | — merged into step 2, see below | — | — |
| 5 ✅ | `on_task_error`, the callback's thread, the destructor, and `README.md` | `:87-96`, `:288-308`, `README.md` | transcription — **DONE** (`e784324`) |
| 6 ✅ | `README.md`, `tools/make_doc.sh`, and the commits | `README.md`, `doc/` | **DONE** (`e784324`) |
| 7 ✅ | a task cannot say it is heavy: every worker takes any task, so a long one can hold all of them | `:56` (constructor), `:220-250` (`add_task`), `:363` (`pick_worker`) | CONFIRMED (9 cases, 63 of 63 on Debug, ASan and TSan) — **DONE** (`f0de732`) |
| **8** | **one class holds both modes: the owned pool carries the shared one's types; split into `owned_executor` (the original, renamed only) and `shared_executor`** | `executor.hpp` (whole), `test/`, `bench/qt_pool_vs_this`, `README.md`, `Doxyfile`, CI | planned 2026-10-30 — OPEN |
| 8.1 | tests first: `owned_executor_tests.cpp`, `shared_executor_tests.cpp`, the smoke test | `test/` | OPEN |
| 8.2 | `owned_executor.hpp`: `1c3a203:executor.hpp`, renamed only | `executor.hpp` | OPEN |
| 8.3 | `shared_executor.hpp`: `f0de732:executor.hpp`, renamed; `unnamed_shared_pools` | `executor.hpp` | OPEN |
| 8.4 | users and docs: `executor.hpp` removed, CMake, Doxyfile, CI, the bench, `README.md`, `tools/make_doc.sh`, LF | `test/`, `bench/`, `README.md`, `doc/` | OPEN |
| 8.5 | checks: Debug, ASan, TSan for both; the bench matches the original | — | OPEN |
| **9** | **tasks from one owner on a shared pool overlap and finish out of order: ordered queues, as a drain loop, on `shared_executor`** | `shared_executor.hpp` | first implementation kept as a POC (stash), not landed; planned 2026-10-28, reworked 2026-10-30 — OPEN; after step 8 |
| 9.1 ✅ | async bumped to `7464139`: `add_queued_task()` public | `async` (pin) | **DONE** (`ab267f3`) |
| 9.2 | tests first: the POC's 10 cases; a throwing callback moves the queue on; an ordered queue takes any action type | `test/shared_executor_tests.cpp` | OPEN |
| 9.3 | the drain: `add_drain()`, `drain()`, `stopped_`, `ordered_queues_` | `shared_executor.hpp` | OPEN |
| 9.4 | docs: `README.md`, the doc blocks, `tools/make_doc.sh`, LF | `README.md`, `doc/` | OPEN |
| 9.5 | checks: Debug, ASan, TSan; the ordered cases 50 times; the bench; fluxcpp | — | OPEN |

### Step 1 ✅ · `add_task()` renamed `add_action()` — DONE

`executor.hpp:145-179`, `test/executor_tests.cpp` (46), `test/executor_smoke_test.cpp` (7) and
`README.md` (9). 27 of 27 and 5 of 5 green, clang-format clean, doxygen clean, `doc/refman.pdf` at
25 pages.

**51 call sites, not 52** — the count in the recommendation above was off by one, from a `grep -c`
of lines rather than occurrences. Every one of them changed a single identifier and none gained an
argument, which was the point of the rename over the alternative.

**The header gained four lines beyond the rename**, saying what the name now buys:

> Named for async::execution::add_action(), which it forwards to and which behaves the same way:
> the work runs, what it returns is dropped, and nothing reports that it finished. The answer below
> is the last the caller hears.

**Two things the rename dragged with it**, both small and both worth knowing for the next two
repos' worth of renames: clang-format rewrapped two comment lines once the identifier grew three
characters, and a sentence in `README.md` became "binds them the way `execution::add_action()`
does" — true, and now reading as a tautology, so it says *its namesake* instead.

> **The class vocabulary is now inconsistent, and step 3 is where it gets fixed.** `add_action()`
> takes a `taskT`; after step 3 so will `add_task()`. Async resolves this by having the template
> parameter name the **callable type** — `execution<actionT>` — and the two doors name the two
> **kinds of queued work**, both taking an `actionT`. The pool should match: `executor<actionT>`,
> with `task_call`, `pending_` and the class comment following.
>
> **Deliberately not done here.** The rename only becomes legible once both doors exist, so doing
> it now would land a whole-file diff ahead of the thing that motivates it. Folded into step 3,
> where every name changes for one stated reason. Recommended 2026-09-25, not separately agreed.

### Step 2 ✅ · one queue, two kinds, and the door that fills it — DONE

Steps 2, 3 and 4 merged. `executor.hpp:36-43` (the class doc), `:193-229` (`add_task()`), `:326-344` (`task_call`),
`:350-368` (`bind_action()`), `:377-395` (`queue()`), `:403-414` (`give_to_worker()`). Five cases at
`test/executor_tests.cpp:797-959`. 32 of 32 and 5 of 5 green,
clang-format clean, doxygen clean, `doc/refman.pdf` 25 to 27 pages.

**Merged for the same reason async's steps 1 to 3 were, and with that precedent.** The pool has no
second kind of queue entry until `add_task()` exists, so step 2 alone had nothing to carry and
nothing to observe, step 3 had nowhere to put what it built, and step 4's cases *were* the
observable claim. Three pieces, one review, because there is one claim: a task queued here runs and
notifies, and the order across both kinds is the order they arrived.

**Route one, as recommended.** `task_call` gained a private optional callback slot:

| Route | Cost |
|---|---|
| **Taken:** `task_call` gains an optional callback slot | one queue, so FIFO across kinds and a single `pending()` come out for free rather than being arranged; the pool holds an optional callback internally while the public rule is that a task always has one |
| Two deques | order across the two kinds is lost, and `pending()` stops having one answer |
| `std::deque<std::variant<...>>` | order preserved, but every reader of the queue grows a visit for a distinction only `give_to_worker()` cares about |

**`queue()` is what makes the ordering free.** It was extracted from `add_action()` and both doors
call it; **neither kind is treated differently there**, so the FIFO promise is a consequence of
having one container rather than something the code arranges. `give_to_worker()` is the only place
that tells them apart, and only to choose which door of the worker to use — taking the callback out
first, so the worker is handed the callable with the callback beside it, where
`async::execution::add_task()` expects to find one.

> **The two queues differ, deliberately, and this is where that is written down.** A pass in
> `async` fires every action it holds and then every task, so within one batch the kinds do not
> interleave in arrival order; the pool keeps strict first in, first out across both. The pool can
> promise it because it owns one container outright, while async fires an actuator in two passes.
> Decided 2026-09-25 rather than discovered — async's plan raised the discrepancy and this is the
> answer to it. `the_queue_keeps_both_kinds_in_the_order_they_arrived` states the pool's half.

**`taskT` became `actionT`, folded in from step 1 as recommended.** The parameter names the
callable, the two doors name the kinds of work, and both take an `actionT` — which is exactly
`execution<actionT>`'s arrangement, so the pool and the thing it is built out of now read alike.
The pool's private `bind_task()` became `bind_action()` in the same pass: it binds an action and
deliberately leaves the callback slot empty, and sharing a name with `untangle::bind_task()` while
doing something different was going to mislead someone.

**What is enforced elsewhere, and so is not tested here:** that a callback is required (a
`static_assert` in `untangle::bind_task()`), that an empty one is refused (`actuator::add_task()`,
covered in that repo), and that a stopped pool refuses (`add_action()`'s existing cases cover the
`running_` gate, which `add_task()` shares).

### Step 5 ✅ · what the reference has to say — DONE

`executor.hpp:87-96` (the destructor), `:288-308` (`on_task_error`), and `README.md` — a
`## tasks: work that reports back` section, plus the rename of `## the task type`. 32 of 32 and 5
of 5 green, clang-format clean, doxygen clean, `doc/refman.pdf` 27 to 29 pages.

**Most of it was already written**, because each `@attention` went into the header as step 2
landed: the last argument being the callback, finished not meaning failed, and a refused task not
notifying. What this step added is the two places those rules are *met* rather than declared.

- **`on_task_error` now says a task's failure arrives there, and so does its callback's** — so it
  can fire for a task that in fact **succeeded**: the work was done and only the telling failed.
  And a second attention, that a task's callback runs on a worker's thread under the same rule,
  several at once on different workers, with nothing escaping it.
- **The destructor now says that queued work dropped at shutdown never notifies.** The count on
  stderr is the whole report, and it says *how much* was lost, not *which*.

**`README.md` gained a tasks section** — the call shape, the void form, callback-last, the shared
queue's ordering, `pending()` counting both kinds, refusal not notifying, finished not meaning
failed, and which thread a callback runs on. The ordering paragraph is where the difference from
`async` is put in front of a caller:

> The pool can promise that because it owns one container; `async::execution`, which the workers
> are, fires every action in a pass before any task, so its ordering across the two kinds differs.
> Only the pool's own order is a promise to a caller of the pool.

**`## the task type` became `## the action type`**, left over from step 1's rename and found only
by reading the file. It documents the template parameter, which is `actionT` now, and the section
says why: both kinds of queued work *are* that one callable, which is why the parameter names it
rather than either kind.

> **`FIX_PLAN.md` step 21 is not discharged by this, and the earlier note here implied it might
> be.** That step is about the README stating behaviour *its own* steps 4, 5, 7 and 13 change — a
> different set of changes from this feature's. This pass rewrote the same file for tasks and the
> rename, and did not audit it against those four. Whether the README is now correct about them is
> **unverified**, and remains that step's to answer.

### Step 6 ✅ · the reference and the commits — DONE

**`README.md` and `doc/refman.pdf`** were done in step 5 and rebuilt at every step before it — 25
pages when step 1 started, 29 now. **The commits** are `e784324`, carrying steps 1, 2 and 5
together: the header, the five cases, `README.md` and the reference.

**No bump.** `executor` is the top of the chain; nothing records it. The two it takes are already
made — `25bf0ec` for `async` at `d90b28f`, and the actuator's inside that.

> **`FIX_PLAN.md` step 21 is struck from this step and stays open in its own plan.** It asks
> whether `README.md` states behaviour that *that* plan's steps 4, 5, 7 and 13 change. This feature
> rewrote the same file for a different reason and did not audit it against those four. Listing it
> here would have made this step wait on an answer it was never going to give — the same shape the
> actuator's and async's step 7 had, where a bump belonging to another repo kept a step from
> closing on its own terms.

**One thing this plan got wrong four times, and it is worth the paragraph.** The hash was written
into the plan and then swept into the commit it names — `e87da41` became `e784324` under an amend
fixing a typo in the subject, and the citations inside that very commit were stale the moment it
landed. The actuator's plan did it three times before that. **A plan's own update belongs in a
later commit**, not because of bookkeeping etiquette but because it is the only shape in which the
number can be true: a commit cannot contain its own hash, and an amend rewrites it after the fact.

## One pool, many owners — steps 7 to 9, opened 2026-10-28

**Why (2026-10-28, user):** fluxcpp's stores and aggregators each own a pool, a worker each by
default, so an app with many of them runs as many threads. They should be able to share one fixed
pool instead, say four workers, passed to each by reference. Owning a pool stays possible too:
sharing is an extra mode, not a replacement (fluxcpp's `todo/FEATURE_PLAN.md`). Two things are
missing for that: a heavy task must not take every worker (step 7), and one owner's tasks must not
overlap or reorder once they can land on different workers (step 9). Step 8 keeps the owned pool
as it was and puts both in a class of its own.

**Settled with the user:**
- The type is the task's, not its owner's: one store runs both kinds.
- It lives in the shared executor's header, as an enum. No struct that groups pools.
- The pool keeps one owner's tasks in order. A wrapper in fluxcpp was weighed and set aside, because
  it cannot work with a pool-wide drain (step 9).
- A pool-wide `wait()` is acceptable to the user, and so is its refusing new work meanwhile. So is a
  pool whose lifetime, start and stop the application manages.

### Step 7 ✅ · a task cannot say it is heavy — DONE (`f0de732`)
`executor.hpp:56` (constructor), `:220-250` (`add_task`), `:363-385` (`pick_worker`) · planned
2026-10-28, sites at `0d0ecb9`

**The problem.** Every worker takes any task. On a pool shared by many owners, a few long tasks can
hold every worker, and every owner's short ones wait behind them.

> Proposed - agreed with the user, nothing written:
> - **`enum class task_type : std::uint8_t { light, heavy };`** in this header. The two kinds are
>   enough for now.
> - **A second constructor, `executor(std::size_t light_count, std::size_t heavy_count, std::string
>   name = {})`.** The first, `executor(worker_count, name)`, stays and builds light workers only.
>   Zero light workers is refused, as zero workers is today. Zero heavy workers is allowed.
> - **`add_task(task_type type, task, args..., callback)`** gives the task to a worker of that type,
>   picked as `pick_worker()` picks today, but within that type: the first idle one, else the one
>   given the fewest. The untyped `add_task(task, args..., callback)` stays as it is, with any
>   worker and no extra cost.
> - **A heavy task on a pool with no heavy workers runs on a light one,** and the pool warns once on
>   stderr (not once per task) that it should be built with `(light_count, heavy_count)` to dedicate
>   heavy workers.
> - **Workers are named after their type,** `<name>_light_<i>` and `<name>_heavy_<i>`, in every
>   warning.
> - **Tests:** typed tasks land only on workers of their type; the fallback runs and warns once; the
>   untyped path behaves as today; the new constructor's refusals.
> - **Bench:** `bench/qt_pool_vs_this` shows the untyped path no slower.

**Landed in `f0de732`.**
- **`task_type`:** `enum class task_type : std::uint8_t { light, heavy }`, in `untangle`.
- **Constructors:** `executor(light_count, heavy_count, name)` puts the light workers first and the
  heavy ones after, so each type is one range of `workers_`. Zero light workers throws
  `std::invalid_argument`; zero heavy workers is allowed. `executor(worker_count, name)` delegates
  with no heavy workers.
- **`add_task(task_type, ...)`:** copying and moving overloads. `pick_worker(type)` picks within the
  type's range through `pick_worker(first, last)`, the old loop over a range; the untyped
  `pick_worker()` is that over every worker.
- **The fallback:** a heavy task on a pool with no heavy workers goes to a light one, and the pool
  prints, once per pool: `warning: executor <name>: a heavy task found no heavy workers and ran on a
  light one - build the pool with (light_count, heavy_count) to dedicate heavy workers`.
- **Names:** workers are `<name>_light_<i>` and `<name>_heavy_<i>`, no longer `<name>_worker_<i>`.
  The README and the doc blocks say so; every new or changed function documents its parameters and
  return.
- **Tests, written first and red until the API existed:**
  - `a_typed_task_runs_on_a_worker_of_its_type`
  - `a_pool_built_with_one_count_has_light_workers`
  - `a_light_task_does_not_wait_for_heavy_work`
  - `a_heavy_task_waits_for_a_heavy_worker`
  - `a_burst_of_heavy_tasks_is_spread_over_idle_heavy_workers`
  - `a_heavy_task_without_heavy_workers_runs_light_and_warns_once`
  - `an_untyped_task_runs_on_any_worker`
  - `a_pool_with_no_light_workers_is_refused`
  - `a_typed_task_notifies_its_callback`

  63 of 63 on Debug, ASan and TSan; fluxcpp's 39 of 39 against it.

**The bench, and why it looked like a regression.** `bench/qt_pool_vs_this`, alternating with the
code before it (`1c3a203`), as built then - no alignment flag. Submit times, median of 6-12 runs:

| case | before | step 7 |
|---|---|---|
| 1000 x 10 µs, 4 / 1 workers | 30.1 / 19.1 µs | 31.4 / 19.9 |
| mixed, 4 workers | 43.5 µs | 45.6 |
| delivery, every case | - | level |

About 1 ns per untyped task, 4-5%. Two variants did not close it: the new members placed last (kept;
it narrowed the 4-worker empty and mixed batches) and a loop of its own for the untyped
`pick_worker()` (no effect, dropped). **The cause is placement:** the untyped path's machine code
is the same - in `run_batch`, where `submit()`, `add_task()` and `pick_worker()` are inlined, one
load is scheduled two instructions earlier, and `give_to_worker()` is identical - but
`give_to_worker()`, called once per task, moved from byte 52 to byte 20 of its cache line. Both
built with `-falign-functions=64`, four runs each, alternating:

| case | before | step 7 |
|---|---|---|
| 1000 x 10 µs, 4 / 1 workers | 31.7 / 20.1 µs | 30.6 / 19.1 |
| 1000 empty, 4 workers | 36.5 µs | 34.3 |
| mixed, 4 / 1 workers | 43.2 / 22.6 µs | 38.5 / 22.7 |
| single tasks, delivery | level | level |

Level or faster. One worker's empty batch follows placement into its fast or slow mode, in both
versions alike (step 7 fast unaligned, slow aligned). **Decided (user, 2026-10-29):** no
regression; the bench builds with `-falign-functions=64` from now on, so a comparison measures code
and not placement (`bench/CMakeLists.txt`, `bench/README.md`).

### Step 8 · one class holds both modes — OPEN
`executor.hpp` (whole), `test/`, `bench/`, `README.md`, `Doxyfile`, `.github/workflows/ci.yml` ·
planned 2026-10-30

**The problem.** A pool a store owns needs neither task types nor ordered queues, yet since step 7
it carries types, and step 9 would add queues. The owned pool is what fluxcpp's stores and
aggregators run on today, and it works.

**Decided (user, 2026-10-30): the owned executor stays identical to the original.** Two headers,
no base class: the worker management is written twice rather than the owned pool's code moved.

> Proposed - agreed with the user, nothing written:
> - **`owned_executor.hpp`**, `owned_executor<actionT>`: `executor.hpp` as it was before step 7
>   (`1c3a203`), renamed only - the class, the include guard and the file's brief. Its logic,
>   names, messages and worker names (`<name>_worker_<i>`) are unchanged.
> - **`shared_executor.hpp`**, `shared_executor<actionT>`: `executor.hpp` as step 7 left it
>   (`f0de732`), renamed the same way: `task_type`, `(light_count, heavy_count, name)`, the untyped
>   and typed `add_task`. Step 9 adds the ordered queues here.
> - **One clash, renamed in the shared one only:** both headers define `untangle::unnamed_pools`,
>   so including both would not compile. The shared one's becomes `unnamed_shared_pools`, and its
>   default name `shared_pool_<n>`, so the two kinds' warnings tell themselves apart.
> - **`executor.hpp` is removed:** code includes the header of the executor it uses.
> - **8.1 · Tests first.** `test/owned_executor_tests.cpp` is `executor_tests.cpp` as of `1c3a203`
>   and `test/shared_executor_tests.cpp` as of `f0de732`, each renamed only; the smoke test uses
>   `owned_executor`. They fail to build until the headers exist.
> - **8.2 · `owned_executor.hpp`.** Checked by a diff against `1c3a203:executor.hpp` that shows
>   renames only.
> - **8.3 · `shared_executor.hpp`.** Checked the same way against `f0de732:executor.hpp`, plus the
>   clash's rename.
> - **8.4 · Users and docs.** `executor.hpp` removed; `test/CMakeLists.txt`, `Doxyfile` and CI
>   follow; `bench/qt_pool_vs_this` uses `owned_executor`; `README.md` gets a section per
>   executor; `tools/make_doc.sh` clean; every file LF.
> - **8.5 · Checks:** Debug, ASan and TSan green for both; the bench builds and `owned_executor`'s
>   numbers match the original's; fluxcpp's `flux_common.hpp` and its feature step 1 follow there.

### Step 9 · one owner's tasks overlap and reorder on a shared pool — OPEN
`shared_executor.hpp`, after step 8 · planned 2026-10-28, reworked 2026-10-30

**The problem.** An owner that holds state, such as a fluxcpp store, counts on its tasks running
one at a time and in the order it added them, as they do on a pool of its own with one worker. On a
shared pool, and with step 7's types, two of its tasks can run at once on two workers and finish in
either order: its state races, and its answers go out of order.

**Why in the pool, not in a wrapper over it.** A wrapper would hand over an owner's next task
through the public `add_task()`, which `wait()` refuses while it drains, so a pool-wide drain would
drop what the wrapper still holds. Each owner would then need a drain of its own. Inside the pool
the hand-over is internal: what was accepted runs, and `wait()` and `pending()` see it.

**The API, agreed 2026-10-28 and named 2026-10-30, on `shared_executor` only:**
- **`std::size_t next_queue_id()`** returns a new queue id, a counter starting at 1, never 0:
  nothing is allocated.
- **`add_task(std::size_t queue_id, task_type type, task, args..., callback)`.** Tasks of one
  ordered queue never overlap, and run and notify in the order added, each on a worker of its own
  type. Different queues run in parallel. Added means the order `add_task()` calls take the lock:
  one thread's order is kept, two threads' are not ordered between them.
- A queue needs no release: it exists only while it has tasks.
- **`pending()`** counts the tasks waiting in ordered queues; **`wait()`** runs them, since they
  were accepted before it began; an explicit **`stop()`** drops them, with a warning naming how
  many. The destructor drains before it stops, so it drops nothing.
- **Not here, later:** an owner answering light tasks while its own heavy one runs. That gives up
  the ordering for those tasks, so it belongs to the owner: fluxcpp's feature plan, "later".

**First implementation, kept as a POC, not landed (2026-10-30, user: "convoluted").** The finished
task's callback handed the queue's next task over to a worker. It needed both the task and the
callback wrapped (`guarded_task`, `guarded_callback`) so a throw still moved the queue on, a
handing-over `task_t` per waiting task, and an `actionT` able to hold the wrapper. An allocation
failure while handing over left the queue stuck. Saved on the user's machine as `git stash`
"POC: shared pool, ordered queues handed over from the callback"; its 10 cases and its doc work are
reused below.

> Proposed - agreed with the user, nothing written: **a drain loop.** One task on a worker runs the
> queue's tasks in turn.
> - **9.1 ✅ · async bumped to `7464139`** (`ab267f3`), its step 9: `execution::add_queued_task()`
>   is public, so the pool can queue its own drain, which is not of the `actionT` type.
> - **9.2 · Tests first.** The POC's 10 cases, on `shared_executor`: they describe behaviour, not
>   the mechanism. Two more:
>   - `a_queued_callback_that_throws_still_moves_the_queue_on`, which no case covered;
>   - `an_ordered_queue_takes_any_action_type`: an ordered task on a pool whose `actionT` is not a
>     `std::function`, which the POC refused with a static_assert.
> - **9.3 · The drain**, in `shared_executor.hpp`.
>   - **`add_task(queue_id, ...)`** seals the task, its arguments and callback into one `task_t`
>     with `bind_task()`, and pushes `{type, task}` onto the queue. A new queue gets a drain of that
>     type through `add_drain()`; a refused drain erases the queue and answers false.
>   - **`add_drain(queue_id, type)`**, with the queues' lock held, picks a worker of that type,
>     counts the drain on that worker, and queues it with `add_queued_task()`. It does not check
>     `running_`, so `wait()` runs a queue to its end.
>   - **`drain(queue_id, type, index)`**, on the worker, runs the queue's tasks one by one while
>     they are of its type. What a task or its callback throws goes to `on_task_error`, under the
>     worker's name, and the loop goes on. An empty queue is erased; a task of the other type gets a
>     drain of that type.
>   - **Stop:** a `stopped_` flag, set by `stop()` and cleared by `start()`. The drain checks it
>     between tasks and drops the rest with the "dropped N" warning. Not `running_`, which `wait()`
>     clears too and which must still drain.
>   - **Members:** `ordered_queues_`, a queue id to its `std::deque<ordered_task>`, where
>     `ordered_task` is `{task_type type; task_t task;}` - no `running` flag, as a queue is in the
>     map exactly while its drain runs; `ordered_queues_mutex_`; `next_queue_id_`;
>     `queued_in_order_` for `pending()`.
>   - **Recorded, not handled:** an allocation failure inside `add_drain()` can still leave a queue
>     stuck. A drain holds its worker until its queue empties or changes type; handing itself over
>     again after N tasks is the fix, if ever needed.
> - **9.4 · Docs:** `README.md`'s ordered queues section, in the shared executor's; every
>   function's parameters and return; `tools/make_doc.sh` clean; every file LF.
> - **9.5 · Checks:** Debug, ASan and TSan green; the ordered cases 50 times under TSan and ASan;
>   the bench builds; fluxcpp green against it.
> - **Order:** this repo, then fluxcpp's pin.

## Order

actuator → async → executor, each green and bumped before the next. Within this repo step 1 is a
rename and lands alone; 2 is the queue and is the one to settle first; 3 is the new door; 4 closes
the chain.

**What was the chain's one risk is gone.** An earlier draft flagged async's
`bind_action_and_method()` / `_function()` — they hold their action in a `std::shared_ptr` while
`bind_task()` takes it by value — as able to force the actuator's `bind_task` signature to change
after it had landed. Since `add_action()` no longer routes through `bind_task()`, those two are not
touched at all. Nothing downstream now constrains the actuator's steps 1 or 2.

**The actuator's step 6 is the open decision that gates the chain** — `is_connected()` extended, or
a new `has_tasks()` — and it gates async's step 3. Neither touches this repo.

## Working method

Inherited from `FIX_PLAN.md`, unchanged. **Each step is a test first**: a case that shows what is
missing, put up for review on its own, and the code written only once the case is agreed. The test
is the unit of review, not the implementation. **One step per commit**, and a step's case travels
with its own fix. The plan's own updates are their own commit, and always a later one.

**Every header change is followed by `tools/make_doc.sh`**, and all three repos are touched across
the chain.

## Progress

| Commit | Step |
|---|---|
| `1b4e2e1` | — this plan, in its pre-decision form |
| `25bf0ec` | — `async` bumped to `d90b28f`, the tip whose plan is closed |
| `e784324` | 1, 2 and 5 — the two doors, one queue, `actionT`, 5 cases, `README.md` and the reference |
| `1c3a203` | — this plan: steps 7 and 8 |
| `f0de732` | 7 — task types, light and heavy workers, 9 cases, `README.md` and the reference |
| `ab267f3` | 9.1 — `async` bumped to `7464139`: `add_queued_task()` public |

**CLOSED.** Every step is done or merged, and all of it is in `e784324`. 32 of 32 and 5 of 5 green,
clang-format clean, doxygen clean, `README.md` and `doc/refman.pdf` current.

**The chain is complete.** `actuator` at `ef93437` (99 of 99), `async` at `d90b28f` (62 of 62),
`executor` at `e784324` — 53 cases across the three, and the defect that started it is gone: a
trailing `std::function<void(R)>` silently dropped by every queueing layer. The two cases written
on 2026-09-24 against the abandoned shape are green in the shape that was chosen — callback last,
required, not forwarded to the action, and `void()` for work that produces nothing.

**`FIX_PLAN.md` step 21 remains open**, and is not this plan's: it asks a different question about
the same file.

## Appendix — the three cases, in the shape decided on 2026-09-25

Note what is no longer in them: the action signature is `std::function<int(int)>`, carrying no
callback parameter of its own. The callback is `add_task()`'s **last** argument, after the task's
own arguments — which is the call shape the abandoned convention had, for an entirely different
reason. There it was a trailing argument recognised by its type and silently ignored when the type
was wrong; here it is the last argument by position, required, and a `static_assert` when it cannot
serve.

```c++
/**
 * @brief A task notifies its callback with what it returned.
 *
 * Every worker is free as this adds, so the task goes straight to one rather than through the
 * queue - the shortest path the notification has to survive.
 */
TEST(executor_tests, a_task_notifies_its_callback_with_the_result) {
  using callback_executor = untangle::executor<std::function<int(int)>>;

  std::atomic_int reported = {-1};
  std::atomic_bool ran = {false};

  {
    callback_executor pool(2);
    pool.start();

    EXPECT_TRUE(pool.add_task(
        [&ran](int n) {
          ran = true;
          return n * 2;
        },
        21, [&reported](int result) { reported.store(result); }));
  }

  ASSERT_TRUE(ran.load()) << "the task itself never ran";
  EXPECT_EQ(reported.load(), 42) << "the pool ran the task but never notified its callback";
}

/**
 * @brief A callback survives the wait for a worker, and reports the result of the task it came with.
 *
 * The other half of an_argument_survives_the_queue: the single worker is held at the gate, so the
 * task below it waits in pending_ with its callback and is run from there.
 */
TEST(executor_tests, a_callback_survives_the_queue) {
  using callback_executor = untangle::executor<std::function<int(int)>>;

  std::atomic_int reported = {-1};
  gate blocker;

  {
    callback_executor pool(1);
    pool.start();

    // The only worker stops here, so what follows cannot be handed to one.
    EXPECT_TRUE(pool.add_task([&blocker](int n) { blocker(); return n; }, 0, [](int) {}));
    ASSERT_TRUE(wait_for([&blocker] { return blocker.arrived() == 1; }, 2s))
        << "the worker never reached the gate, so the task below was not queued";

    EXPECT_TRUE(pool.add_task([](int n) { return n * 2; }, 21,
                              [&reported](int result) { reported.store(result); }));

    EXPECT_EQ(pool.pending(), 1u) << "the task did not wait in the queue";

    blocker.open();
  }

  EXPECT_EQ(reported.load(), 42) << "a task run from the queue never notified its callback";
}

/**
 * @brief A void task notifies with nothing: finished is the whole message.
 *
 * The half no earlier case covered, and the reason a task's callback is required rather than
 * inferred - a task with no result still has a completion to report.
 */
TEST(executor_tests, a_void_task_notifies_that_it_finished) {
  std::atomic_int notified = {0};

  {
    executor pool(2);
    pool.start();

    for (int i = 0; i < 8; ++i) {
      EXPECT_TRUE(pool.add_task([] {}, [&notified] { notified.fetch_add(1); }));
    }
  }

  EXPECT_EQ(notified.load(), 8) << "a void task finished without saying so";
}
```
