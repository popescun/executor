# executor.hpp — the pool on tasks, feature plan

**A task is an action bound to its arguments**, and after this chain the actuator carries them,
async queues them, and the pool stops hand-rolling its own. This plan is the last of three: what
the pool deletes once `untangle::task<R>` and `untangle::bind_task()` exist, and the two cases that
say the whole chain worked.

**Status (2026-09-25) — nothing written, two cases written and failing, blocked on two repos.**
This is a feature plan, not a fix plan: no step below is a defect in code meant to do something
else. The defect at its root is async's, and the pool only inherits it. Claims marked **PROBED**
were compiled and run on 2026-09-24/25; the rest are read-only and say so.

**Baseline.** `executor` at `c2bf9f0`, `async` at `985a991`, `actuator` at `a8b8b47`. Sites are
line numbers at those commits and move with every step that lands.

## The chain

| Repo | Plan | Holds |
|---|---|---|
| `actuator` | `async/actuator/todo/FEATURE_PLAN.md` | the mechanism: `task<R>`, `bind_task()`, `add_task()`, `call_tasks()`, the convention as a concept |
| `async` | `async/todo/FEATURE_PLAN.md` | the queue: `queued_action_t` deleted, `add_action()` onto `bind_task()`, the drain onto `call_tasks()` |
| `executor` | this file | the pool: `task_call` deleted, `pending_` on tasks, and the acceptance criteria |

