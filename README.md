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

int main() {
  untangle::executor pool(4);

  for (int i = 0; i < 100; ++i) {
    pool.submit([i] { work(i); });
  }

  return 0;  // ~executor finishes what was submitted, then stops the workers
}
```

## how work is placed

N executions run in continuous mode behind a shared `std::deque` of tasks.

**Nothing overtakes.** A task goes straight to a free worker *only when the shared queue is empty*;
otherwise it joins the back of the queue. Finding a free worker does not let a task jump a queue
that already has work in it.

**Workers pull rather than being pushed to.** When one reports its queue finished — `on_finished`,
raised by `execution` the moment its list empties — it takes the front of the shared queue. That is
what spreads the work, with no scheduling logic of its own: 400 tasks over 4 workers come out
100/100/100/100.

**A worker is asked, not tracked.** `execution::is_busy()` is what `submit()` reads to find a free
worker and what the destructor reads to decide the pool is idle. A tally kept here would be the
pool's belief about its workers; this is the workers' own answer, and it cannot drift.

## lifetime and failure

**The destructor finishes what was submitted.** It refuses further work, waits for the queue to empty and for every
worker to go idle, and only then stops them. A task submitted before the pool goes out of scope has
run by the time it does.

**`submit()` says whether the task was taken.** It returns false once the pool is shutting down,
which a task submitting more work from inside the pool can see.

**A task that throws does not take the worker with it.** `execution` catches it, reports it on
stderr, and the worker carries on with the next task. Nothing reaches the submitter — a task that
has something to report carries its own channel, because a continuous worker does not collect
results.

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
