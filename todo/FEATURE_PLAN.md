# executor.hpp — tasks on the pool, feature plan

**The pool gains a second door.** Today `add_task()` is fire-and-forget: the pool runs the task, and
what it returns is dropped. After this chain a caller can hand the pool a **task** — an action bound
to its arguments *and* to the callback it must notify — and be told when it finished, with its
result if it has one.

**A task without a callback does not exist.** Decided 2026-09-25 in the actuator, and it is what
separates a task from an action rather than a restriction laid on top. The callback is a named
parameter, not a trailing argument the pool infers, and it is **not** forwarded to the task's
action.

**Status (2026-09-25) — nothing written, two cases written against the wrong shape, blocked on two
repos.** This is a feature plan, not a fix plan. Claims marked **PROBED** were compiled and run on
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
| `add_task()` | `add_task(task, callback, args...)` | yes, always, on a normal return |

So the existing method is **renamed** `add_action()` and `add_task()` becomes the new
callback-carrying one. It restores step 23's intent rather than reversing it: the pool's
fire-and-forget door is again spelled like async's, and the two repos now offer the same pair of
names.

**It costs one identifier at 52 call sites** — 46 in `executor_tests.cpp`, 6 in
`executor_smoke_test.cpp` — and none of them gains an argument. The alternative, making the existing
`add_task()` require a callback, costs an argument at all 52 and leaves the pool with no
fire-and-forget door at all.

> **Recommended, not settled.** Proposed 2026-09-25 and awaiting a decision. Everything below
> assumes it.

## What this does not delete — a correction to an earlier draft

An earlier version of this plan had `task_call` (`:289-301`) and `executor::bind_task` (`:304-312`)
deleted outright, some 35 lines, on the premise that the queue would hold one kind of entry.
**Withdrawn.** With two doors the pool has two kinds of queued work, and `task_call` is still the
carrier for the fire-and-forget one. `executor::bind_task` still binds it. What changes is that the
notifying door builds an `untangle::task<R>` through `untangle::bind_task()` instead.

## Step index

| # | Step | Sites | Evidence |
|---|---|---|---|
| 1 | `add_task()` renamed `add_action()`, at 52 call sites | `:171`, tests | naming decision |
| 2 | one FIFO queue, two kinds of entry | `:422`, `:323`, `:341-360` | **OPEN, see below** |
| 3 | `add_task(task, callback, args...)` onto `execution::add_task()` | beside `:171` | read-only |
| 4 | the two cases go green | `test/executor_tests.cpp` | **to be rewritten, see below** |
| 5 | `on_task_error`, the callback's thread, and what a refused task does not say | `:225-236`, `README.md` | decision |
| 6 | the `async` bump, `tools/make_doc.sh`, and `FIX_PLAN.md` step 21 | `doc/` | — |

### Step 2 · one queue, two kinds — OPEN

`pending_` (`:422`) is a `std::deque<task_call>`, and the order tasks come out of it is the order
they went in. Two kinds of entry must share that one queue, because **FIFO across both is the
behaviour, not an implementation detail**: a caller who posts an action and then a task expects them
to run in that order.

| Route | Cost |
|---|---|
| **`task_call` gains an optional callback slot** — private to the pool, filled only by `add_task()` | one queue, order preserved, `give_to_worker()` picks `add_action()` or `add_task()` on whether the slot is filled; the pool holds an optional callback internally while the public rule is that a task always has one |
| Two deques, `pending_actions_` and `pending_tasks_` | order across the two kinds is lost, and `pending()` stops having one answer |
| `std::deque<std::variant<task_call, untangle::task<R>>>` | order preserved, but every reader of the queue grows a visit, for a distinction only `give_to_worker()` cares about |

**The first, I think.** The optional slot is private and is exactly the shape async's queue has —
two kinds side by side — expressed in the one container the pool needs. Not decided.

### Step 4 · the two cases, and why they must be rewritten

Two cases were written on 2026-09-24, at `test/executor_tests.cpp:829-908`, and **they assert the
shape that was then abandoned**: a pool on `std::function<int(int, callback_t)>` with the callback
as a trailing argument the pool infers. Under decision (B) the action carries no callback parameter
and the callback is passed to `add_task()` directly, so both cases are wrong as written — not
failing for the right reason, but testing a design that will not be built.

They are uncommitted, and this plan's appendix carries the **new** shape. Two ways forward:

| Route | Cost |
|---|---|
| **Clean the tree; add the appendix cases at step 3** | the suite stays 27 of 27 green for the whole chain; the cases live in this file, committed, until there is an API for them to compile against |
| Rewrite them in place now | the binary stops compiling — `add_task(task, callback, args...)` does not exist — so all 27 other cases stop running too, for the length of a three-repo chain |

**The first.** The old plan's own note anticipated it: the cases are quoted in full below, so the
tree can be cleaned without losing them. The repo's precedent for leaving non-compiling cases live
(step 27, item 24) was a single-repo fix that landed the same day, not a chain across three.

The same pair belongs in async's suite (its step 5), because a caller using `execution` directly
meets this with no pool in sight.

### Step 5 · what the reference has to say

- **A callback runs on a worker's thread**, inside the drain, and several workers may be in one at
  once. The same warning `on_task_error` (`:230-236`) already carries, for the same reason.
- **A throwing callback reaches `on_task_error`**, because it runs inside the actuator's `try` and
  travels the path a failing task's exception does. So the handler can fire for a task whose body
  succeeded.
- **A task that throws does not notify. Finished does not mean failed**, decided 2026-09-25 and
  documented as a choice, with the revisit noted. An `exception_ptr` overload is the obvious shape
  if a use case asks; nothing here forecloses it.
- **A refused task does not notify.** `add_task()` answering `false` is the whole report — an
  unstarted, stopped or shutting-down pool never runs it, so nothing ever calls back. A caller
  waiting on the callback rather than on the answer would wait forever.
- **A queued task dropped at shutdown does not notify either.** `~executor()` reports dropped
  queued tasks on stderr (`:117-121`); with a callback in the picture that report is now the only
  thing a caller gets.
- `README.md` states the pool's surface and will be wrong on all of it; `FIX_PLAN.md` step 21
  already tracks that file drifting behind the code.

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
| — | nothing landed, and nothing can until `actuator` and `async` are both green and bumped |

**NEXT: not here.** The actuator's step 1.

## Appendix — the two cases, in the shape decision (B) calls for

Note what is no longer in them: the action signature is `std::function<int(int)>`, with no callback
parameter, and the callback is handed to `add_task()` in its own place.

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
        [&reported](int result) { reported.store(result); }, 21));
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
    EXPECT_TRUE(pool.add_task([&blocker](int n) { blocker(); return n; }, [](int) {}, 0));
    ASSERT_TRUE(wait_for([&blocker] { return blocker.arrived() == 1; }, 2s))
        << "the worker never reached the gate, so the task below was not queued";

    EXPECT_TRUE(pool.add_task([](int n) { return n * 2; },
                              [&reported](int result) { reported.store(result); }, 21));

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