Steps are numbered per repo, as in `FIX_PLAN.md`; cross-repo references are qualified ("async's
step 5"). Each repo is green and bumped before the next starts. **Nothing here can be written until
both of the others have landed** — which is the point of putting the acceptance criteria at this
end.

## Why — a defect the pool inherits

The actuator fires a trailing `std::function<void(R)>` with the action's result, reading it out of
the **invocation** argument pack. Every queueing layer binds that pack into a nullary callable
first, so the pack the actuator reads is empty and the callback is never fired:

| Path | What the actuator is invoked with | Callback |
|---|---|---|
| `connect(action)` then `a(21, cb)` | `(21, cb)` | **fired** — 42 |
| `execution::add_action(action, 21, cb)` | `()` — `std::bind` sealed the pack (`async.hpp:496`) | lost |
| `executor::add_task(task, 21, cb)` | `()` — `bind_task` sealed it again (`executor.hpp:304`) | lost |

**PROBED.** The task runs and the callback is silently skipped; nothing warns, because under the
convention the callback is also a legitimate argument of the task, so `add_task()`'s `static_assert`
(`:170`) is satisfied. **The pool buries it a second time**, which is why fixing async alone would
not reach here: `bind_task` (`:304`) folds the callback into `task_call` before `add_action()` is
ever called.

## What the pool gives up

Almost entirely deletion. Roughly 35 lines, comments included, stop having a reason to exist:

| Today | After |
|---|---|
| `struct task_call` (`:289-301`) — exists *only* to name a `result_type` that C++20 removed from `std::function` | deleted; `task<R>` names one |
| `executor::bind_task()` (`:304-312`) | deleted; it is `untangle::bind_task()` line for line |
| `std::deque<task_call> pending_` (`:422`) | `std::deque<untangle::task<R>>` |
| `give_to_worker()` → `add_action()` (`:323-326`) | → `add_task()` |
| `using worker_execution = execution<task_call>` (`:315`) | `execution<taskT>` — `taskT::result_type` is still the `R` everything is in terms of |

**`bind_task` moving down is not a loss of anything.** The pool wrote it privately because it
needed it; the actuator's copy is the same lambda with the callback extracted before the pack is
sealed. That extraction is the entire feature.

## Step index

| # | Step | Sites | Evidence |
|---|---|---|---|
| 1 | `task_call` deleted | `:289-301`, `:315` | read-only |
| 2 | `executor::bind_task` deleted for `untangle::bind_task` | `:304-312` | read-only |
| 3 | `pending_` holds tasks; `give_to_worker()` calls `add_task()` | `:323`, `:422` | read-only |
| 4 | the two red cases go green | `test/executor_tests.cpp:829-908` | **written, failing** |
| 5 | `on_task_error`, and which thread a callback runs on — the reference | `:225-236`, `README.md` | decision |
| 6 | the `async` bump, `tools/make_doc.sh`, and `FIX_PLAN.md` step 21 | `doc/` | — |

### Step 4 · the two cases that close the chain

Written 2026-09-24, in `test/executor_tests.cpp:829-908`, live rather than disabled, under a
comment in the style of the step 27 block above them. **They fail today** — the suite is 27 of 29:

```
[ FAILED ] executor_tests.a_trailing_callback_is_invoked_with_the_task_result
[ FAILED ] executor_tests.a_callback_survives_the_queue
  reported.load() Which is: -1   vs   42
```

The first takes the free-worker path; the second is held behind a `gate` so the task waits in
`pending_` and runs from the queue — the same pair of paths `an_argument_survives_the_queue`
(`:751`) covers for arguments. Both use a pool on
`std::function<int(int, std::function<void(int)>)>`, which compiles today and does nothing.

**They are not committed, and they should not be dropped.** The rule in `FIX_PLAN.md` is that a
step's case travels with its own fix, so they belong to this step's commit, not to an earlier one.
Until then they sit in the working tree and the suite is red on purpose. If a red suite across a
chain this long is unwanted, the cheaper move is to commit **this plan** first: both cases are
quoted in full in the appendix, so the tree can be cleaned without losing them. Dropping them with
nothing written down is the only option that costs something.

The same pair belongs in async's suite (its step 6), because the defect is async's — a caller using
`add_action()` directly has it today with no pool in sight.

### Step 5 · what the reference has to say

Decided in the actuator's step 5 and async's step 7; this repo owes the caller-facing half:

- **A callback runs on a worker's thread**, inside the drain, and several workers may be in one at
  once. The same warning `on_task_error` (`:230-236`) already carries, for the same reason.
- **A throwing callback reaches `on_task_error`**, because it runs inside the actuator's `try` and
  its exception travels the path a failing task's does. So the handler can fire for a task whose
  body succeeded.
- **A task that throws gets no callback**, because no result exists to report.
- `README.md` states the pool's surface and will be wrong on all three; `FIX_PLAN.md` step 21
  already tracks that file drifting behind the code.

## Order

actuator → async → executor, each green and bumped before the next. Within this repo 1 to 3 are one
change in three reviewable pieces — the file does not build between them — and 4 is the whole
chain's acceptance criterion.

**What can send the chain back is async's step 5**, not anything here:
`bind_action_and_method()` / `_function()` hold their action in a `std::shared_ptr` while
`bind_task()` takes it by value. If the wrapper that reconciles them constrains `bind_task`'s
signature, the actuator's step 3 is rewritten after it has landed. It is read first and settled
first in both of the other plans.

**The actuator's step 7 is the other open decision** — `is_connected()` extended, or a new
`has_tasks()` — and it gates async's step 4. Neither touches this repo.

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
| — | nothing landed, and nothing can until `actuator` and `async` are both green and bumped |

**NEXT: not here.** The actuator's step 1, after async's step 5 has settled the wrapper.

## Appendix — the two cases, verbatim

```c++
TEST(executor_tests, a_trailing_callback_is_invoked_with_the_task_result) {
  using callback_t = std::function<void(int)>;
  using callback_executor = untangle::executor<std::function<int(int, callback_t)>>;

  std::atomic_int reported = {-1};
  std::atomic_bool ran = {false};

  {
    callback_executor pool(2);
    pool.start();

    EXPECT_TRUE(pool.add_task(
        [&ran](int n, const callback_t&) {
          ran = true;
          return n * 2;
        },
        21, callback_t([&reported](int result) { reported.store(result); })));
  }

  ASSERT_TRUE(ran.load()) << "the task itself never ran";
  EXPECT_EQ(reported.load(), 42) << "the pool ran the task but never invoked its callback";
}

TEST(executor_tests, a_callback_survives_the_queue) {
  using callback_t = std::function<void(int)>;
  using callback_executor = untangle::executor<std::function<int(int, callback_t)>>;

  std::atomic_int reported = {-1};
  gate blocker;

  {
    callback_executor pool(1);
    pool.start();

    // The only worker stops here, so what follows cannot be handed to one.
    EXPECT_TRUE(pool.add_task(
        [&blocker](int n, const callback_t&) {
          blocker();
          return n;
        },
        0, callback_t([](int) {})));
    ASSERT_TRUE(wait_for([&blocker] { return blocker.arrived() == 1; }, 2s))
        << "the worker never reached the gate, so the task below was not queued";

    EXPECT_TRUE(pool.add_task([](int n, const callback_t&) { return n * 2; }, 21,
                              callback_t([&reported](int result) { reported.store(result); })));

    EXPECT_EQ(pool.pending(), 1u) << "the task did not wait in the queue";

    blocker.open();
  }

  EXPECT_EQ(reported.load(), 42) << "a task run from the queue never invoked its callback";
}
```
