# executor

[![ci](https://github.com/popescun/executor/actions/workflows/ci.yml/badge.svg)](https://github.com/popescun/executor/actions/workflows/ci.yml)

*c++ thread pool executor based on async execution*

The interface is one header that exposes a pool of workers, each one an
[async](https://github.com/popescun/async) `execution` in continuous mode, fed by a shared queue of
tasks.

**This is the prototype, imported.** It was written in `async/prototypes/` to answer whether
`execution` is a usable building block for a pool — it is — and it is being hardened here. The
behaviour below is what the tests state today, not a settled interface.

## requirements

C++23, and the `async` submodule:

```sh
git submodule update --init --recursive
```

`async` carries its own `actuator` submodule, which is why the clone has to be recursive. Both are
header-only; nothing is built or installed.

The headers report warnings with `std::println`, so they need a standard library that provides
`<print>` — GCC 14 or newer.

## example

```c++
#include <executor.hpp>
#include <functional>

int main() {
  untangle::executor<std::function<void(void)>> pool(4);
  pool.start();

  for (int i = 0; i < 100; ++i) {
    pool.add_action([i] { work(i); });
  }

  return 0;  // ~executor finishes what was added, then stops the workers
}
```

## the action type

The pool is a template on the callable it runs, the way an `execution` is — and on the same
parameter name, because both kinds of queued work are that one callable. One pool runs one
signature, and the caller names it:

```c++
untangle::executor<std::function<int(void)>> pool(4);   // returns are run and dropped
```

The action type does not have to be a `std::function`. What an `execution` requires of it is a nested
`result_type`, which it reads rather than deduces, so any callable declaring one will do — and a
pool built on such a callable never touches `std::function::result_type`, which the standard removed
in C++20 and both libc++ and libstdc++ still supply by grace rather than contract.

```c++
struct my_task {
  using result_type = void;
  void operator()() const;
};

untangle::executor<my_task> pool(4);
```

An action type may take arguments, and `add_action()` binds them the way its namesake `execution::add_action()` does —
at the door, where the caller still holds them:

```c++
untangle::executor<std::function<int(int)>> pool(4);
pool.start();
pool.add_action([](int n) { return n * 2; }, 21);
```

What waits for a worker is one callable carrying its own arguments, so nothing about a task changes
between the queue and the worker that runs it. The arguments are **copied** at `add_action()` and
handed to the task as the pool's own lvalues when it runs: a task is called later, possibly much
later, and what the caller passed may be gone by then. A task taking `int&` therefore mutates the
pool's copy, which the caller never sees.

A task that cannot be called with the arguments given is refused by a `static_assert` in
`add_action()`, rather than by an error from inside the binding.

## tasks: work that reports back

Everything above queues work through `add_action()`, which is fire and forget: it runs, what it returns is dropped, and the caller hears nothing more about it. `add_task()` carries the callback it must notify:

```c++
untangle::executor<std::function<int(int)>> pool(4);
pool.start();

// the callback comes last, after the arguments the task is bound to
pool.add_task([](int n) { return n * 2; }, 21, [](int result) { /* result == 42 */ });
```

Work that returns `void` still reports that it finished, with a callback taking nothing — *finished* is the message and the result is optional:

```c++
untangle::executor<std::function<void(void)>> pool(4);
pool.add_task([] { /* ... */ }, [] { /* finished */ });
```

**The callback is the last argument, and the signature cannot say so.** A parameter pack cannot be followed by a deducible parameter, so it arrives inside the arguments and `untangle::bind_task()` splits it off. It must be callable with the task's result and return nothing — or callable with nothing at all, when the task returns void. A missing or unusable one is a compile error rather than silence.

**Both doors share one queue, and the order out is the order in.** An action posted before a task runs before it. The pool can promise that because it owns one container; `async::execution`, which the workers are, fires every action in a pass before any task, so its ordering across the two kinds differs. Only the pool's own order is a promise to a caller of the pool.

**`pending()` counts both kinds**, as it always counted work waiting for a worker.

**A refused task does not notify.** `add_task()` answers `false` under exactly the conditions `add_action()` does — before `start()`, after `stop()`, once the destructor has begun, or if the worker it was offered to has stopped — and a refused task is destroyed rather than queued. The answer is the whole report, so a caller waiting on the callback rather than on the answer would wait for ever. The same is true of work still queued when a stopped pool is destroyed: it never runs, so it never notifies, and the count on stderr is all there is.

**Finished does not mean failed.** A task that throws is *not* notified: there is no result to hand over, and for a void task no completion to report either. What it threw goes to `on_task_error`, as a failing action's does. And because a callback runs inside the same `try` as the task that owns it, `on_task_error` can fire for a task that in fact succeeded — the work was done and only the telling failed.

**A callback runs on a worker's thread**, inside that worker's drain, and several may be running at once on different workers. It is the same rule `on_task_error` carries, for the same reason: nothing may escape it.

## how work is placed

N executions run in continuous mode behind a shared `std::deque` of tasks.

**Nothing overtakes.** A task goes straight to a free worker *only when the shared queue is empty*;
otherwise it joins the back of the queue. Finding a free worker does not let a task jump a queue
that already has work in it.

**Workers pull rather than being pushed to.** When one reports its queue finished — `on_finished`,
raised by `execution` the moment its list empties — it takes the front of the shared queue. That is
what spreads the work, with no scheduling logic of its own: 400 tasks over 4 workers come out
100/100/100/100.

**A worker is asked, not tracked.** `execution::is_busy()` is what `add_action()` reads to find a free
worker and what the destructor reads to decide the pool is idle. A tally kept here would be the
pool's belief about its workers; this is the workers' own answer, and it cannot drift.

## running, stopping, restarting

**A pool is built stopped.** The constructor makes the workers and wires them up; `start()` runs
them. Until it is called the pool refuses work, because a task taken then would sit unrun and the
destructor would wait for it. It is also the moment to assign `on_task_error`, before anything can
throw.

**`stop()` stops the workers**, and stops the pool accepting. What a worker is already running it
finishes; what is still queued stays queued. `stop()` does not wait for the workers to leave their
threads — only the destructor does that, and that wait is what makes destroying the pool safe.

**`start()` after `stop()` restarts it**, workers and all, and the queue it was holding drains as
they pick it up again. Either call is harmless twice over.

## lifetime and failure

**The destructor finishes what was added.** It stops accepting, waits for the queue to empty and for
every worker to go idle, and only then stops them. A task added before the pool goes out of scope
has run by the time it does.

A pool destroyed while stopped is the exception: its queue can never empty, because only a running
worker takes from it, so the destructor does not wait for it. Whatever is left is reported on
stderr and dropped.

**Both waits are unbounded, and both say so while they last.** A task that never returns would
otherwise leave the destructor silent for ever, so it reports instead — the first wait naming who
is still inside a task, the second naming whose thread has not left:

```
executor: still waiting to finish after 1s - 1 queued, 2 in a task: pool_worker_0, pool_worker_1
executor: still waiting to finish after 3s - 0 queued, 1 in a task: pool_worker_1
executor: still waiting to stop after 1s - 1 not left its thread: pool_worker_1
```

The interval starts at a second and doubles to a ceiling of thirty, so a stuck teardown says
something almost at once without a long one becoming a flood. The two waits are kept apart because
they answer different questions: a worker can be idle and still be in its thread.

Bounding either wait was never available. Returning early while a worker is still in a task is a
use-after-free, and a bounded wait only moves the hang into `~execution()`, which spins on its own
running state with no timeout. Workers are detached and `async::execution` has no cancellation, so
a pool cannot outlive a task it cannot interrupt. What can be fixed is the silence.

**`add_action()` says whether the task was taken.** It returns false before `start()`, after `stop()`,
and once the destructor has begun. It returns false again if the worker it was offered to has
stopped. A refused task is destroyed rather than queued, so a false answer means it will not run.

**A task that is still running is refused too**, and that is chosen rather than incidental. A pool
shutting down is waiting for exactly that task, so it is tempting to let the work it spawns through
— but then a task that re-adds itself could keep the shutdown from ever ending. One rule instead: a
pool that is not running takes no work, whoever is asking. `add_action()` answers false inside the
task, so it can tell.

**A task that throws does not take the worker with it**, and what it threw is not lost. The worker
catches it and carries on with the next task; `on_task_error` is where the throw goes:

```c++
untangle::executor<std::function<void(void)>> pool(4);

pool.on_task_error = [](std::exception_ptr thrown) {
  try {
    std::rethrow_exception(thrown);
  } catch (const std::exception& e) {
    log(e.what());
  }
};
```

It carries a `std::exception_ptr` because a task may throw something that is not a `std::exception`,
and that is the case most worth hearing about. Assign it before the first `add_action()` — a worker
reads it — and expect it on a worker's thread, with more than one worker possibly inside it at once.
Leave it unset and the pool warns on stderr instead, naming the worker.

**That name is not unique between pools.** Workers are named by index — `pool_worker_0`,
`pool_worker_1` — so two pools in one process each have a `pool_worker_0`, and every warning that
names one is ambiguous: the failure reports above, and the destructor's waits alike. With a single
pool the name says which worker; with two it does not. Assigning `on_task_error` sidesteps it for
failures, since the handler is the pool's own and the warning is replaced.

A *return value* is still not collected: a continuous worker keeps none. Work with something to
report carries its own channel, which is what `add_task()` is for — see *tasks: work that reports
back* above.

## building the tests

```sh
cmake -S test -B test/build
cmake --build test/build
ctest --test-dir test/build
```

`test/CMakeLists.txt` fetches googletest at configure time, so the first configure needs network
access.

### sanitizers

`EXECUTOR_SANITIZE` builds the tests under a sanitizer. It is off by default, because a sanitized
build is several times slower and ThreadSanitizer does not ship for every toolchain.

```sh
cmake -S test -B test/build-asan -DEXECUTOR_SANITIZE=address
cmake --build test/build-asan
ctest --test-dir test/build-asan
```

Accepted values are `address`, `thread`, `undefined`, or empty. Anything else is refused at
configure time rather than passed through to the compiler.

**Use a separate build directory per sanitizer.** `address` and `thread` instrument the same
accesses in incompatible ways and cannot be combined, so one build directory is one sanitizer.

## the reference

`tools/make_doc.sh` builds `doc/refman.pdf` from the header and this README. It needs doxygen,
tectonic and python3, and it fails on any doxygen warning that is not an obsolete-tag notice, so an
undocumented member or a broken `\ref` does not reach the reference.
