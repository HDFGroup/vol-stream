# vol-stream: Python subscriber plan

The design reasoning is in RFC-VOLSTREAM-2026-001 `sec:pysub` ("A Python
Subscriber"). This is the build plan for it: what to write, in what order,
and what each stage has to prove before the next one starts.

Same two rules as [`dev-plan.md`](dev-plan.md). No changes to HDF5, and
borrow by default — which here means the extension wraps the connector's
existing C API and adds no protocol.

> **Status (2026-09-24): P0–P5 are implemented and pass CI**, each milestone's
> "As built" note records where it departed from this plan, and
> [Known gaps](#known-gaps) lists what remains. Two connector changes came
> out of this work and were made in C rather than worked around:
> `H5Fack_stream_step()` (backpressure for subscribers) and end of stream for
> subscribers; a third, capturing attribute writes through a handle kept
> open across steps, was a bug the binding's tests found.

## Both premises re-verified, 2026-09-23

`sec:pysub-not-h5py` rejects an h5py binding for two reasons. Neither was
taken on trust; both were checked against the current h5py before planning
any work.

| Premise | Check | Result |
|---|---|---|
| h5py has no `H5VLfile_optional_op()` binding | `dir(h5py.h5f)`, `import h5py.h5vl` | No optional-op names; **no `h5py.h5vl` module at all**. Still true at h5py 3.16.0 |
| An h5py wheel vendors its own libhdf5 | `otool -L` on h5py's `h5f*.so` | `@loader_path/.dylibs/libhdf5.320.0.0.dylib` — **vendored, not shared** |

The second matters more than the version number suggests. This h5py reports
HDF5 2.0.0, which is the version the connector requires — and it is still
unusable, because it is a *separate loaded copy* of libhdf5 with its own
identifier table. An `hid_t` from it means nothing to the connector's
library. Matching versions do not make two libhdf5 instances one.

`sec:pysub-gate` left open "whether an HDF5 2.x-based Python environment
exists at the target site at all, which determines whether the standalone
design is a convenience or a necessity." On this machine the answer is
both: a 2.x Python environment exists, *and* the standalone design is a
**necessity** rather than a convenience, because the 2.x it has is
vendored inside a wheel.

## Scope

**Subscriber only.** A Python consumer does not produce steps, so the
writer side stays out — that halves the surface, and `sec:pysub-not-h5py`
already argues it.

Out of scope, explicitly: the writer API, h5py interop (addable later,
nothing depends on it), device-direct buffers (that is bulk-transfer
Phase 3 — but see "The Phase 3 seam" below, which must not be foreclosed),
and Windows.

## The call surface

`sec:pysub`'s table, with one addition. As built (see the P milestones for why
each differs from the original):

| Python | Wraps |
|---|---|
| `open(path, backpressure=)` / `File.close()`, context manager | `H5Fopen()` with the connector on the FAPL, `H5Fclose()` |
| `follow(path, selections, backpressure=, deflate=)` | `open()` plus `subscribe()`, to every numeric dataset by default |
| `File.schema()` | `H5Fget_stream_schema()` / `H5Ffree_stream_schema()`, returning `{path: Var(dtype, shape, is_attr)}` |
| `File.subscribe(selections, deflate=)` | `H5Fsubscribe()`, a DCPL for `deflate` |
| `File.subscribe_type(path, dtype)` | `H5Fsubscribe_type()` |
| `File.subscribe_predicate(path, op, value)` | `H5Fsubscribe_predicate()` |
| `File.next_step()`, `File.steps()`, `for step in f` | `H5Fwait_step_ready()` plus a zero-timeout drain of `H5Fget_subscribed_data()`, reassembled per step |
| `File.get()` | `H5Fget_subscribed_data()`, one raw push |
| `File.end_of_stream` | `H5Fstep_status()` reporting `H5F_STEP_EOS` |
| (with `backpressure=True`) | `H5Fack_stream_step()` after each step |
| `volstream.torch.StreamDataset` | `follow()` wrapped as a PyTorch `IterableDataset` |

`logical_steps()` and `step_status()` from the original table were not built:
logical-step navigation belongs to the reader cursor (`H5Fbegin_step()`), which
this subscriber does not use, and the only step state a subscriber needs is
end of stream.

**`schema()` is the addition, and it should arguably lead.** The RFC's
table predates M10. `H5Fget_stream_schema()` is what makes a *generic*
consumer possible — it returns real decoded type and space ids, so a
consumer that has never opened the file can size a buffer and build a
selection from nothing but a filename. That matters more in Python than in
C: the idiomatic Python consumer is `for arr in stream:`, and asking a
user to declare shapes that the stream already knows is exactly the
friction the C examples had to live with before M10.
`examples/detector_pipeline`'s `discover` mode is the C precedent.

No `hid_t` crosses into Python, per `sec:pysub-not-h5py`. Selections
arrive as shapes and start/count tuples and become `H5Screate_simple()` +
`H5Sselect_hyperslab()` inside the extension; `schema()` returns NumPy
dtypes and shape tuples, not ids.

## Binding technology: the CPython C API

| Option | Verdict |
|---|---|
| **Raw CPython C API** | **Chosen.** Matches `sec:pysub-gate`'s "one C extension whose only dependencies are NumPy and this connector." Full control of the three things that actually matter here: GIL release, signal checking, and buffer ownership |
| Cython | Less boilerplate, but adds a build-time dependency and a second language to a C codebase, for ~8 functions |
| pybind11 | C++ in a C codebase. Overkill |
| ctypes / cffi | No build step at all, but the struct layout of `H5F_stream_var_t` would have to be mirrored by hand and would rot against the header, and it gives no natural home for the Phase 3 seam |

## The three risks

`sec:pysub-gate` ranks them. Their status has changed since it was written.

### 1. Deterministic teardown — cannot be deferred

`sec:pysub-ordering` is blunt about this: the standard Python idiom is
actively wrong. A generator `__iter__` with cleanup in `finally` does not
run that cleanup when the training loop `break`s, and the writer hangs.

This is *in the exit gate*, not after it — the gate requires clean exit on
"**both** the run-to-completion path and the early-`break` path." So it
cannot be a later phase. Teardown lands in P3, before the Pythonic layer
that would make it easy to get wrong.

### 2. Multi-run reassembly — answered by the transport, pinned by P0

No test asks "is this step's set of pushes complete."
`test/t_subvolume_strided.c` is the fragmented-selection test ("one
column: ROWS separate flat runs"), and it validates **in aggregate across
the whole run**.

Predicting the push count from the selection is unsound: RFC appendix A's
findings F4 and F5 show the writer *coalesces* past a run cap. Waiting for
a push of step *N+1* to arrive, to show that step *N* is complete, works
but has a gap: with a bounded timeout, "step *N* has no more pushes" and
"step *N+1* has not started" look the same.

That gap does not need closing, because the transport already makes a
stronger guarantee (`src/tr_mercury.c`, `vs_tr_writer_broadcast_step_ready()`):

- The writer finishes delivering step *N*'s pushes (`vs_tr_drain_pushes()`)
  before it sends step *N*'s step-ready notification. The reader's push
  handler queues each item before it responds, so "delivered" means
  "queued on the reader."
- So when `H5Fwait_step_ready()` returns *N*, every delivered push for *N*
  is already in the data queue. Draining it with a zero timeout gets the
  complete step. There is no need to wait for a push of *N+1*.
- Pushes never interleave across a step boundary: step *N*'s pushes all
  finish before step *N+1*'s are issued.

What this does **not** give, and the reassembly layer must handle:

- **A lagging reader already has later steps queued.** If the consumer
  falls behind, step *N+1*'s pushes sit in the data queue right behind
  step *N*'s. The drain for step *N* will pop the first *N+1* push, and
  there is no peek, so the layer has to hold that one item for the next
  step rather than discard it. One item of carry-over, and only when
  lagging.
- **Completeness means "everything delivered," not "everything sent."**
  A push that fails or times out on the writer does not stop the step
  from being announced. The step arrives with runs missing. The layer
  should compare the elements received against the selection and flag a
  short step rather than hand back a partly filled array as if it were
  whole.
- **The late-joiner seed.** A reader that joins mid-stream gets a step-ready
  notification for the writer's latest step. That step was never pushed to
  it, so it drains to nothing. The layer must treat the first step after
  joining as possibly empty, not as a step with no matching data.
- **The two queues must be drained in pairs.** The step-ready queue is
  FIFO and unbounded, and the failure mode is worse than unbounded growth
  alone. `vs_push_pending()` doubles `cap_pending` with no ceiling and no
  back-pressure; if the `realloc` fails it takes the
  `n_pending < cap_pending` branch and **silently drops the notification**
  rather than erroring. So a consumer that only calls `get()` and never
  `wait_step_ready()` grows that queue until allocation fails and then
  begins quietly losing steps. The iterator must always pair the two, the
  raw `get()` path must say so in its docstring, and a long-running
  `get()`-only consumer needs a test that the queue does not grow. Owned by
  P4, which is where the raw/iterator split is actually built.

`examples/detector_pipeline`'s `monitor` mode is the C precedent for the
pairing. Until commit `3fae80d` (2026-09-24) it read only one push per step
and got the wrong answer for multi-run steps. It still discards a lookahead
item rather than carrying it. That is fixed alongside this plan, and the
Python layer should copy the fixed loop, not the original.

A per-subscriber end-of-step marker on the wire is still the cleaner
long-term design (it would also make short steps detectable without
knowing the selection). It is a protocol change, so it stays out of scope.

### 3. Environment — resolved

See above. Necessity, not convenience.

## The Phase 3 seam

`sec:pysub-buffer` argues this and it constrains P2's API, so it is
settled now rather than later: the extension owns *allocate a destination,
describe it to the framework, free it when the framework is done*.

`H5Fget_subscribed_data()` hands back a `malloc()`ed buffer the caller
frees. Wrapping it in a NumPy array whose base object frees on dealloc
avoids a copy; `torch.from_numpy()` is then zero-copy too. If the
extension instead returns `bytes` and lets the caller build the array,
there is no seam, and bulk-transfer Phase 3 — which swaps the allocator
and exposes `__cuda_array_interface__` in place of the buffer protocol —
becomes a breaking API change instead of an internal one.

## Milestones

Sizes follow [`dev-plan.md`](dev-plan.md)'s convention.

### P0 — Pin per-step push grouping in C · S

A test, `test/t_step_grouping.c`, that locks in the transport guarantee
from risk 2, for a fragmented (one push per row) selection:

- **In lockstep:** once `H5Fwait_step_ready()` returns *N*, a zero-timeout
  drain returns all of step *N*'s pushes, and nothing else.
- **While lagging:** with several steps already queued, pushes come out
  grouped by step in ascending order and never interleave, and the drain
  for step *N* does run into step *N+1*'s first push. That confirms the
  one-item carry-over P2 needs.
- **Every step is complete:** each step's pushes cover exactly the
  selection, with that step's values.
- **Joining late:** a reader that attaches after the writer has already
  committed steps gets a step-ready for the writer's *current* step, seeded
  by the join itself (`H5Fwait_step_ready()`'s own documentation), and
  that step drains to nothing — it was committed before this reader
  subscribed, so nothing was ever pushed to it. The test asserts the empty
  drain and that the *next* step arrives whole. This pins the empty first
  step as normal behaviour P2 must accept, not a fault it should report.

In C, not a Python spike, deliberately. It settles the P2 design before
P2 starts, it runs in the existing CI, and it stays as a regression test.
A throwaway ctypes probe would answer the question once and then rot.

**What this test cannot cover: short steps.** A short step (risk 2) is
produced by a *writer-side* push that fails or times out, while the step is
still announced. Nothing in a healthy two-process run produces one, so
none of the assertions above can reach the short-step path — the
"every step is complete" assertion would only notice one by accident. It
needs fault injection: a way to make the writer drop or time out a chosen
push. There was none, so the recommended option was built: an
environment-gated, test-only hook in the push path,
`VOL_STREAM_TEST_DROP_PUSH=<k>`, which makes the writer skip its k-th push
while still announcing the step. `python_stream_drop` uses it to check that
the step with the lost push arrives masked on exactly that row.

**Exit gate:** `t_step_grouping` passes in the na+sm CI job, fails if
step-ready is ever sent before that step's pushes are queued, and includes
the late-join case.

### P1 — Extension skeleton and build · S

`VOL_STREAM_BUILD_PYTHON` (matching the existing
`VOL_STREAM_BUILD_TESTS`/`_EXAMPLES` options), the module skeleton,
`open()`/`close()` registering the connector and setting it on the FAPL.

Note this stage needs **no transport**: the connector still captures and
replays without Mercury, so import, open, close and error paths are
testable anywhere — including on a machine with no Mochi stack.

**Exit gate:** `import volstream; volstream.open(path)` works and closes
cleanly, tested without the transport.

### P2 — Subscribe, get, NumPy, reassembly · L

The core, and where the real work is. `subscribe()`, `subscribe_type()`,
`subscribe_predicate()`, `schema()`, `get()`. The NumPy wrapper with
base-object ownership per the Phase 3 seam. Reassembly of several pushes
into one array per step, using P0's pinned semantics: `wait_step_ready()`,
then a zero-timeout drain, with one item of carry-over and a short-step
check (risk 2).

Three rules the reassembly layer follows, each traceable to risk 2:

- **Carry-over:** a drain that pops a push for a later step holds it for
  that step instead of discarding it.
- **Late join:** the first `subscribe()` discards every step notification
  already queued, and any pushes for those steps. That covers more than the
  one seeded step: every step the writer commits between `open()` and
  `subscribe()` is also announced with nothing in it. Later `subscribe()`
  calls discard nothing.
- **Short steps:** each path's array has the selection's shape. If every
  selected element arrived it is a plain `ndarray`; otherwise it is a
  `numpy.ma.MaskedArray` whose mask marks what did not arrive. It does not
  raise, because a dropped push cannot be told apart from a writer that
  wrote only part of the object this step (`b_push_fanout`'s tail-only
  writes), and a predicate subscription is partial by design. The mask is
  the flag. The dropped-push case is tested with P0's fault-injection hook
  (`python_stream_drop`).

**Exit gate:** a Python consumer receives *N* steps of a fragmented
(multi-run) subscription as correctly-shaped NumPy arrays whose values
match what the writer sent — in lockstep, after deliberately falling
several steps behind, and after attaching mid-stream (where the first
yielded step is the first *whole* one, not the empty seed).

### P3 — Lifecycle correctness · M

The three things `sec:pysub` identifies as where this breaks in ways that
surface as someone else's bug:

- **fork:** record the PID at subscribe time, raise on any call from a
  different one. Converts PyTorch's default `DataLoader` fork from a
  silent inert transport into a named exception.
- **GIL and Ctrl-C:** release the GIL around blocking calls, and loop on a
  short (~100 ms) internal timeout calling `PyErr_CheckSignals()` rather
  than passing the caller's full timeout to C — otherwise a consumer
  waiting on a stalled writer ignores SIGINT for the whole interval.
- **Teardown:** explicit `close()`, a context manager, and an `atexit`
  backstop. Never cleanup in a generator's `finally`.

**Exit gate:** the early-`break` case, tested rather than assumed — the
writer proceeds to completion after the consumer breaks out mid-stream.

As built: the PID is recorded at `open()`, and `open()` itself refuses to
run in a child of the process that first registered the connector. The
early-break test measures what an unclean exit actually costs the writer:
until the transport declares the reader dead, each commit pays a 1 s push
timeout. The test requires the writer's commits after the consumer exits
to stay under 500 ms.

Backpressure: a subscriber never sends the step acks M7's queue policy
tracks on its own (a cursor reader acks from its sequential
`H5Fbegin_step()`, which this binding does not use), so a Block policy
could not see a Python consumer. The C API now has `H5Fack_stream_step()`,
and `open()`, `follow()` and `StreamDataset` take `backpressure=True`, which
acks each step `next_step()` returns (and the backlog discarded at
subscribe, so the reader is tracked from then on). It is opt in: under
Block, a consumer that stalls then stalls the writer. Tested both ways by
`python_stream_ack` and `python_stream_noack`.

### P4 — The Pythonic layer · M

Iteration with a defined termination policy (a bare timeout does *not*
raise `StopIteration`; a caller-supplied step count or deadline does —
`sec:pysub-gil` notes this is a choice the C API does not make for us), a
schema-driven convenience path, and a PyTorch `IterableDataset` that
subscribes in `__init__` and only drains in `__iter__`.

This is also where the raw/iterator split gets built, so it owns the
queue-pairing hazard from risk 2. The iterator always pairs
`wait_step_ready()` with the drain, so it cannot leak. The raw `get()`
stays available — some consumers genuinely want one push at a time — but
its docstring states that `get()` alone never empties the step-ready
queue, and that a long-running `get()`-only loop grows it until allocation
fails and then silently loses step notifications. Whether raw `get()`
should also drain the step-ready queue itself, removing the hazard rather
than documenting it, is a decision for this milestone; documenting it is
the floor, not the target.

**Exit gate:** RFC `sec:pysub-gate`'s gate, in full — a consumer written
entirely in Python, no user-written C glue, following a live C writer,
exiting cleanly on both paths — plus a long-running `get()`-only consumer
test showing the step-ready queue stays bounded (or, if the hazard is only
documented, showing that it does grow, so the docstring is a claim with a
test behind it rather than a warning nobody checked).

As built:

- `File.steps(max_steps=, timeout=, idle_timeout=)` and `for step in f:`.
  `idle_timeout` is a bound the caller chooses, so it is consistent with
  "a bare timeout does not end iteration."
- `get()` drains queued step notifications on every call, which removes the
  hazard rather than documenting it. The test checks that none are left
  after a `get()`-only run.
- `volstream.follow(path, selections=None)` opens and subscribes, to every
  integer or float dataset by default.
- `volstream.torch.StreamDataset` refuses to run in a DataLoader worker or
  to be pickled into one, so it is `num_workers=0` only. A stream is one
  ordered source, and each worker would be a separate subscriber receiving
  every step. Its docstring says it applies no backpressure (see P3's open
  item).

No shared filesystem between the two sides: `python_stream_column` and
`python_lifecycle_break` run the writer with `STREAM_WRITER_SUBSCRIBERS=1`,
so it waits in `H5Fwait_subscribers()` rather than for a "ready" file, and
the subscription arriving over the transport is what releases it. The other
modes still use the file. There the writer commits step 0 before it waits,
because a Python `subscribe()` checks its paths against the schema that step
0 publishes. To subscribe before any step exists, as a C subscriber can
(`t_rendezvous_barrier`), pass `subscribe(..., expect={path: (shape, dtype)})`.
It stands in for the schema, and is checked against it when the first step
arrives: a mismatched type, rank or trailing dimension raises rather than
misplacing data. `python_stream_early` runs the writer's barrier before step
0 and requires step 0 itself to be delivered. Because the writer is
released before `subscribe()` returns, the notifications queued before the
subscription are drained before subscribing, not after. Otherwise the
writer's first step could be announced in time to be thrown away with them.

Narrowing fallbacks: each `Push` carries the writer's delivery bits
(`volstream.DELIVERY_*`), and `Step.delivery` maps each path to them, 0 when
every narrowing was applied exactly. The arrays do not depend on them.
Elements outside a selection are dropped either way, and when the writer
could not apply a predicate exactly (unevaluated, or matches coalesced to
their span) the binding applies it and masks what does not match, so a step
looks the same as when the writer filtered it.

End of stream: a subscriber treats the writer leaving the group (closing
the file, or its process dying) as the end of the stream. Steps the writer
announced before leaving are still delivered. After the last one,
`H5Fstep_status()` reports `H5F_STEP_EOS` and the waits return at once, so
`for step in f:` ends by itself. The reader recognises the writer from its
step announcements (which now carry its member id) or from the join seed,
and a parallel writer's stream ends when every rank has left. A writer that
leaves before announcing any step to a reader is not detected. A killed writer is reported once SWIM declares it dead. Pinned in C
by `test/t_eos.c` and in Python by `python_stream_eos`.

### P5 — CI and packaging · M

Add the Python build to the existing na+sm CI job, which already builds
the whole Mochi stack. Package with scikit-build-core so the extension
builds against the same libhdf5 the connector links — the constraint that
made the standalone design necessary in the first place.

**Exit gate:** CI runs the P4 gate on every push.

As built: `pyproject.toml` at the repository root, using scikit-build-core.
`pip install .` configures the whole project with the Python binding on and
tests and examples off, then installs only CMake's `python` component.
`libvol_stream` goes inside the package next to the extension, which finds it
through `$ORIGIN`; the library keeps RPATHs to the HDF5 and Mochi it was
built against. It is a source build by design, since the whole point is to
share the machine's libhdf5, so there is no portable wheel. CI installs it
with pip in the na+sm job and tests the installed copy from outside the
source tree. The import check runs without `LD_LIBRARY_PATH`, so a wrong RPATH
fails there.

## Known gaps

What P0–P5 do not cover, in one place. Each item is either untested, not
supported, or depends on something outside this binding.

### Untested

- **`ofi+tcp` is not gating.** The Python stream and lifecycle tests run in
  CI's `ofi+tcp` pass as well as its `na+sm` one, but that pass is
  non-gating (it tolerates a known libfabric teardown stall; see ci.yml), so
  an `ofi+tcp` failure is reported rather than blocking.
- **`volstream.torch` outside CI.** It is tested only where torch is
  installed (the CI job installs the CPU wheel). Elsewhere the test is skipped.
- **macOS with the transport.** CI is Linux only. On macOS (checked locally
  on 2026-09-24, against HDF5 2.3.0 and MPICH 4.3.2, without Mochi) the
  connector and the binding build, `pip install .` works, and the tests that
  need no transport pass. Nothing with the transport has run on macOS.
  Windows is out of scope: the binding assumes POSIX (`fork()`, `getpid()`,
  `clock_gettime()`).

### Not supported

- **Variable-length sequences, references and bitfields.** `subscribe()`
  raises `NotImplementedError` for them, and for a variable-length string
  inside a compound or array: their pushed bytes are not plain values a
  dtype can describe. A top-level variable-length string is delivered, as an
  object array of `str` (`None` for an unset one): the connector decodes the
  writer's serialized push into `char *` pointers and bytes in one
  allocation, and the extension's `vl_strings()` reads them while it lives.
  It cannot be deflated, converted or filtered by value. Attributes, compound types (as structured
  arrays, with HDF5's member offsets), fixed-length strings, enums (as their
  base integer), array types and opaque types are supported. An attribute
  cannot be delivered deflated.
- **Per-subscriber precision beyond deflate.** `subscribe(deflate=level)`
  exposes the C API's `plists` argument for deflate, the one filter every
  HDF5 build has, with one chunk spanning the selection. Other filters
  (bslz4, zstd, zfp) are not exposed, and neither is a chunk shape (the
  writer honors one as its element count per push).
- **A dataset resized in any dimension but the first.** A whole-dataset
  subscription follows growth along the first dimension: it is made against
  an unbounded first dimension so the writer sends new rows, and the returned
  array grows to hold them (rows not written that step are masked). So
  does a box whose `count[0]` is `None` (every row from `start[0]` on).
  `tail=True` returns only the rows a step sent, so a step costs what
  arrived rather than everything the dataset has grown to. A box with a
  fixed `count[0]` stays fixed. A change to any other dimension would place
  elements wrongly; re-subscribe after one.
- **`num_workers > 0` in a DataLoader.** Refused, by design (see P4).

### Depends on the connector or the Mochi stack

- **Backpressure is opt in** (`backpressure=True`; see P3). Without it a
  writer's queue policy does not see a Python subscriber.
- **An attribute written in a step where its dataset is not** is held on a
  group of the dataset's name in that step's physical layout (user guide
  §2.3). A Python subscriber still receives the attribute, and reads through
  the connector are correct; only a native view of the file sees the group.
- **mochi-flock 0.8.0** crashes when several readers join at once
  (mochi-hpc/mochi-flock#8, fixed on Flock's `main`, not yet released). A
  Python consumer is exposed like any other reader. CI builds Flock `main`.

### Performance

- **One process-wide HDF5 lock.** All Files in a process share it, so a
  blocking wait on one holds up calls on another for up to 100 ms at a time.
- **Reassembly copies.** A step that arrives as one push covering the whole
  selection is returned as a view with no copy. Anything else (several
  pushes, or a strided selection) is copied into a new array.

### Packaging

- **GCC's own RPATH entries on macOS.** Built with Homebrew GCC, the
  installed extension and library list GCC's library directories in their
  RPATH, because the GCC driver adds them to every link. Nothing loads
  through them. pip's temporary build directory used to be listed too: CMake
  could not strip the build RPATH at install, because pip builds under
  `/var`, a symlink to `/private/var`. Both targets are now linked with their
  install RPATH from the start (`BUILD_WITH_INSTALL_RPATH`). On Linux, CI
  prints the installed extension's RPATH and fails if it points into a
  temporary build directory.

## What this does not fix

It does not make vol-stream reachable from an existing h5py-based
application — that would still need the upstream binding
`sec:pysub-not-h5py` rules out depending on. It does not provide a Python
*writer*. And it does not by itself enable an H5Web-style live viewer:
that additionally needs a provider speaking this API and re-serving it,
which is separate work this plan is a prerequisite for rather than a
delivery of.
