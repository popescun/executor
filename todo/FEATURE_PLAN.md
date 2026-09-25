# executor.hpp — tasks on the pool, feature plan

**The pool gains a second door.** Today `add_task()` is fire-and-forget: the pool runs the task, and
what it returns is dropped. After this chain a caller can hand the pool a **task** — an action bound
to its arguments *and* to the callback it must notify — and be told when it finished, with its
result if it has one.

**A task without a callback does not exist.** Decided 2026-09-25 in the actuator, and it is what
separates a task from an action rather than a restriction laid on top. The callback is the **last
argument**, taken by position rather than recognised by type, and it is **not** forwarded to the
task's action.

**Status (2026-09-25) — steps 1, 2 and 5 are done, 32 of 32 and 5 of 5 green; steps 3 and 4 were
merged into step 2. `actuator` and `async` are both closed and bumped. Only step 6's commits are
left.** This is a feature plan, not a fix plan. Claims marked **PROBED** were compiled and run on
2026-09-24/25; the rest are read-only and say so.

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
| 1 ✅ | `add_task()` renamed `add_action()`, at 51 call sites | `:145-179`, tests, `README.md` | CONFIRMED (27 + 5 green) — **DONE** (`e87da41`) |
| 2 ✅ | one FIFO queue, two kinds, `add_task()`, and the cases | `:193-229`, `:326-344`, `:377-395`, `:403-414` | CONFIRMED (5 cases) — **DONE** (`e87da41`) |
| 3 | — merged into step 2, see below | — | — |
| 4 | — merged into step 2, see below | — | — |
| 5 ✅ | `on_task_error`, the callback's thread, the destructor, and `README.md` | `:87-96`, `:288-308`, `README.md` | transcription — **DONE** (`e87da41`) |
| 6 | `README.md`, `tools/make_doc.sh`, `FIX_PLAN.md` step 21, and the commits | `doc/` | — |

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
| `e87da41` | 1, 2 and 5 — the two doors, one queue, `actionT`, 5 cases, `README.md` and the reference |

**NEXT: step 6**, and only its commits. `tools/make_doc.sh` has run at every step and `README.md`
is current for this feature. **`FIX_PLAN.md` step 21 stays open and is not this plan's** — it asks
a different question about the same file.

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
