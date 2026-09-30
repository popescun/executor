# executor

[![ci](https://github.com/popescun/executor/actions/workflows/ci.yml/badge.svg)](https://github.com/popescun/executor/actions/workflows/ci.yml)

*c++ thread pool executor based on async execution*

A header-only library: `executor.hpp` exposes a pool of workers, each one an
[async](https://github.com/popescun/async) `execution` in continuous mode, fed by a shared queue of
tasks.

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
    pool.add_task([i] { work(i); }, [] { /* finished */ });
  }

  return 0;  // ~executor finishes what was added, then stops the workers
}
```

## the action type

The pool is a template on the callable it runs, the way an `execution` is — and on the same
parameter name, because every task queued is that one callable. One pool runs one
signature, and the caller names it:

```c++
untangle::executor<std::function<int(void)>> pool(4);   // each result goes to the task's callback
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

## tasks: work that reports back

Work enters the pool through one door, `add_task()`, and every task carries the callback it must
notify. An action type may take arguments, and `add_task()` binds them the way its namesake
`execution::add_task()` does — at the door, where the caller still holds them — with the callback
after them:

```c++
untangle::executor<std::function<int(int)>> pool(4);
pool.start();

// the callback comes last, after the arguments the task is bound to
pool.add_task([](int n) { return n * 2; }, 21, [](int result) { /* result == 42 */ });
```

What waits for a worker is one callable carrying its own arguments and its callback, so nothing
about a task changes between the queue and the worker that runs it. The arguments are **copied** at
`add_task()` and handed to the task as the pool's own lvalues when it runs: a task is called later,
possibly much later, and what the caller passed may be gone by then. A task taking `int&` therefore
mutates the pool's copy, which the caller never sees.

A task that cannot be called with the arguments given is refused by a `static_assert` in
`add_task()`, which names the mismatch before the errors from inside the binding follow it.

Work that returns `void` still reports that it finished, with a callback taking nothing — *finished* is the message and the result is optional:

```c++
untangle::executor<std::function<void(void)>> pool(4);
pool.add_task([] { /* ... */ }, [] { /* finished */ });
```

A task with nothing to bind passes the callback alone, whatever it returns:

```c++
untangle::executor<std::function<int(void)>> pool(4);
pool.add_task([] { return 42; }, [](int result) { /* result == 42 */ });
```

**The callback is the last argument, and the signature cannot say so.** A parameter pack cannot be followed by a deducible parameter, so it arrives inside the arguments and `untangle::bind_task()` splits it off. It must be callable with the task's result and return nothing — or callable with nothing at all, when the task returns void. A missing or unusable one is a compile error rather than silence.

**Work nobody wants told about still passes a callback**, one that ignores what it is given. There is no fire-and-forget door: a void task's callback costs one empty call, and a single door means a single queue, in which the order out is the order in.

```c++
constexpr auto ignore_result = [](auto&&...) {};   // takes a result, or nothing for a void task
pool.add_task([](int n) { return n * 2; }, 21, ignore_result);
```

**A refused task does not notify.** `add_task()` answers `false` before `start()`, after `stop()`, while a `wait()` is draining, once the destructor has begun, or if the worker it was offered to has stopped — and a refused task is destroyed rather than queued. The answer is the whole report, so a caller waiting on the callback rather than on the answer would wait for ever. The same is true of work still queued when a stopped pool is destroyed: it never runs, so it never notifies, and the count on stderr is all there is.

**Finished does not mean failed.** A task that throws is *not* notified: there is no result to hand over, and for a void task no completion to report either. What it threw goes to `on_task_error`. And because a callback runs inside the same `try` as the task that owns it, `on_task_error` can fire for a task that in fact succeeded — the work was done and only the telling failed.

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

**A worker is asked, not tracked.** `execution::is_busy()` is what `add_task()` reads to find a free
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

**`wait()` is a barrier, not a shutdown.** It returns once everything added has run and no worker is
busy, and it leaves the pool exactly as it found it — workers still in their threads, nothing
destroyed, accepting again. Work added after it runs as it did before, and calling it repeatedly is
how you step through work in phases:

```c++
pool.add_task([] { first(); }, [] {});
pool.wait();                                // first() has run, the pool is still alive
pool.add_task([] { second(); }, [] {});
pool.wait();                                // and so has second()
```

**It refuses new work while it waits**, then restores accepting to whatever it was. A task that
re-adds itself would otherwise keep the drain from ever ending, so `add_task()` answers `false`
for the duration — and because the previous state is restored rather than assumed,
`wait()` on a stopped pool does not leave it accepting.

So only work accepted *before* the wait began is what the wait covers. Work offered during it was
never queued, and work added after it returns is simply the next batch, for the next `wait()`. This
is a stop-the-world drain, not a transparent one: a producer on another thread sees `add_task()`
answer `false` while a `wait()` is in progress, and has to offer the work again afterwards.

**A pool that is not running is not waited for.** Only a worker takes from the queue, so a stopped
pool can never empty it, and a wait insisting otherwise would never return. `wait()` returns at once
instead, leaving what is queued for the next `start()`.

## lifetime and failure

**The destructor finishes what was added.** Its drain *is* `wait()`, so a task added before the pool
goes out of scope has run by the time it does. What the destructor adds is the shutdown `wait()`
deliberately leaves out, in the order that makes it safe: stop the workers, wait for every one of
them to leave its thread, and only then destroy them.

**The destructor waits for the pool, not for what the pool's work touched.** That distinction is the
whole reason `wait()` is public. The drain guarantees no worker is still running when the pool is
destroyed, which is a statement about the *pool's own* members — a destructor body runs before
them, so they are all still alive for it. It says nothing about the lifetime of what a task
captured.

For a pool that is a local variable outliving what its work touches, the destructor is enough and
nothing else is needed. For a pool held as a **member of another class**, it is already too late:
`~executor()` runs during its owner's member destruction, so by then the owner's derived subobject
is gone, and so is every member declared after the pool. A callback reaching into either of those
runs against destroyed storage however faithfully the pool drains — and on the stack that is not
even a fault a sanitizer can name, because the memory is still addressable.

So an owner whose callbacks reach back into itself calls `wait()` where the whole object graph is
still standing — in its own destructor body, ahead of its members, or better still at a point the
owner of *its* peers controls — and the destructor's drain goes back to being a backstop rather
than the thing relied upon.

A pool destroyed while stopped is the exception: its queue can never empty, because only a running
worker takes from it, so the destructor does not wait for it. Whatever is left is reported on
stderr and dropped.

**Both waits are unbounded, and both say so while they last.** A task that never returns would
otherwise leave the destructor silent for ever, so it reports instead — the first wait naming who
is still inside a task, the second naming whose thread has not left. The first line below is the
drain's, so a plain `wait()` prints it exactly as a destructor does; the second is the destructor's
alone:

```
executor: still waiting to drain after 1s - 1 queued, 2 in a task: pool_worker_0, pool_worker_1
executor: still waiting to drain after 3s - 0 queued, 1 in a task: pool_worker_1
executor: still waiting to stop after 1s - 1 not left its thread: pool_worker_1
```

The interval starts at a second and doubles to a ceiling of thirty, so a stuck teardown says
something almost at once without a long one becoming a flood. The two waits are kept apart because
they answer different questions: a worker can be idle and still be in its thread.

Bounding either wait was never available. Returning early while a worker is still in a task is a
use-after-free, and a bounded wait only moves the hang into `~execution()`, which spins on its own
running state with no timeout. Workers are detached and `async::execution` has no cancellation, so
a pool cannot outlive a task it cannot interrupt. What can be fixed is the silence.

**`add_task()` says whether the task was taken.** It returns false before `start()`, after
`stop()`, while a `wait()` is draining, and once the destructor has begun. It returns false again if
the worker it was offered to has stopped. A refused task is destroyed rather than queued, so a false
answer means it will not run.

**A task that is still running is refused too**, and that is chosen rather than incidental. A pool
draining is waiting for exactly that task, so it is tempting to let the work it spawns through — but
then a task that re-adds itself could keep the drain from ever ending, and that holds for a `wait()`
just as for a destructor. One rule instead: a pool that is not running takes no work, whoever is
asking. `add_task()` answers false inside the task, so it can tell.

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
and that is the case most worth hearing about. Assign it before the first `add_task()` — a worker
reads it — and expect it on a worker's thread, with more than one worker possibly inside it at once.
Leave it unset and the pool warns on stderr instead, naming the worker.

**That name is not unique between pools.** Workers are named by index — `pool_worker_0`,
`pool_worker_1` — so two pools in one process each have a `pool_worker_0`, and every warning that
names one is ambiguous: the failure reports above, and the destructor's waits alike. With a single
pool the name says which worker; with two it does not. Assigning `on_task_error` sidesteps it for
failures, since the handler is the pool's own and the warning is replaced.

A *return value* is not collected by the pool: a continuous worker keeps none. It goes to the
task's own callback — see *tasks: work that reports back* above.

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
