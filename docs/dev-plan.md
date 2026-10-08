# vol-stream: development and implementation plan

A typeset version with diagrams is in [`dev-plan.pdf`](dev-plan.pdf); it is a
snapshot from 2026-08-09 and predates everything from M2 on. The architectural
reasoning is in [`design-plan.md`](design-plan.md). For what is implemented now,
[`user-guide.md`](user-guide.md) is current; this plan is the development
record.

Two rules govern this plan. **No changes to HDF5** — everything is reachable from
an out-of-tree connector. And **borrow by default** — we write the step
semantics, the queue policy and the subscription protocol, because those are the
differentiators; everything else already exists in a library that does that one
job better.

## The finding that closed three decisions

HDF5 can serialize its own data model. `H5Tencode` encodes any datatype,
`H5Sencode2` encodes a dataspace **including its selection**, and `H5Pencode2`
encodes a property list — so chunking and the whole filter pipeline travel with
the step.

The step manifest is therefore not a new format. It is a thin envelope around
blobs HDF5 produces and consumes, which makes the wire representation *exactly*
as expressive as the data model — compound types, enums, opaque types, odd byte
orders, arbitrary hyperslabs — with none of it hand-written. A reader
reconstructs types and selections with `H5Tdecode2` and `H5Sdecode` rather than
interpreting our description of them. The `H5Sencode2` selection encoding was
widened to 64-bit in the 1.12 series, so large selections are not a constraint.

## Verified plugin-side surface

Checked against HDF5 `develop`. These are the load-bearing APIs.

| API | Role |
|---|---|
| `H5VLregister_opt_operation` | Connector defines `begin_step`, `end_step`, `step_status`, `subscribe` at init. Public |
| `H5VLfile_optional_op` | How the shipped wrappers invoke them. Public, and `H5VLconnector.h` is installed |
| `H5VLquery_optional` | Applications and tools discover support via the connector's `opt_query` |
| `H5VL_request_class_t` | `wait`/`notify`/`cancel` — the deferred-Put queue and the step barrier |
| dataset read/write callbacks | Already take a `count` array, so `H5Dwrite_multi` maps onto a step batch |
| `H5Sselect_project_intersection` | M×N redistribution. Public, exercised by VDS |
| `H5Sselect_intersect_block` | Chunk-granularity routing |
| `H5VLpassthru.c` | A complete pass-through connector in-tree — the M0 starting point |
| `HDF5_VOL_CONNECTOR` | Points the whole `test/API` suite at the connector with no code changes |

### What the no-library-changes constraint costs

Two things assumed to need core work turn out not to:

- **An `H5ES` barrier for `end_step`.** Not needed. The connector creates its own
  request objects, so it already knows which belong to the open step and can wait
  over its own list.
- **Advertising what the connector cannot do.** `H5VLquery_optional` is public and
  `opt_query` can report per-operation support, covering the programmatic case.

Two are real:

- **Existing tools on a stream.** `h5dump` assumes random access and will fail
  unhelpfully. Without a capability flag there is no clean detection path, so the
  mitigation is documentation, a tested error-message matrix, and shipping
  `h5stream` early. If this becomes the top adoption complaint, that is the
  evidence for proposing the flag upstream — driven by reports, not speculation.
- **Automatic step boundaries.** `H5Pset_append_flush`'s callback is acted on by
  the native library, not an arbitrary connector, so the free auto-`end_step` is
  off the table. Applications call `end_step` explicitly, as they do with ADIOS2.

**Measured limit on the byte-identity promise (2026-08-14).** "Unbracketed
writes remain a pure pass-through, identical to native HDF5" holds only when
no transport is configured. With `VOL_STREAM_NA` set — either `na+sm` or
`ofi+tcp`, so this is not transport-specific — an unbracketed write produces
a file with the *same objects and the same data* but a different internal
allocation: 2995 bytes against native's 2816 in `t_step`'s own reference
case, the difference showing up as the superblock's end-of-file address.
`h5ls`/`h5dump` show no extra objects or attributes, so nothing is being
added to the logical file; the allocator simply takes a different path.

Found by running `t_step` under `VOL_STREAM_NA`, which nothing had done
before — every test that sets it is a transport test, and every test that
checks byte identity ran without one, so the two had never met. Worth stating
because the promise as written carries no qualification, and a reader would
reasonably expect it to hold for a transport-enabled writer too. In practice
a streaming writer is not the configuration anyone byte-compares, so this is
documented rather than fixed.

## The five decisions

### 1. Is a step file-wide, or does each group get its own cadence?

**Evidence.** openPMD hit this in production. Their ADIOS2 backend assumed one
step equals one logical iteration ascending, and it *broke on restart from
checkpoint*: resuming at iteration 500 after writing through 750 produces
`0, 50 … 750, 500, 550` — duplicate, non-monotone logical time on a monotone
physical counter. Their fix was not a second counter; it was an explicit
`snapshot` attribute annotating which logical iteration(s) a step carries.

**Decision.** A step is **file-scoped and physically monotone** — matching ADIOS2,
which keeps atomicity and the queue simple — but the manifest carries an
explicit, application-settable **set of logical step ids**, distinct from the
physical counter.

This resolves per-group cadence without a second mechanism: a group absent from
step *N* retains its value from its last appearance, and readers ask for "the
state of group G as of step N." A mesh written once per hundred field steps costs
one manifest entry, not a re-send. It also fixes the restart overlap ADIOS2 still
has.

### 2. How does the step index appear in the dataspace?

**Evidence.** openPMD standardized three encodings and ships all of them:
*groupBased*, one group per iteration at `/data/<n>/`, which their HDF5 backend
uses; *variableBased*, where a variable holds successive versions and no
per-iteration group exists, requiring ADIOS2; and *fileBased*, one file per
iteration. Nothing in the ecosystem models the step as an extra array dimension.

**This overturns an earlier recommendation.** Making the step an implicit
slowest-varying dimension conflates a transaction axis with a data axis, breaks
the moment a variable's per-step shape changes, and has no precedent anyone reads.

**Decision.** Two encodings, matching what exists. On the wire, steps are
successive versions of the same named object — openPMD's variable-based form,
which is simply what a stream is. On replay and in the file engine, steps land
group-based at `/step/<n>/`, the portable HDF5 encoding every existing tool can
read. The `[nsteps][…]` aggregate view remains available as an *optional VDS
overlay generated at replay*, never the primary model.

### 3. What is the subscription vocabulary?

**Evidence.** `H5Sencode2` encodes a dataspace with its selection, so a hyperslab
— including strided and irregular selections — is already a portable blob.
`H5Pencode2` encodes a filter pipeline with its parameters.

**Decision.** A subscription is a list of *(object path, `H5Sencode2` blob,
optional `H5Pencode2` blob)*. Variables, subvolumes and stride come from the
dataspace encoding; precision comes from the property-list encoding, so
per-subscriber precision reuses the existing filter pipeline instead of new
compression code.

Only the predicate is genuinely new, and it does not ship until M8. No reserved
slot is needed: because the records are **FlatBuffers tables**, adding a field
later is a compatible change old readers ignore.

### 4. Which parts of the data model are legal in v1?

**Evidence.** `H5Dvlen_get_buf_size` and `H5Treclaim` let a connector size and
release variable-length buffers, so it can deep-serialize an `hvl_t` itself. And
references are translatable: `H5Rget_obj_name`, `H5Rget_file_name` and
`H5Rget_attr_name` convert an opaque `H5R_ref_t` into names, which
`H5Rcreate_object` and `H5Rcreate_region` reconstruct on the far side — with the
region's selection carried by `H5Sencode2`.

**This overturns an earlier recommendation.** Excluding VL data cited VFD SWMR
hitting the same wall because HDF5 stores VL as metadata. That reasoning does not
transfer: VFD SWMR's problem is *concurrent metadata in a shared file*; a
connector serializing its own payload never touches the global heap.

**Decision.** v1 covers datasets, attributes, groups, committed datatypes, and
**variable-length data** via deep serialization. References are supported **by
name translation** when the target is inside the stream. The exclusion list
shrinks to references pointing outside the stream, which must be rejected at
`end_step` with a clear error rather than silently dangling.

### 5. What are the public calls named?

**Evidence.** HDF5's house style, visible in `test/API/H5_api_async_test.c`, is
to extend within the owning class namespace (`H5Dwrite_async`,
`H5Fcreate_async`) or add a new two-letter subsystem like `H5ES`. The Async VOL
sets the out-of-tree precedent by shipping `H5F`-namespaced calls.

**This fixes a collision.** An earlier draft used `H5SBegin_step`. `H5S` is the
dataspace namespace — actively misleading, and it would have shipped in a public
header.

**Decision.** `H5Fbegin_step()`, `H5Fend_step()`, `H5Fstep_status()`,
`H5Fsubscribe()`. Registered operation strings are namespaced independently as
`"vol-stream:begin_step"` so two connectors cannot collide in the registry.

## Implementation design

### Step manifest

```
// vol_stream.fbs -- compiled by flatcc. Adding a field is a compatible
// change, so there is no reserved slot to guess the size of.
namespace vs;

enum Kind    : ubyte { DsetCreate, DsetWrite, Attr, Group, Link }
enum Payload : ubyte { Raw, FilteredChunks }

table Entry {
  kind:Kind;
  path:string;
  type_enc:[ubyte];        // H5Tencode
  space_enc:[ubyte];       // H5Sencode2 -- carries the selection
  dcpl_enc:[ubyte];        // H5Pencode2 -- chunking + filter pipeline
  form:Payload;
  payload_off:ulong;
  payload_len:ulong;
  predicate:[ubyte];       // added in M8; absent in earlier writers
}

table Step {
  physical_step:ulong;     // monotone, connector-assigned
  logical_ids:[ulong];     // decision 1 -- restart-safe
  wall_time_ns:ulong;      // supplied by caller, never generated
  hdf5_version:uint;       // which library produced the blobs above
  entries:[Entry];
  payload_bytes:ulong;
}
root_type Step;
```

The `Step` table is serialized after all entries are known, so a step remains a
single forward-only append with no back-patching — the property that made the VOL
the right layer.

### Filtered-chunk passthrough

> **Status: not being built.** This section is retained as design rationale —
> the conflict rule below is live code — but `FilteredChunks` as a manifest
> payload form was decided against in M8.5.1; see that section for the
> evidence.

`H5Dread_chunk2`, `H5Dwrite_chunk`, `H5Dchunk_iter` and `H5Dget_chunk_info` are
public, so when a dataset is chunked and filtered and a write covers whole
chunks, the connector can move **already-compressed chunks** without
decompressing. That is the `FilteredChunks` payload form.

It cuts CPU on both ends, shrinks the wire payload for free, and a chunk is the
natural routing unit for subscription — paired with `H5Sselect_intersect_block`
the writer decides which chunks a subscriber needs without touching the data.
Partial-chunk writes degrade to `Raw`.

**This conflicts with per-subscriber precision (decision 3, M8.5) unless
resolved explicitly.** `FilteredChunks` bytes are already compressed under the
dataset's own native filter pipeline; a subscriber whose requested
`H5Pencode2` pipeline differs from that native one cannot be served those
bytes as-is. Rule: if any current subscriber's pipeline differs from the
dataset's native DCPL, the writer falls back from `FilteredChunks` to `Raw`
for that push — decompress, apply the subscriber's own pipeline, re-compress
— rather than sending mismatched or unfiltered bytes. `FilteredChunks` stays
zero-copy only for subscribers whose pipeline matches the write-time DCPL
exactly.

Per-subscriber precision shipped in M8.5, and the conflict rule above is now
**live code rather than a trivially-satisfied one** — though reached from the
opposite direction than this section assumed.

**M8.5.1 — the zero-copy fast path.** No new capture machinery or payload
form was needed to get the first real benefit. By the time a subscriber is
served, replay has *already* written a real, chunked, filtered dataset into
the underlying file — so when a subscriber's requested pipeline is the one
the data was written under, the compressed bytes it wants exist verbatim,
and `H5VL__stream_refilter_zero_copy()` reads them straight out
(`H5VL_NATIVE_DATASET_GET_CHUNK_INFO_BY_COORD` /
`H5VL_NATIVE_DATASET_CHUNK_READ`) instead of rebuilding an identical
throwaway `H5FD_CORE` dataset and re-running the same filters over the same
input to recompute the same answer.

That is exactly this section's rule, enforced by construction: matching
pipeline → serve stored filtered bytes; differing pipeline → fall back to
re-filtering from `Raw`. The equivalence test is `H5Pequal()` on the decoded
subscriber and write-time DCPLs, which compares chunking as well as filters,
so a subscriber asking for a different chunk shape correctly declines rather
than being silently served the dataset's own shape. Declining is always safe
— the slow path still produces a correct result.

Two constraints are deliberate, both matching this section's own "partial
writes degrade to `Raw`" rule: the push must cover the whole dataset from
element 0 (a partial overlap would need the chunk grid walked and per-chunk
intersection computed), and the extent must be exactly one chunk (the
receiving side reconstructs a single whole-extent chunk, so serving several
would need a wire-format change). Anything else declines to the slow path.

Both branches are covered by tests that distinguish *which* ran, not merely
that the data came out right: `VOL_STREAM_DEBUG_REFILTER` tags the fast
path's log line `(zero-copy)`, so `test/t_chunk_zerocopy.c` (matching
pipeline) fails if it silently falls through, and `test/t_precision.c`
(deliberately mismatched pipeline) continues to exercise the temporary-
dataset route.

**Step payload staging is compressed (2026-08-14).** Measuring before
building `FilteredChunks` turned up a bigger and much cheaper win. Every step
writes its raw bytes to `/step/<n>/.payload` and then replays the real
objects beside it, so the same data is stored twice — and for a filtered
dataset the two costs are nothing alike. On a 2000-int GZIP dataset the real
object allocated **48** bytes while `.payload` allocated the full **8000**:
the staging copy was 167x the size of the data it staged, and dominated the
file.

`.payload` is an opaque byte array this connector both writes and reads, so
deflating it needs no format change and no reader change — HDF5 inflates it
transparently. It now allocates **83 bytes instead of 8000** for that same
step, and the win is generic (every step, not only filtered datasets) rather
than confined to the case `FilteredChunks` would have addressed. Best-effort
throughout: no deflate available, or any property-list call failing, falls
back to the previous uncompressed layout rather than failing the commit.
`test/t_payload_compress.c` asserts it, because no behavioral test can —
compression is invisible to correctness checks, so a regression that dropped
it would leave the whole suite green while the file quietly grew back.

That measurement also changes the case for `FilteredChunks`: the duplication
it was meant to attack is now largely gone, and what remains of its value is
the CPU saved by not re-filtering at replay — which the M8.5.1 zero-copy path
already captures for the transport side.

**`FilteredChunks` as a manifest payload form: decided against.** Not
deferred — dropped, on evidence. It was motivated by two costs, and both
have since been addressed more cheaply:

- *Payload duplication.* The staging copy really was the dominant cost
  (167x the real data), but compressing `.payload` fixed that generically,
  for every step rather than only filtered datasets, in a few lines and with
  no format change.
- *Re-filtering CPU at replay.* M8.5.1's zero-copy fast path already avoids
  it wherever a subscriber's pipeline matches the dataset's, which is the
  case `FilteredChunks` was aimed at.

What remains of its value is a narrow slice — avoiding a re-filter for a
subscriber whose pipeline matches but whose write did not cover a whole
single chunk — bought at the price of a new payload form, a `filter_mask`
schema field, capture-side filtering, and a replay path that injects chunks
via `H5Dwrite_chunk()`. That is an M2-scale change for a case neither
measured nor reported. Revisit only if profiling shows re-filtering as a
real cost; the design above stands ready if so.

**Closed 2026-08-15, and without the chunk grid.** This was the last routing
gap: subscription routing linearized the selection's *bounding box*, so a
non-contiguous subscription received a superset — correct, but coarser than
it could be. It was framed as needing chunk-granularity routing
(`H5Sselect_intersect_block` against `FilteredChunks`), which is why it
outlived the payload form it depended on.

It needed neither. The subscription's `H5Sencode2` blob — decision #3's, and
until now reduced to its bounds before it ever left the reader — now travels
to the writer, which intersects the selection with each write and pushes one
RPC per contiguous run of the real intersection. That is
`H5VL__stream_space_flat_runs()`, the same decomposition the strided-write
fix already used on the other side, applied to the subscriber's selection
instead of the writer's. No chunk grid, no new payload form, no wire change.

Measured in `test/t_subvolume_strided.c`, subscribing to column 3 of a 6×8
dataset — 6 elements whose bounding box spans 41 of the 48:
**41 elements (164 bytes) before, 6 (24 bytes) after.** The extents of the
two dataspaces are compared before any flat index from one is trusted in the
other, and a mismatch declines to the old behavior; getting that wrong is
exactly what caused silent data loss twice here already.

### Connector state machine

| State | Legal operations | Notes |
|---|---|---|
| `OPEN` | create/open objects; `begin_step`; `subscribe` | Writes outside a step are an error, not an implicit step |
| `IN_STEP` | dataset and attribute writes; further creates; `end_step` | Entries accumulate; deferred writes queue on the connector's request list |
| `COMMITTING` | none from the application | Barrier over this step's requests, reference validation, manifest emit. Collective in parallel |
| `READING` | `begin_step` to advance; reads within the current step | Advancing past an unconsumed step is governed by queue policy |

### Registration and introspection

```c
herr_t vs_init(hid_t vipl_id) {
    H5VLregister_opt_operation(H5VL_SUBCLS_FILE, "vol-stream:begin_step",  &vs_op_begin);
    H5VLregister_opt_operation(H5VL_SUBCLS_FILE, "vol-stream:end_step",    &vs_op_end);
    H5VLregister_opt_operation(H5VL_SUBCLS_FILE, "vol-stream:step_status", &vs_op_stat);
    H5VLregister_opt_operation(H5VL_SUBCLS_FILE, "vol-stream:subscribe",   &vs_op_sub);
    return 0;
}

/* opt_query lets applications and tools discover this without a capability flag. */
herr_t vs_opt_query(void *obj, H5VL_subclass_t cls, int op, uint64_t *flags) {
    if (op == vs_op_begin || op == vs_op_end)
        *flags = H5VL_OPT_QUERY_SUPPORTED | H5VL_OPT_QUERY_COLLECTIVE
               | H5VL_OPT_QUERY_MODIFY_METADATA;
    ...
}
```

`H5VL_OPT_QUERY_COLLECTIVE` states that step boundaries must be called by every
rank, so the parallel contract is discoverable rather than only documented.

### Threading

**Argobots and Margo own the progress engine from M4**, not from the end. Margo
exists precisely to drive Mercury from user-level threads; the alternative was
writing a progress thread by hand for three milestones and then deleting it. The
connector keeps one lock over its own step and queue state.

v1 does not advertise `H5VL_CAP_FLAG_THREADSAFE`. Borrowing a good threading
library is not the same as promising the connector is safe under concurrent
application calls.

## Milestones

Sizes are relative — S is days, M is weeks, L is a quarter-ish for one developer.
Every gate is runnable.

### M0 — Skeleton, CI, and the regression net · S

Start from `H5VLpassthru.c`. Register as `vol-stream`, build as a shared plugin,
wire CI across every HDF5 version to support. No streaming logic — this exists to
make later milestones measurable.

**Exit gate.** `test/API` driven via `HDF5_VOL_CONNECTOR=vol-stream` behaves
identically to native. Not merely "passes": a pass quietly becoming a skip is the
regression this catches.

### M1 — Step API surface, as no-ops · S

Register the four namespaced operations, ship `H5VLstream.h` with the wrappers
over `H5VLfile_optional_op`, implement `opt_query` including the collective flag,
pin the `H5VL_VERSION` check.

**Exit gate.** An application brackets writes in `H5Fbegin_step`/`H5Fend_step`
and produces a byte-identical file to one written without them.
`H5VLquery_optional` reports each op correctly, including collectivity.

### M2 — Manifest, capture, and the replay invariant · M

The milestone carrying the weight an RFC would have. Define the flatcc schema and
fill it from `H5Tencode`/`H5Sencode2`/`H5Pencode2`; implement the logical-id set;
land steps group-based; implement VL deep-serialization and reference-by-name.

Then stand up the replay invariant: the same unmodified writer run through the
connector and through native HDF5 must produce indistinguishable files. Because
it compares against the library rather than a hand-written expectation, it catches
type-conversion, dataspace and ordering bugs nobody predicted.

**Exit gate.** `h5diff` clean across compound and enum types, byte-order
mismatch, committed datatypes, VL data, in-stream references, attributes, nested
groups, chunked and contiguous layouts, and partial hyperslab writes.
Out-of-stream references fail at `end_step` with a specific error. Filtered-chunk
passthrough round-trips, and partial-chunk writes demonstrably fall back to `Raw`.

### M3 — Decoupled reader · M

A separate reader process opens the stream, iterates steps, and reads with
hyperslabs unrelated to how the writer decomposed its writes. Single-process both
sides. Implement "state of group G as of step N" and the optional VDS overlay.

**Exit gate.** Selections straddling multiple writer calls return correct data. A
group written every hundredth step resolves correctly at every intermediate step.
A restart-overlap sequence is read back in correct logical order.

### M4 — Mercury transport, Margo progress, deferred I/O · M

Real transport arrives here rather than at the end. `tr_mercury` with Margo
driving progress on Argobots threads, and `H5VL_request_class_t` implemented so
writes queue and resolve at `end_step`.

**Exit gate.** `H5_api_async_test.c` passes. Writer and reader in separate
processes over Mercury on both `na+sm` and `ofi+tcp`, replay invariant intact
with deferral on.

### M5 — Rendezvous and late joiners · S

SSG for group membership — what SST's `RegistrationMethod`,
`RendezvousReaderCount` and late-joiner discovery amount to. *(Migrated to
mochi-flock after M10, when SSG was deprecated by its own maintainers; see
[After M10](#after-m10).)*

**Exit gate.** A writer starts with no readers and proceeds; a reader attaches at
step 500 and gets a coherent view; a reader leaving mid-stream does not stall the
writer. All without a shared filesystem.

### M6 — Parallel writer and real M×N · L

Collective `begin_step`/`end_step` over the writer communicator, per-rank
selections aggregated into the manifest, reader requests resolved with
`H5Sselect_project_intersection`. Aggregation topology borrowed from the
Subfiling VFD's I/O concentrators.

**Exit gate.** Byte-exact data with coprime rank counts — 7 writers to 3 readers,
64 to 5. Replay invariant holds in parallel.

M6's first increment meets this gate on the uniform-topology case only — every
rank creates the same objects, varying just which hyperslab each rank covers,
which is already legal under parallel HDF5's own collective-create rule and
needs no manifest aggregation across ranks. [M6.5](#m65--heterogeneous-mn-and-concentrator-topology--m)
carries the general case forward.

### M6.5 — Heterogeneous M×N and concentrator topology · M

The two things M6 named but didn't need for its first increment: ranks that
create *different* objects (rank 0 writes a dataset rank 1 never touches) and
the Subfiling-style I/O-concentrator aggregation topology. Heterogeneous
object sets break the "every rank calls the same creates" shortcut, so pending
entries must be exchanged across ranks — `MPI_Allgatherv` of a per-rank flatcc
mini-manifest (metadata only; payload bytes stay local), merged deterministically
(create-kind entries deduped by `(kind, path)`; `DsetWrite` never deduped;
`Attr` first-seen-wins). Reader-side requests spanning a concentrator's
aggregated region are resolved with `H5Sselect_project_intersection`, the
public API M6 traced but left unused.

**Exit gate.** Coprime rank counts (7→3, 64→5) with a heterogeneous per-rank
object set — not every rank creating the same objects — still byte-exact and
replay-invariant, routed through at least one concentrator that aggregates
more than one writer rank.

Cross-rank manifest aggregation and heterogeneous per-rank object sets shipped
and are verified byte-exact at this gate's own 7→3 and 64→5 rank counts, real
per-rank-private datasets included — see `H5VL__stream_replay_step_parallel()`
in `src/H5VLstream.c`.

**Update:** the Subfiling-style I/O-concentrator topology has also landed,
opt-in via `VOL_STREAM_CONCENTRATION` (unset/1 = every rank still does its own
raw-data I/O directly, unchanged from the paragraph above). Writer ranks are
partitioned into contiguous groups of that size; each group's first rank is
its concentrator, and every other member ships its `DsetWrite` entries to it
over MPI point-to-point instead of writing directly (type/dataspace carried
via `H5Tencode`/`H5Sencode2`, the same idiom the manifest replay already
uses) — see `H5VL__stream_replay_concentrated_writes()`. Verified byte-exact
with real aggregation at 3→2 (one 3-rank concentrator group) and 7→3 (two
concentrator groups of 3, one singleton), via
`run_parallel_test.sh --concentration N`, which also asserts the concentrator
actually logged writing on another rank's behalf rather than silently
no-op'ing. Still open: `H5Sselect_project_intersection`-based reader
resolution — this increment only changes *which rank* issues each
`H5VLdataset_write()`, not what a reader does, so a concentrator's aggregated
region is not yet combined into fewer/larger writes or specially resolved on
read.

**Update 2026-08-16: the "still open" wording above overstated what was
actually missing.** Reading the code directly (not just this paragraph)
before touching anything: a concentrated write already lands as an ordinary
`H5VLdataset_write()` against the real underlying file, with the originating
rank's own hyperslab selection intact — a reader was never broken or
suboptimal, since plain HDF5 hyperslab I/O already resolves a decomposition
mismatch between writer and reader for free once bytes are real file bytes
(`test/t_parallel.c`'s reader phase already uses an independently-computed
decomposition that need not match the writer's, and always did). There was
never a read-side gap to close, and `H5Sselect_project_intersection` was
never actually needed for correctness here.

What *was* real: `H5VL__stream_replay_concentrated_writes()` opened and
closed the target dataset once per contributing entry — once per rank's own
write, whether local or received — even when several entries in one group
targeted the same shared path. Closed by gathering every entry (this rank's
own plus every group member's, decode-only via a new `H5VL__stream_recv_
entry()`) into one job list *before* issuing any I/O, then grouping by path:
entries sharing a path — regardless of which member they came from — now
share one `H5VLdataset_open()`/`H5VLdataset_close()` pair, each entry still
getting its own `H5VLdataset_write()` call in exactly the original relative
order. Deliberately *not* combined into one `H5VLdataset_write()` call via
its `count`-array form across contributors sharing a path: that would need
building one unioned destination selection (`H5Sselect_hyperslab(...,
H5S_SELECT_OR, ...)`) and a matching flattened memory-side gather buffer in
the file selection's own iteration order — exactly the selection-algebra
shape `H5Sselect_project_intersection` would have been for, and HDF5 does
not document a same-dataset-repeated-in-one-multi-write ordering guarantee
strong enough to trust for overlapping regions without far deeper
verification than this pass could give it. Reopen elimination is the safe,
verifiable slice of "combine into fewer/larger writes" and is what actually
matched "the current cost" this section originally meant (repeated
open/close, not repeated write, per entry).

Verified against the exact same two aggregation shapes this section already
claimed (3→2, 7→3 two-groups-plus-singleton), byte-exact, via
`run_parallel_test.sh --concentration N` run directly — and, since that
script had never actually been CI-wired despite the claim above, two new
`ctest` entries now cover it for real: `parallel_concentration` (3→2) and
`parallel_concentration_7x3`.

### M7 — Queue policy and BAKE spill · M

`Block`, `Discard` and `Spill`, plus reserve slots and latest-step-only for
monitoring readers. BAKE moves bytes to node-local storage; the policy deciding
*when* is ours.

**Exit gate.** Under a deliberately slow reader: `Block` stalls the writer and
loses nothing, `Discard` drops only whole steps, `Spill` does neither and the
lagging reader catches up from local storage.

### M8 — Subscription protocol · L

The differentiator. Readers declare interest as *(path, `H5Sencode2`,
`H5Pencode2`)* triples; the writer marshals only what is subscribed, routing at
chunk granularity. Mercury's RPC direction, in place since M4, is the
back-channel.

**Exit gate.** Two subscribers on one stream at different precisions from a
single `end_step`. Measured wire bytes scale with subscribed volume, not total
step volume — the number that proves the thesis.

M8's first increment proves the core mechanism — a subscribe RPC and a real
data-push RPC (Mercury's first carrying payload bytes, not just control-plane
scalars) — at whole-object granularity: an unsubscribed sibling object in the
same step is never sent. [M8.5](#m85--chunk-level-subscription-routing--m)
carries the exit gate's literal chunk-level/multi-precision numbers forward.

### M8.5 — Chunk-level subscription routing · M

The two things M8 named but its first increment didn't need: routing a
subscription's *(path, `H5Sencode2` selection)* at chunk granularity via
`H5Sselect_intersect_block` against the `FilteredChunks` payload form (M2), and
per-subscriber precision — reusing a subscription's `H5Pencode2` filter
pipeline to re-filter one dataset's bytes differently per subscriber, per
dev-plan.md decision #3. Both need a subscription to carry (and the writer to
actually consult) the dataspace selection and property list M8's first
increment already accepts and validates but never acts on.

**Exit gate.** Two subscribers on one stream at different precisions from a
single `end_step`, wire bytes measured (not just asserted less-than) to scale
with subscribed volume — dev-plan.md's M8 exit gate, in full.

**Correctness fix 2026-08-14 — N-D selections were silently truncated.**
Routing is expressed in flat element indices, and the bounds helper used
`H5Sget_select_bounds()`'s *dimension-0* low/high directly as element
indices. For a 1-D dataspace that is the same thing, and every subscription
test in the suite was 1-D, so it went unnoticed. For rank >= 2 dimension 0
indexes rows, not elements: subscribing to row 0 of a ROWS x COLS dataset
produced the range `[0, 1)` — one element instead of the COLS-wide row
asked for — and the subscriber was served a fraction of its subscription
with no error. That is data loss, not coarse routing. The bounds are now
linearized against the dataspace's own extent (row-major, matching HDF5's
element ordering); `test/t_subvolume_nd.c` pins it by checking the *count*
of elements received, since the values that did arrive were correct and a
value-only check passes while most of the data goes missing.

The linearized range is the smallest flat span *containing* the selection's
bounding box, so a non-contiguous **subscription** yielded a superset of what
was asked for. Deliberate at the time: over-sending is inefficiency,
under-sending is data loss. **Since closed** — the span is now only the
coarse filter and the fallback, with the subscription's own encoded
selection carried through and intersected exactly; see the filtered-chunk
section above for the measurement.

**A second, worse instance of the same assumption, on the write side.** A
push describes its payload as one flat range with the bytes contiguous from
`elem_start` — truthful only when the *write's* own selection is a single
contiguous flat run. It is for a whole-dataset write or a slab of leading
dimensions, but not for a strided one: a write of column 0 of a 4x8 dataset
was pushed as `elem_start=0 elem_count=4`, claiming flat elements 0..3 when
three of those were never written and the column's real values belong at
flat 0, 8, 16, 24. Unlike the subscription case that was *wrong values*, not
missing ones. Fixed by `H5VL__stream_space_flat_runs()`, which decomposes a
selection into maximal contiguous flat runs and pushes each separately —
no wire change, since the receiving queue already delivers one push at a
time with its own range. A whole-dataset or slab write stays a single push,
so the common path is unchanged. Point/irregular selections, or selections
exceeding the run cap, push nothing rather than something mislabelled: the
data is durably in the file regardless, and a missing push is recoverable
where silently wrong values are not. `test/t_strided_write.c` pins it by
checking every pushed element against whether the writer actually wrote that
flat index.

1-D element-range intersection shipped: a subscription's `H5Sencode2` bounds
(via `H5Sget_select_bounds()`, dimension 0) now thread through to the writer,
which intersects each subscriber's requested range against what it actually
wrote and pushes only the overlap — verified with a bounded subrange
subscription receiving exactly its requested elements, not the whole object.
See `vs_tr_writer_push_data()` in `src/tr_mercury.c`.

**Update: per-subscriber precision shipped too.** A subscription's
`H5Pencode2` DCPL (accepted and ignored since M8) now travels with the
subscribe RPC, and the writer re-filters that subscriber's own overlap slice
through the requested pipeline before sending it. HDF5 exposes no public
"apply this pipeline to a buffer" call outside ordinary dataset I/O, so the
mechanism is a throwaway `H5FD_CORE` (in-memory, no backing store) dataset
built with the subscriber's decoded DCPL: `H5Dwrite()` applies the filters,
`H5Dread_chunk2()` pulls the filtered bytes straight back out, and the
receiving side mirrors it exactly (`H5Dwrite_chunk()` to inject the already-
filtered bytes, ordinary `H5Dread()` to decode). Decision #3's "reuse the
existing filter pipeline instead of new compression code" holds literally —
there is no codec here, only HDF5's own. `src/tr_mercury.c` stays HDF5-free
via a registered `vs_tr_refilter_fn` callback rather than decoding property
lists itself.

Reversal is transparent: `H5Fget_subscribed_data()` always returns decoded
values, so a caller never sees filtered bytes and existing subscribers are
byte-for-byte unaffected. Verified end-to-end in `test/t_precision.c`: a
writer stores `/precise` **uncompressed** (`H5P_DEFAULT`), a subscriber
requests GZIP, and the push measurably shrinks **8000 → 48 bytes on the wire**
while decoding back to the exact original 2000 values — real measured wire
bytes, this section's exit gate asks for, from a pipeline the dataset itself
never had.

**Update: the exit gate's literal wording, demonstrated.** `test/t_precision_
dual.c` — three real OS processes, two simultaneous subscribers on `/precise`
(reader A: GZIP-9, reader B: none) served from one `end_step` — measures two
pushes sharing the same raw byte count but differing filtered sizes, proving
the "wire bytes scale with subscribed volume" claim with actual numbers, not
just per-subscriber-by-construction reasoning. Building it surfaced two real
bugs, both fixed at the source, not test artifacts:

1. `vs_subscribe_ult()`/`vs_reader_ack_ult()` answered "success" from *any*
   group member, not just the writer — invisible with one subscriber, but
   with two, reader B could file its subscription with reader A instead of
   the writer. Fixed with an `is_writer` guard in `src/tr_mercury.c`.
2. `vs_tr_stop()` called `ssg_finalize()` immediately after
   `ssg_group_leave()`/`ssg_group_destroy()`, with no settle window. A peer's
   SWIM ping already in flight to a just-departed member could still be
   dispatched to mochi-ssg's own `swim_dping_req_recv_ult()` after
   `ssg_finalize()` had nulled its runtime pointer, crashing the process
   (`Assertion 'ssg_rt' failed`). This was a real usage gap, not purely a
   third-party bug: mochi-ssg's own `tests/ssg-join-leave-group.c` sleeps
   between leave/destroy and finalize; `vs_tr_stop()` did not. Fixed by
   adding that same settle window — see its comment in `src/tr_mercury.c`.
   Stress-tested 20/20 crash-free afterward (reliably crashing before).

Still open, and why this test is real, built, and passing but *not* wired
into the default `ctest` run (`DISABLED TRUE` in `test/CMakeLists.txt`): a
separate, deeper residual where Mercury's single progress ULT can still block
inside an unbounded `connect()` in libfabric's TCP provider, reaching for an
already-departed peer during teardown. This never produces a wrong answer —
every run's actual exit-gate assertions pass first — and never crashes after
the fix above, only occasionally stalls process teardown past a normal test
timeout. The real fix belongs in libfabric/Mercury's own connection
handling, well outside this project.

**Update: selection-exact routing shipped 2026-08-15**, closing what this
paragraph called chunk-storage-granularity routing — and closing it without
chunks. A subscription's `H5Sencode2` selection now reaches the writer and is
intersected with each write directly, so a non-contiguous subscription gets
exactly its own elements rather than its bounding span (41 elements → 6 in
`test/t_subvolume_strided.c`). `H5Sselect_intersect_block` against
`FilteredChunks` was never the only way to get there; it was the way that
depended on a payload form M8.5.1 then dropped.

What remains genuinely tied to chunks is narrower than this paragraph
implied: re-filtering still builds a single chunk spanning the pushed run
rather than honoring a chunk *shape* a subscriber's DCPL asks for. That
affects how a re-filtered push is stored on the way across, not which
elements are chosen.

**Update 2026-08-16: closed, and without a wire-format change.** The
obvious-looking fix — teach `H5VL__stream_refilter_for_subscriber()` to
write several real chunks and send them as one bigger RPC — would have
meant extending `vs_data_push_in_t` itself (today: one `payload` blob, one
`filter_mask`) to carry an array of chunks, a change to the Mercury proc
struct every writer and reader in a running job shares.

Not needed. `H5Fget_subscribed_data()` already accepts several independent
pushes for one logical write — that is exactly what selection-run splitting
(the paragraph above) and M9's predicate-run splitting already do, each run
becoming its own `vs_data_push_in_t` RPC with its own `elem_start`/
`elem_count`. Chunk-shape honoring reuses that same mechanism instead of
inventing a second one: a new query-only callback,
`vs_tr_refilter_shape_fn` (`H5VL__stream_refilter_shape_for_subscriber()`
in `src/H5VLstream.c`), decodes a subscriber's `dcpl_enc` just far enough to
answer "how many elements per chunk did this DCPL actually ask for via
`H5Pget_chunk()`" — 0 when there is no real preference (unchunked, or a
requested shape ≥ the run), which is every existing test
(`test/t_precision.c`, `test/t_chunk_zerocopy.c` both request a chunk shape
equal to the whole extent) and therefore a provable no-op against every
prior caller. A non-zero answer smaller than the run makes `vs_tr_writer_
push_data()`'s per-run loop in `src/tr_mercury.c` split that run into
that-sized slices and call the *existing*, unmodified `vs_tr_refilter_fn`
once per slice — `H5VL__stream_refilter_for_subscriber()` needed no
internal change at all, since handing it a smaller `count` already produces
a correctly-sized single chunk of exactly that count, the same "spans the
run" behavior it always had, just applied to a smaller run. The push-loop's
repeated RPC-forward logic was factored into `vs_tr_push_one_item()` so the
whole-run path (unchanged) and the new per-slice path share it rather than
duplicating the margo forward/timeout/cleanup sequence.

New test, since none of the existing precision/chunk tests could have
caught a regression here (all of them request a chunk shape equal to the
whole run, exactly the case that must *not* split): `test/t_chunk_shape_
split.c`, 64 ints, subscriber requests a chunk shape of 16 — asserts
exactly 4 pushes of 16 elements each are received (not 1 of 64), each
independently GZIP-filtered and correctly decoded across chunk boundaries.
Verified both ways: passes with the new callback registered, and — reverted
(`if (0) vs_tr_set_refilter_shape_cb(...)`) — fails with exactly the old
symptom (1 push of 64), confirming the test actually exercises the change
rather than passing by construction.

### M9 — Tools and the long tail · M

**Scope narrowed 2026-08-15.** An ADIOS2 interop bridge is **dropped** — not
deferred. h5py bindings are **postponed**, not abandoned. *(Superseded
2026-09-24: Python reaches the stream through a separate subscriber package,
not h5py; see [`python-plan.md`](python-plan.md).)*

In scope:

- `h5stream`, a real tool: list steps, tail a live stream, reorganize one
  into a static file. Note it does not exist yet in any form — the original
  wording ("promoted from test scaffolding") overstated the starting point,
  as there is no such scaffolding.
- The tool-compatibility matrix, documented honestly, `h5dump` included.
- Then predicate pushdown and onion-backed addressable step history. Both
  shipped; see the two sections below.

**Exit gate.** `h5stream` follows a live writer end to end: lists steps as
they commit, tails new ones as they arrive, and reorganizes a running stream
into a static file that `h5dump` reads without complaint.

*Progress: `list`, `export`, `tail`, predicate pushdown and onion-backed
addressable step history have all landed and are tested. The exit gate above
is met and M9's scope is complete.*

#### Predicate pushdown — shipped 2026-08-15, and cheaper than designed

`H5Fsubscribe_predicate(file_id, path, op, type_id, value)` attaches a value
test to an existing subscription; the writer evaluates it against the bytes
it is about to marshal and sends only the elements that satisfy it. Measured
in `test/t_predicate.c`: **1256 of 16000 bytes (7.8%)** across four steps of
a 1000-element dataset, one of those steps sending **nothing at all** — and
that 7.8% is inflated on purpose, since one of the four steps is the
deliberately worst case where the match set is too fragmented to describe
run by run and coalesces to a superset. The three ordinary steps send 460 of
12000 (3.8%).

That last part is what makes a predicate categorically different from the
narrowings already in the protocol. Subvolume routing (M8.5) and
per-subscriber precision both still put *something* on the wire for every
write overlapping a subscription; a predicate is the only one that can take
a write to zero bytes, which is exactly the monitoring case — a reader
watching for an exceedance that occurs in one step out of hundreds.

**`design-plan.md` calls this "the expensive one … needs per-chunk stats
added to the format." That is true of a different problem than the one this
connector has.** The reasoning there is about *skipping stored chunks*
without reading them, which does need statistics `H5D_chunk_rec_t` has no
room for. But this connector evaluates at push time, when it is holding the
data in memory anyway, having just written it — so the predicate costs one
pass over a buffer already in cache and needs no format change, no schema
field, and no new dependency. The design-plan's cost estimate was attached
to the storage-side optimization, and got read as the cost of the feature.

Three things fell out of building it that are worth stating:

- **The `Entry.predicate` schema field, reserved for this since M2, stays
  unpopulated — it was misfiled.** A predicate belongs to a *subscription*
  (reader → writer, one per subscriber), not to a manifest entry (writer →
  file, one per write). Putting it on an `Entry` would have meant a step's
  manifest carrying one subscriber's query, which is neither meaningful to
  another subscriber nor to a later reader of the file. The field is left in
  the schema rather than removed: FlatBuffers ignores it, and removing it
  would renumber nothing but would rewrite history in the `.fbs`.
- **No wire-format change was needed for a discontiguous result.** Matching
  elements are delivered as one push per maximal contiguous run, each
  labelled with its own `elem_start`/`elem_count` — the identical trick the
  M8.5 strided-write fix already used, and for the identical reason (a push
  describes one flat range truthfully or not at all). A predicate matching
  one region stays a single push. Past a cap of 64 runs the match set is
  coalesced to the span containing it, a superset, never truncated — the
  alternative at the cap is either a hundred tiny RPCs or a truncated
  answer, and truncation here is data loss. `test/t_predicate.c` exercises
  that path with an alternating pattern producing ~100 runs, and checks the
  coalesced span is exactly the one containing every match.
- **An unevaluable predicate over-sends rather than under-sends.** Against
  a compound, a string, an unsigned 64-bit type whose values can exceed
  `LLONG_MAX`, or a float wider than `double`,
  `H5VL__stream_eval_predicate()` declines and the whole overlap goes out as
  if no predicate had been set. Same rule the rest of the routing code
  follows, for the same reason: over-sending is inefficiency, under-sending
  is data loss. `test/t_predicate.c` pins this with a compound-typed dataset
  carrying a predicate that cannot apply to it.

The comparison itself borrows HDF5 rather than reimplementing: both sides
are moved into one canonical type with `H5Tconvert()`, chosen from the
*data's* class, so a big-endian writer, a 16-bit-int subscriber and a
floating-point constant against integer data all work without this code
knowing anything about representation. The constant travels as `H5Tencode()`
bytes for the same reason. Only the small blob header — op, version, two
lengths — is hand-rolled, and it is written explicitly little-endian, which
is the lesson the VL length tag taught (see the cross-endian section).

#### Onion-backed addressable step history — shipped 2026-08-15

`h5stream history FILE OUT` writes an onion-VFD archive in which every step
is a numbered, re-openable revision holding the stream as of that step, at
ordinary logical paths. This is design-plan.md's "stream and archive as one
object" (#2): ADIOS2 makes you choose an engine, BP5 to a file or SST to a
stream, and a step that is both a live stream element and a nameable
revision collapses the choice.

The load-bearing property, which neither a finished stream nor `export` can
provide: **an older revision hides what later steps did.** A finished stream
shows every step at once and an export shows only the newest; only a
revision history answers "what did this file look like at step 1?".
`test/t_onion_history.c` turns on exactly that — an object written in the
last step is absent from earlier revisions — and reads everything back with
plain HDF5 and an onion fapl, no connector involved, because being readable
without this project is the point of writing an archive.

**Why this is a tool and not something `end_step()` does live.** Measured,
not assumed: the onion VFD commits a revision when the file is *closed*, and
exposes no "commit a revision now" call. One revision per step would
therefore mean closing and reopening the underlying file inside every
`end_step()`, invalidating every under-object the connector and the
application still hold — including the dataset handle an application
legitimately keeps open across steps, which is exactly what the capture fix
below just made work. Paying that in the live writer to gain what a
post-hoc pass produces for free is a bad trade.

**Three onion-VFD findings, each reproduced in a standalone HDF5 program
with no vol-stream code in it.** They are recorded here because they shaped
the tool, and because HDF5's own onion tests use page sizes 4 and 32 only
(`test/onion.c`), so all three sit outside that suite's coverage:

1. *Revisions are silently lost when the archive is small and the page size
   is large.* At `page_size >= 512` with a few-KB file, a three-session
   history reads back with the first revision empty and the third identical
   to the second. At 32/64/128/256 the same program is correct, and once the
   file comfortably exceeds one page every size tested is correct, 4096
   included. `H5Ocopy` is not implicated — direct `H5Dcreate`/`H5Dwrite`
   behaves identically at every page size.
2. *A reader must pass the page size the history was written with.*
   `H5FDonion.h` documents 0 as "whatever the file already uses", but a
   revision opened with 0 comes back apparently empty — the datasets are
   simply not found.
3. *A reader that passes the wrong non-zero page size aborts the process*
   rather than getting an error: `H5FD__onion_read: Assertion
   '0 == bytes_to_read' failed`.

The tool's response is to verify rather than trust: it writes the history at
the efficient 4096, reads every revision back, and rewrites at 32 if that
does not verify — cheap precisely in the small-archive case where the
problem occurs. Whichever size won is stored as an attribute in the
*canonical* file, which stays an ordinary plainly-openable HDF5 file since
every amendment lives in the `.onion`; `h5dump -a` reads it. Given finding 2
and finding 3, a reader that had to guess would either see an empty file or
crash, so self-description here is not a nicety. `test/t_onion_history.c`
asserts both branches: a tiny archive falls back to 32 and says so, and an
archive with real bulk data verifies at 4096 on the first pass, so no real
archive pays the small-page index cost.

**A connector bug this surfaced: vol-stream could only be registered once
per process.** Building a second stream in one test process failed at
`H5VL_stream_register()`, several frames away from the cause —
`H5VLregister_opt_operation(): operation name already exists`. HDF5's
dynamic-operation registry is global and outlives any one registration,
while the connector's init callback runs on *every* registration, so the
second collided with the first's names and the whole
`H5VLregister_connector()` failed. Register/close/register did not work, and
neither would two independent components in one program each registering the
connector. Nothing documented it and nothing caught it, because every test
until now registered exactly once. Fixed by making init idempotent —
`H5VL__stream_register_op()` recovers the already-assigned value with
`H5VLfind_opt_operation()`. Unregistering in `term()` instead would have
fixed the sequential case and broken the concurrent one, where the first
close would pull the operations out from under a live registration;
`test/t_reregister.c` pins both, including that a re-registered connector's
recovered operation values still drive a real step.

**A capture gap found by building it, unrelated to predicates and worse than
they were (2026-08-15).** M9's test needed a variable that *changes* step to
step — the first thing in this suite to write one dataset across several
steps through the handle it was created with. It wrote nothing. Capture
keyed strictly on a dataset wrapper still being a placeholder, i.e. created
in the step currently open and not yet materialized; once `end_step()`
replayed it, the wrapper went live and every later `H5Dwrite` through it
fell through to the under connector. Two silent consequences, both data
loss:

- the new step captured nothing, so no manifest entry, no push to any
  subscriber, and `/step/<n>/` was created empty (a 0-byte `.payload`);
- the write landed directly on the *earlier* step's copy of the object, so
  step history was not merely incomplete but retroactively wrong —
  `/step/0/temp` held whatever the last step wrote.

This is the most ordinary streaming pattern there is, and nothing covered it
because every existing test either wrote a single step or created a
differently-named object per step. Fixed by capturing a write to a live
dataset inside a step and synthesizing that step's own `DsetCreate` from the
object itself — its real datatype, extent and DCPL read back through the
under connector, so the step's copy is created exactly as the original was
and carries the whole extent even when the write covers part of it. That is
precisely what re-creating the dataset each step already produced, which is
the pattern that worked and the one decision #2 describes. Writes *outside*
a step still pass straight through, so M1's byte-identity promise is
untouched. `test/t_step_rewrite.c` pins both halves — each step holding its
own values, and a partial write landing at its own indices — checked through
the native connector, so it measures what is in the file rather than what
the connector believes.

#### Tool-compatibility matrix

Measured 2026-08-15 against HDF5 2.3's own tools, in all three states a
stream can be in. This is the "document it honestly, `h5dump` included" item,
and the honest answer has two halves rather than one.

| State | `h5ls` | `h5dump` | `h5stat` | Usable? |
|---|---|---|---|---|
| **Live** (writer holds it open) | `**NOT FOUND**` | error | error | **No** — use `h5stream tail` / `h5stream schema` |
| **Finished** stream | opens | opens | opens | Yes, but see below |
| **Exported** (`h5stream export`) | opens | opens | opens | **Yes, fully** |
| **Archived** (`h5stream history`) | opens¹ | opens¹ | opens¹ | Yes for the latest state; past revisions need an onion fapl |

**Live streams: the tools do not work, and cannot.** All three fail, and this
is not a shortcoming of the tools or something a future release fixes: while
a writer holds an HDF5 file open, a second process cannot read it. `h5ls`
reports `**NOT FOUND**` on a file that is plainly on disk with content in it.
This is the constraint named at the top of this document, confirmed by
measurement rather than argument, and it is precisely why the connector
carries a transport — `h5stream tail` follows a live stream over
`H5Fwait_step_ready()` because no file-reading tool can, and (M10)
`h5stream schema` lists its contents over `H5Fget_stream_schema()` for the
same reason `h5ls` cannot.

**Finished streams: the tools open, but show the layout, not the data.** They
work in the sense that nothing errors — and are still not what a user wants.
A dataset the application wrote as `/temperature` appears as
`/step/0/temperature`, once per step that touched it, alongside `.manifest`
and `.payload` bookkeeping datasets. `h5ls -r` on a two-object, three-step
stream lists eleven entries. Everything is present and correct; finding the
current value of one variable means knowing which step last wrote it.

**Exported files: ordinary HDF5, no caveats.** `h5stream export` collapses a
stream to each object at its logical path, newest version, with no `/step`
groups and no bookkeeping. All three tools work normally, and so will
anything else that reads HDF5 — this is the answer for handing a stream to a
plotting script, an existing analysis pipeline, or a colleague.

¹ **Archived files, and the one honest caveat about them.** The canonical
file an archive consists of is ordinary HDF5 and opens in any tool — that is
how the recorded page size is readable with `h5dump -a`. But a *past
revision* is not reachable from the command line: `h5dump` does have
`--vfd-name onion`, and its `--vfd-info` is a string that the tools library
casts straight to `H5FD_onion_fapl_info_t *`, so there is no way to spell a
revision number there. Measured: `h5dump --vfd-name onion` on an archive
prints the empty canonical file rather than any revision. Addressing a
revision therefore takes a few lines of C (`H5Pset_fapl_onion()` with
`revision_num` and the recorded `page_size`), which
`test/t_onion_history.c` shows end to end. This is a tools gap, not a
format one — the history is there and any HDF5 program can read it.

The practical guidance, then: `h5stream tail` while it is running,
`h5stream export` once it is not for a single current snapshot, and
`h5stream history` when the whole step history should be re-openable.
Pointing `h5dump` at a stream directly is the one path that disappoints, and
it disappoints in both states for different reasons.

**Measured 2026-08-15 — `tail` cannot be built by polling the file.** The
obvious implementation, re-opening the file and watching for new `/step/<n>/`
groups, works against a finished stream and reports nothing at all against a
live one. The reason is not subtle and is not ours: while a writer holds the
file open, another process cannot read it. Plain `h5ls` on a stream mid-write
prints `**NOT FOUND**` even though the file is on disk with content in it.

That is this document's own "existing tools on a stream" constraint, met in
practice rather than in theory — and it is precisely why the connector has a
transport. `tail` has to be built on `H5Fwait_step_ready()`, which makes the
transport a hard requirement of the subcommand (`VOL_STREAM_NA` set, writer
publishing its group) rather than a preference: there is no other channel by
which a second process learns a step was committed.

The transport-based implementation has since shipped, once its test harness
was built correctly. The first attempt had the reader racing a fixed-length
writer: an ad-hoc writer emits its steps and exits, destroying its SSG group
before the reader finishes joining, so the reader reports "Group not found"
and gives up. That looks exactly like a broken tool and is entirely the
test's fault.

The fix was to invert the dependency rather than to tune timings — the
writer now keeps committing steps until the *reader* signals it is done, so
the reader may join whenever it manages to and steps will still be arriving.
That removes the attach race outright instead of making it less likely.
`test/t_h5stream_tail.c` asserts the tool observed at least three steps while
the writer was still running, which is precisely what the polling version
could not do; 10/10 stress runs, each seeing all three.

This replaces the previous gate, which was *"a viewer written entirely in
Python follows a live C or Fortran writer, subscribing to a subvolume, with
no user-written C glue."* That gate is precisely the h5py deliverable, so
postponing h5py left the milestone with nothing reachable to prove. It is
kept verbatim here and becomes M9's gate again if h5py is picked up — the
Python-viewer scenario is a strictly better demonstration than the tool
alone, which is why it is parked rather than discarded.

The narrowing also changes what the milestone is worth. The bridge and the
bindings were the two items aimed at other ecosystems; what remains is aimed
at this one — making a live stream inspectable with the tools an HDF5 user
already has, which is also what the "existing tools on a stream" constraint
in the opening section says is the real adoption risk.

### M10 — live schema discovery · S

**Added 2026-08-18**, and not from the roadmap: it came out of asking whether
VisIt or ParaView could consume a live vol-stream, and finding that the
answer was no for a reason that belonged to this repo rather than to them.

The subscription protocol was complete except for its first question. A
subscriber must name a path and hand over a dataspace, and until now the only
way to have either was to already know them: `t_subscribe.c` hard-codes
`NSUB`, and `examples/heat_diffusion` takes the grid size as a command-line
argument on *both* sides with nothing checking that the two agree — its own
README says so. That is fine for a purpose-built monitor written against one
simulation. It is impossible for a generic consumer, because both tools ask
for metadata before they ask for anything else: VisIt's
`PopulateDatabaseMetaData` and ParaView's `RequestInformation` run before any
data request, and neither can offer a user a variable that the reader could
not enumerate.

Nothing on the wire carried that. The five RPCs were `step_ready`,
`get_current_step`, `reader_ack`, `subscribe` and `data_push`; a push carried
a path, raw bytes and a flat `elem_start`/`elem_count`, and no rank, extent
or datatype at all. `h5stream tail` had the same hole from the other side —
it could report that step 7 committed and not one word about what was in it.

**Shipped:** `H5Fget_stream_schema()` / `H5Ffree_stream_schema()`, a sixth
RPC (`vol-stream:get_schema`), and `h5stream schema` — which is `h5ls` for a
stream that is still being written, the state where `h5ls` itself reports
`**NOT FOUND**`. `h5stream tail` now prints the variable list once at attach.
`test/t_schema.c` is the gate: a reader that names no dimension, element
count or datatype of its own discovers three objects, subscribes with the
*discovered* dataspace, validates the pushed bytes through the *discovered*
datatype, and then requires the schema to have followed both a resize and a
newly created path.

Six decisions worth recording, because each had a plausible alternative:

- **Pull, not push.** The writer could broadcast its schema alongside
  `step_ready`, reusing a proven path. It would also put metadata on the wire
  once per step per member for something that changes on almost no step,
  which is the exact waste the reader-driven thesis exists to remove — and
  push-on-change alone strands a late joiner, who attaches to a steady stream
  and then waits forever for a change to learn from. Pulling makes the
  late-joiner case the ordinary one.
- **Published at `end_step()`, not produced by a callback in the handler.**
  Every other writer-side hook in `tr_mercury.h` is a callback
  (`vs_tr_refilter_fn` and friends), so a `vs_tr_schema_fn` would have been
  the consistent-looking choice. It would also run inside a Margo/Argobots
  RPC handler, and encoding a datatype is an HDF5 call — which every handler
  in that file is forbidden to make, because v1 is not
  `H5VL_CAP_FLAG_THREADSAFE`. The writer hands over finished bytes from its
  own thread instead, where the encoded blobs already exist because the
  step's manifest just built them.
- **Change detection by `H5Tequal()`/`H5Sextent_equal()`, not by comparing
  freshly encoded bytes.** Comparing bytes means encoding every path every
  step to discover that nothing changed, which is precisely the
  O(steps × paths) bookkeeping the manifest's own type cache was added to
  avoid. Two library calls per entry answer the same question, and a steady
  stream rewriting the same objects publishes once and then pays nothing.
- **The schema's dataspace is the extent, with everything selected.** An
  entry's captured space carries whichever selection the write that produced
  it used, and a consumer that copied that would silently subscribe to one
  writer's partial write. A schema describes the object; a selection
  describes a write.
- **Hand-rolled little-endian framing, not flatcc.** The manifest is
  persisted into the file and has to stay readable by later versions, which
  is what flatcc's evolution rules buy. A schema blob lives for one RPC
  between two processes that are talking right now, so a version byte covers
  it — and it keeps flatcc out of the transport module, which has stayed
  HDF5-free and schema-free on purpose.
- **The decode bounds-checks everything.** It is the only place in the
  connector that structurally walks a buffer produced by another process, and
  a truncated or malformed blob has to fail the call rather than index past
  the end of it. Lengths are compared against the remaining space
  (`len > len - off` form) so an overflowing length cannot wrap past the
  check.

**One honest limitation, and one thing this deliberately does not do.** A
parallel writer answers a schema query from whichever rank's group the reader
joined — every writer rank starts a transport and publishes a group, and the
`.vsgroup` sidecar carries the last one to write it. Under the ordinary
parallel-HDF5 pattern — every rank creates the same
objects and writes its own hyperslab — that rank's schema is the whole
stream's, extents included, because an entry's dataspace is the dataset's
own. A writer whose ranks create *different* objects (M6.5's heterogeneous
case) therefore under-reports here, unlike the manifest, which is aggregated
across ranks at replay. And a schema is a list of arrays, not a mesh: a
visualization tool still needs a convention on top (Conduit Blueprint, which
VisIt reads natively, or a VTKHDF-shaped layout) to know that one array is
coordinates and another is a field on them. That convention belongs to the
reader plugin, not to the connector, which has no business inventing a data
model HDF5 does not have.

**Measured, for the two tools that motivated this** (2026-08-18). ParaView
5.12.0, as installed on the machine this was written on, ships HDF5 1.14.2 and
exports `H5VLregister_connector`, `H5Pset_vol`, `H5VLregister_opt_operation`,
`H5VLfind_opt_operation`, `H5VLfile_optional_op` and `H5PLappend` from its
bundled `libhdf5.310.2.0.dylib` unmangled — so a connector built against the
same version can be loaded into it, and the step API resolves. VisIt's current
release builds against HDF5 **2.0.0**: `bv_hdf5.sh` pins `HDF5_VERSION=2.0.0`
at the `v3.5.0` tag, so a current VisIt has the VOL layer too. (The VisIt 3.3.1
build installed here ships HDF5 1.8.14, which predates the VOL entirely —
worth knowing only for someone on an older VisIt.)

Neither tool can consume a live stream without a reader plugin of its own —
nothing here changes that — but the metadata such a plugin has to have before
it can be written now exists, and on both tools' current releases the
connector itself is loadable.

## After M10

Work done after the last numbered milestone, recorded here so the plan still
matches the code. Each item is documented for users in
[`user-guide.md`](user-guide.md).

- **Membership moved from SSG to mochi-flock.** SSG was deprecated by its
  maintainers; `src/tr_mercury.c` now uses Flock's SWIM backend behind the same
  `vs_tr_*` seam. CI found two Flock bugs under concurrent joins: a group-file
  race fixed on Flock's `main` (7b484c5) but unreleased, and a crash when a
  joiner's provider receives gossip before `init_group()` returns
  (mochi-hpc/mochi-flock#8, fixed on `main` by #9). Neither fix is in a
  release yet; CI builds Flock `main`, and the Flock 0.8.0 release is a
  documented known limitation.
- **A Python subscriber**, `volstream`, milestones P0–P5 in
  [`python-plan.md`](python-plan.md): a CPython extension over the C API,
  NumPy arrays per step, pip-installable against the machine's own HDF5.
- **Per-step push grouping is a pinned guarantee.** The writer finishes
  delivering a step's pushes before it announces the step, so a subscriber
  woken by step-ready can drain that step without waiting. `test/t_step_grouping.c`
  pins it, including a lagging reader and a late joiner.
- **End of stream for subscribers.** `H5F_STEP_EOS` was declared but never
  set. A subscriber now treats every writer member leaving the group as the
  end of the stream: steps announced before the departure are still delivered,
  and afterwards `H5Fstep_status()` reports `H5F_STEP_EOS` and the waits return
  at once. Step-ready messages carry the writer's member id (a wire change), and
  a writer is also recognised from its answer to subscribe, schema and ack
  requests. `test/t_eos.c`.
- **Opt-in backpressure for subscribers.** `H5Fack_stream_step()` sends the ack
  a cursor reader sends from `H5Fbegin_step()`, so a writer's queue policy can
  count a subscriber.
- **Attribute writes through a handle kept open across steps are captured.**
  The same bug `t_step_rewrite` pins for datasets, left open for attributes:
  such a write was never pushed and overwrote the earlier step's copy. Fixed by
  synthesizing the step's own Attr entry; `t_step_rewrite` covers it.
- **A test-only fault hook.** `VOL_STREAM_TEST_DROP_PUSH=<k>` makes a writer
  skip its k-th push while still announcing the step, the only way to reach a
  subscriber's lost-push handling in a healthy run.
- **A subscriber is told when a narrowing fell back.** Every push carries
  `H5VL_STREAM_DELIVERY_*` bits (a wire change), returned through a new last
  argument of `H5Fget_subscribed_data()` (an API change; NULL ignores it): the
  selection sent as its bounding span, a predicate not evaluated, matches
  coalesced to their span, or a type conversion declined. `t_predicate` and
  `t_subvolume_strided` pin exact pushes at 0 and each fallback's bit.
- **Phase 1: large push payloads go by Mercury bulk.** A payload of at least
  `VOL_STREAM_BULK_THRESHOLD` bytes (64 KiB default, provisional) is
  registered and pulled by the subscriber; smaller ones stay inline. A push
  from the caller's memory completes before `vs_tr_writer_push_data()`
  returns, which keeps that function's contract for every replay path; owned
  refilter output is freed on completion. `bulk_push` moves 256 MiB and
  checks it; ten C and three Python tests rerun with every push by bulk.
- **Non-fatal diagnostics from `H5Fend_step()` reach the caller.** Every
  public HDF5 call clears the error stack on entry, and that includes the
  `H5VL*()` calls the connector makes to do its own I/O. So a frame pushed
  mid-step was erased by the connector's next call, and the API wrapper's own
  `H5VLclose()` after the op erased whatever was left. The wrapper now closes
  first, and end_step collects its notes and pushes them as its last action.
  A `Discard` drop is reported that way (`t_queue_policy`), and so is the
  parallel Spill-as-Block warning, which had never been visible.
- **A failed step-API call says why.** Wrong role (a reader-only call on a
  writer, or the reverse), no transport, no next step for a reader, an
  unknown logical id, a failed replay, a bad subscribe argument, a predicate
  on an unsubscribed path: each now pushes a frame naming the reason, not
  just HDF5's generic callback failure. Timeouts stay silent, since polling
  with 0 is normal. `t_error_messages`.
- **Variable-length data reaches subscribers.** The writer pushes the
  serialized form capture already made (`vs_tr_writer_push_opaque()`, marked
  with an internal delivery bit), and `H5Fget_subscribed_data()` decodes it
  into one allocation -- pointers first, bytes after -- so one `free()` still
  releases it. The delivery flags report the narrowings it could not apply;
  a whole-object subscription gets none. `t_vl_push`: VL strings (NULL and
  empty kept apart) and VL int sequences, exact.
- **The `/stream` overlay: a timeline for native tools (and H5Web).** Opt-in
  (`overlay` / `VOL_STREAM_OVERLAY`). `/stream/<path>` is a "printf" virtual
  dataset, `[rows, dims...]` with an unlimited first dimension, whose row j is
  `/stream/.steps/<path>/j` -- a hard link to the copy that is current as of
  step `first_step + j`. So a step that did not write the dataset repeats the
  last value, there are no gaps, no data is copied, and the view grows by one
  link per covered dataset per step. Fixed-shape, non-VL datasets only; off
  under a retention policy and for parallel writers. `t_overlay` checks it
  natively; h5dump and h5py (and so H5Web through h5grove) read it.
- **The overlay covers growing frame stacks, and is NeXus.** A dataset growing
  along dim 0 (rank 2 or more), such as a detector's `[nP, i, j]`, was
  excluded, although it is the layout NeXus prescribes. It is now viewed a row
  per frame: `/stream/.frames/<path>/k` is a one-mapping VDS selecting frame k
  in the copy of the step that wrote it (carry-forward is 1-D only, so no one
  copy holds every frame), and the view is a printf VDS over those. That is a
  VDS of VDSs, which plain HDF5 reads. No data is copied; there is one small
  object per frame. Every covered dataset is also an `NXdata`,
  `/stream/<path>`, with `data` (the view), a `step` axis, `@signal`, `@axes`
  and `data@interpretation`, under an `NXentry` `/stream` whose `@default` is
  the first. That moves the view from `/stream/<path>` to
  `/stream/<path>/data`. The root `@default` is left alone: an application
  that later created it would fail. `t_overlay`.
- **Phase 1 follow-ups: shared registrations, overlapped transfers.** A
  borrowed bulk push now pulls from a region registered once -- the step's
  whole staging buffer, or one write's data -- at an offset carried in the
  push (a wire change), so a step registers once however many pushes it
  makes; the refilter's owned output still registers per push. And
  `vs_tr_writer_push_data()` no longer waits for its pulls: they complete
  at the end of the step's replay (`vs_tr_writer_release_sources()`), so a
  large transfer overlaps the rest of the replay. A subscriber's converted
  copy is freed there too rather than waited on. `bulk_regions`: 32 bulk
  pushes of a column from one registration, values checked.
- **No crash when HDF5 walks open objects mid-step.** A dataset created in
  the open step has no native object until the step commits, but HDF5 asks
  for one whenever it walks the open dataset IDs (a file close's flush, a
  second handle's close) and dereferenced the NULL it got. The connector now
  hands it a stand-in: one real dataset in a private in-memory file, which
  those walks skip. And closing the file with such a dataset still open
  discarded the step under it, so the dataset's own close wrote into freed
  step state; it now checks. `t_open_step_objects` crashes without either.
- **Flush and refresh behave consistently, so `FLUSH_REFRESH` is accurate.**
  The flag only promises that the calls are supported, and the library never
  reads it, so nothing needed a core change. Inside an open step `H5Dflush()`
  and `H5Gflush()` used to fail and `H5Fflush()` could crash (the native flush
  walks open dataset IDs, and one created in the step has no native object
  yet). Now every flush succeeds, a file flush is deferred to the step's
  commit, a refresh fails with a reason, and a reader's virtual group
  refreshes. `t_flush_refresh`.
- **Backfill for late joiners: `H5Fsubscribe_from()`.** A reader that joins
  at step 500 can get steps from any earlier one. The writer reads each
  missed step back from its own file on its own thread (HDF5 is not
  thread-safe, so the subscribe handler only records the request) and sends
  it to that reader alone before the next live step. The subscriptions are
  created staged and released together, and the reader gets nothing live
  until it is served, so every step arrives once and in order. Reading the
  history reader-side was rejected: the file is not consistent on disk while
  the writer runs (no per-step flush, and HDF5 does not support reading a
  file another process writes, outside SWMR). `t_backfill`,
  `python_stream_backfill`.
- **The connector's error frames reach the caller.** Frames are buffered
  while a connector callback runs and pushed as its last action, so the
  connector's own HDF5 calls after a failure -- cleanup, closes -- no longer
  erase them. It covers the step API and the dataset, attribute, file-open
  and group-open callbacks, and replaces end_step's note mechanism.
  `t_config` checks that a pending-limit refusal's reason reaches the caller,
  and fails without the buffering. Frames are also pushed with a `"%s"`
  format now; a `%` in a path used to be read as a conversion.
- **Per-file configuration on the FAPL.** `H5Pset_fapl_stream(fapl,
  &config)` sets the NA string, payload staging, the pending-bytes cap, the
  spill directory, the concentrator factor and the bulk threshold for one
  file, so one process can drive two streams configured differently. The
  config travels in the connector info, so it also works in an
  `HDF5_VOL_CONNECTOR` string, and an environment variable that is set still
  overrides its field. `t_config` (two files, the cap, the override, the
  string form) and `t_config_na` (the transport per file). Also fixed: the
  info string parser wrote its terminator one byte past its buffer.
- **An attribute written before its dataset in a step no longer loses the
  step.** Its entry came first, replay made a group of the dataset's name, and
  the dataset's create then failed. Replay now does a step's attributes after
  its other entries. `t_step_rewrite` pins it, and fails four ways without it.
- **Variable-length objects are no longer pushed.** The push read the rebuilt
  pointer buffer after it had been freed. `t_vl_push`.
- **Archival step-count scaling, measured.** `test/b_step_scale.c` writes N
  retained steps of two 64-double datasets and a scalar attribute, rewritten
  through open handles. On an M-series Mac, local SSD, HDF5 2.3.0:

  | steps | `H5Fend_step()` first 100 / last 100 | file bytes per step | reader `H5Fopen()` | last-step seek + read |
  |---|---|---|---|---|
  | 100 | 0.37 / 0.37 ms | 3162 | 9.0 ms | 0.04 ms |
  | 1,000 | 0.35 / 0.41 ms | 3148 | 18.1 ms | 0.04 ms |
  | 10,000 | 0.41 / 0.40 ms | 3148 | 21.9 ms | 0.05 ms |
  | 100,000 | 0.35 / 0.37 ms | 3148 | 8.9 ms | 0.13 ms |

  With `VOL_STREAM_STAGE_PAYLOAD=0`; with staging on, about 4.5 KB per step
  and the same times. Nothing grows with step count: a step costs the same at
  100,000 as at 100, and the flat `/step/<n>` namespace (dense group storage)
  is not a bottleneck at this scale. About 2 KB of each step is metadata
  around its 1 KiB of data -- the per-step overhead to budget for.
- **A chunk shape of any rank is honored** as its element count per push,
  where rank 2 or more used to be ignored. `t_chunk_shape_split` covers a
  (2, 8) chunk on an 8x8 dataset.
- **Python: a box that follows growth, and `tail=True`.** `count[0]=None` in
  a `(start, count)` selection subscribes every row from `start[0]` on,
  including rows not yet written: an ROI of every future frame. `tail=True`
  returns only the rows a step sent, with `step.first_row[path]`, so a
  step's cost no longer grows with the run. `python_stream_growtail`;
  `examples/silx_live_view` uses it. That subscription is a hyperslab about
  2**60 rows tall, and the writer's flat-run decomposition walked it one row at
  a time: CI hung. It now starts at the first row the push's range reaches,
  stops past its end, and takes a block spanning every trailing dimension
  as one run.
- **A reader can tell a finished stream from one that has not started.** A
  reader recognises the writer from its announcements or answers, so one that
  opened the file after the writer left had neither and waited forever: it
  could not tell "not started" from "finished". The writer now leaves
  `<file>.vsdone` when the file state is torn down, written atomically after
  its last step was announced, and removes a stale one when it starts. A
  reader that finds it at open does not join (the group file may still name
  the departed writer), and reports the end of the stream once nothing is
  queued. `H5Fwait_step_ready()` fails at once rather than waiting out its
  timeout, and looks for the marker every 250 ms while no writer is known.
  A sidecar rather than an attribute in the file: a reader that opened the
  file while the writer ran cannot see metadata added later without SWMR,
  and Flock owns and rewrites `.vsgroup`. Every test that removed `.vsgroup`
  between runs now removes `.vsdone` too, or a rerun's early reader would
  take the last run's marker for this one. `t_writer_done`.
- **A step reader's `H5Lexists()` resolves to the step.** It passed through
  to the live namespace, where an object the stream carries does not live,
  so it answered false for everything the step had. It now answers from the
  path index as opens resolve, and asks the file only for what the stream
  does not carry (a live group). `t_links`.
- **Links made in a step go into the stream.** A NeXus writer links
  `/entry/data/data` to its detector dataset in the step that creates it.
  That went to the live namespace, where the target does not exist (a
  dataset lives only under `/step/<k>/`), so a hard link failed and a soft
  one dangled. A link made in a step is now a `Kind.Link` manifest entry, the
  first use of that reserved kind. Replay makes it in `/step/<n>/`, naming
  the target's copy as of that step, and the path index learns the link's
  path, so a connector reader opening it resolves there. A later step that
  writes the target gets the link again, so every step's native view is
  complete. The overlay skips link paths, so data is not covered twice.
  Still passed through: a link made outside a step. Not routed: a
  subscription to a link's path (subscribe to the target). `t_links`.
- **Variable-length data: backfilled, and strings in Python.** Backfill
  skipped a variable-length object. It now reads what the step wrote back from
  its copy, serializes it as capture does, and pushes it to the one
  subscriber flagged serialized, as the live push does
  (`vs_tr_writer_push_opaque_to()`; not on the diaspora backend). The
  Python binding refused every variable-length type. A top-level
  variable-length string now arrives as an object array of `str`, `None` for
  an unset one: the extension's `vl_strings()` reads the connector's decoded
  pointers while their allocation lives. Sequences, and strings inside a
  compound, are still refused. `t_backfill` (a string dataset through
  backfill), `python_stream_types` (a string dataset and attribute).
- **A step whose replay fails is removed.** `H5Fend_step()` does not advance
  the step number when replay fails, so the next step replays into the same
  `/step/<n>/`. The failed replay's objects stayed there, where the next
  replay could collide with them and a native reader found them, and the
  writer's path index still named step n for them. The failed step's group is
  now unlinked and its path-index entries dropped, so the retried step
  starts clean (serial writers; a parallel writer's unlink would have to be
  collective). `t_failed_replay`, which provokes the failure with a real name
  collision.
- **An attribute-only step shows a dataset natively, not a group.** An
  attribute written in a step that does not write its dataset had no copy of
  the dataset in `/step/<n>/` to attach to, so replay made a group of that
  name, and a native reader found a group where the dataset should be. Replay
  now makes `/step/<n>/<path>` a virtual dataset mapping all of the dataset's
  last written copy, `/step/<r>/<path>`, and attaches the attribute to that.
  Group parents (`/entry@NX_class`) still get a group. `t_step_rewrite`.
- **A writer that exits mid-step no longer crashes, or loses committed
  steps.** Returning with a step open and a dataset from it unclosed crashed
  as HDF5 shut down: the connector's `H5atclose()` callback closed the
  native stand-in that `get_object` returns for that dataset, and closing
  the stand-in's file walks every open dataset ID, the leftover one included.
  The crash also skipped the file's final flush, so steps that had already
  committed were missing from the file. The callback now leaves the stand-in
  to the library's own teardown when a placeholder is still open (dataset
  IDs close before files there), and discarding the open step skips IDs the
  teardown has already freed. `t_exit_midstep`, which fails both ways without
  the fix.
- **Group attributes written outside a step reach the stream.** A NeXus
  writer sets `NX_class`, `@signal` and `/@default` on its groups before the
  first step. Those writes passed through to the live groups and nothing more:
  no `/step/<n>/entry` had `NX_class`, and no subscriber or connector reader
  saw it. `H5VL__stream_note_group_attr()` now records each such creation, and
  `H5Fend_step()` reads the value back into the committing step, unless the
  step wrote it itself. The live write stays byte-identical. Not carried: an
  out-of-step *rewrite* through `H5Aopen`, and a parallel writer's attributes.
  `t_group_attrs`.
- **A backfilled step sends what it wrote, not its whole copy.** For a
  dataset growing in N-D, a step's copy has fill in the rows other steps
  wrote, because carry-forward is 1-D. Backfill sent the whole copy, so a
  late joiner's earlier frames were overwritten with fill. `python_silx_live_view`
  caught it once it used `tail=True`: frame 0 arrived a second time as zeros.
  Backfill now reads the step's manifest and pushes each DsetWrite selection as
  runs, as the live push did. It falls back to the whole copy only when the
  manifest is unreadable or a selection is too fragmented.
- **`H5Dset_extent()` on a dataset created in the open step now works.** A
  NeXus detector writer creates its frame stack empty (`[0, i, j]`,
  unlimited) and extends it before the first frame, all in one step. That
  extend returned a bare -1, since such a dataset is a placeholder with no
  real object, so `examples/detector_pipeline`'s writer failed on frame 0.
  Nothing ran that writer in CI until `examples/silx_live_view` did. The
  resize now changes the pending create's extent, which nothing reads before
  the commit. Growing is allowed; shrinking is refused with a message, because
  it could orphan writes already staged. `t_placeholder_extend`.

## Remaining gaps

As of 2026-09-24. Each is documented where it applies (user guide §2.3,
[`python-plan.md`](python-plan.md)'s Known gaps, the RFC's open items); this is
the one list. ★ marks what is being worked on next.

**Transport**

- **Bulk transfer (RFC Phase 1) is built** (see *After M10*). Still open
  from its gate: the throughput comparison against a Phase 0 baseline (needs
  an RDMA fabric) and the 1 GiB run (`T_BULK_MIB=1024 t_bulk_push`, by hand).
  An ASan build with the transport runs in CI's transport job, and gates
  (its first run was clean).
- HMEM provider and device-direct delivery (RFC Phases 2 and 3), after Phase 1.
- The payload-size sweep on a real RDMA fabric (RFC Phase 0), which needs
  multi-node hardware. It sets Phase 1's priority and threshold, not whether
  Phase 1 happens.

**Protocol and semantics**

- Variable-length data is pushed whole (no selection, predicate or type
  narrowing), only for writes whose selection is one contiguous run. The
  Python binding delivers top-level strings, not sequences.
- Backfill (`H5Fsubscribe_from()`) must be a reader's first subscription,
  waits for the writer's next step boundary, and is not on the diaspora
  backend.
- A reader that vanishes without closing costs the writer up to one push
  timeout (1 s) per step until SWIM declares it dead -- about 5 s in CI, after
  which its subscriptions are dropped. Accepted as the bound: skipping a
  subscriber after one timeout would lose a slow-but-alive reader's data. A
  writer that is killed before a reader has identified it is not seen to
  leave by that reader (a writer that closes leaves `<file>.vsdone`).
- Error-stack coverage is complete for the step API's reachable failures,
  not yet for every interior helper of the object callbacks (dataset,
  attribute and group creation and I/O), which still mostly rely on HDF5's
  own frames.
- No fault tolerance for a rank failure inside a collective commit.
- Writer and reader cannot cross an HDF5 major.minor boundary.

**Configuration and release**

- Connector value 1091 is provisional; request an official one before release.
- A second, native handle to a file the connector has open in the same
  process is not supported: HDF5 shares one file state between them, and
  closing the native handle mid-step fails (with an error, not a crash).

**Measurement**

- Archival scaling is measured to 100,000 steps on one laptop-class machine
  (`test/b_step_scale.c`, see *After M10*), not on a parallel file system.
- The ADIOS2 SST comparison is single-node, single-rank and statistically
  informal.
- `precision_dual` (the literal M8 exit gate) is still `DISABLED`.

**Python binding** (details in [`python-plan.md`](python-plan.md))

- Variable-length sequences, reference and bitfield types are refused, and a
  variable-length string inside a compound. Top-level strings are delivered.
- Per-subscriber precision is deflate only; other filters and chunk shape are
  not exposed.
- A whole-dataset subscription follows growth along the first dimension only.
- No strided selection. It would not help yet anyway: the writer sends one
  push per contiguous run of the intersection and falls back to the whole
  bounding span past 256 runs (`SELECTION_SPAN`). So a subsampled frame,
  such as every 4th pixel, would cross the wire in full. A useful stride
  needs a packed delivery, meaning the selected elements gathered into one
  push, which is a protocol change.
- `StreamDataset` is `num_workers=0` only (by design).
- One process-wide HDF5 lock: a blocking wait on one `File` can delay a call on
  another by up to 100 ms.
- A step that arrives in several pushes is copied into one array.
- Source build only, by design; no wheel.
- GCC's own library directories in the installed RPATH on macOS, from the
  Homebrew GCC driver; nothing loads through them. (pip's temporary build
  directory is no longer there: `BUILD_WITH_INSTALL_RPATH`.)

**CI and upstream**

- Flock is built from `main` until a release includes mochi-flock#9 (merged
  2026-09-30) and the group-file fix (7b484c5); then pin to that release.
- The `ofi+tcp` pass does not gate.
- The diaspora backend is not built in CI, so changes to it are unverified.
- CI is Linux only; `volstream.torch` is tested only where torch is installed.

## Stretch goals

Recorded rather than scheduled: real ideas with no milestone number, no exit
gate, and no commitment to build them. Kept here so the reasoning survives
even if nobody picks one up for a while.

### A ParaView/VisIt reader plugin

**Motivation.** M9's tool-compatibility matrix already establishes that a
live stream is unreadable by any file-opening tool, `h5dump` included —
`h5ls` reports `**NOT FOUND**` against a file a writer still holds open. A
purpose-built visualization or analysis tool coupled to a running simulation
hits exactly this wall, which M10 (live schema discovery) was built to
answer: `H5Fget_stream_schema()` gives a reader with no prior knowledge of
the stream a variable list before it subscribes to anything.

**What is already measured, not assumed.** ParaView 5.12.0 ships HDF5 1.14.2
and exports `H5VLregister_connector`, `H5Pset_vol`,
`H5VLregister_opt_operation`, `H5VLfind_opt_operation`,
`H5VLfile_optional_op` and `H5PLappend` unmangled from its bundled
`libhdf5.310.2.0.dylib` — a connector built against the same version loads
into it and the step API resolves. VisIt's current release (`v3.5.0`) builds
against HDF5 2.0.0 per `bv_hdf5.sh`, so it has the VOL layer too. Neither
ships a reader that speaks `H5Fsubscribe()`/`H5Fget_stream_schema()` — that
plugin does not exist yet in either tool, and writing one is the actual
stretch goal.

**Shape of the plugin, if built.** `VisIt`'s `PopulateDatabaseMetaData` and
ParaView's `RequestInformation` both run before any data request and need a
variable list to offer a user — precisely what `H5Fget_stream_schema()`
provides. `examples/heat_diffusion/heat_monitor.c` is already the minimal
version of this: it discovers `/temperature`'s dataspace from the schema
rather than trusting a command-line argument, subscribes with the discovered
space, and redraws on every `H5Fget_subscribed_data()` push. A real plugin
is the same loop wearing the tool's own reader interface instead of an ASCII
heatmap.

### Conduit Blueprint as the data-model convention

A schema entry is a typed array at a path; it has no notion of "this is a
coordinate array" or "this field lives on that mesh." Blueprint supplies
exactly that, and unlike netCDF or CGNS it costs nothing to make legal here:
Blueprint is a set of conventional HDF5 paths and attributes
(`/coordsets/<name>/values/{x,y,z}`, `/topologies/<name>/type`,
`/fields/<name>/{association,topology,values}`), not a separate C library
with its own `H5Fopen`-equivalent. `conduit_relay_io_hdf5` reads and writes
that convention with ordinary `H5D`/`H5G`/`H5A` calls, so nothing about the
connector needs to change for a writer to legally produce it — a simulation
that names its `H5Dcreate2()`/`H5Awrite()` calls at Blueprint paths already
gets a Blueprint-shaped stream, no bridge required.

Confirmed rather than assumed: `H5VL_stream_group_create()` always passes
groups straight through, live, uncaptured in the step manifest — its own
comment says a group "exist[s] only to supply path bookkeeping for their
children's manifest entries," and replay rebuilds any needed ancestor group
via `H5Pset_create_intermediate_group()`. The nested `/coordsets/coords/...`
hierarchy Blueprint wants therefore needs no special handling; it is already
exercised by every nested-path test in this suite.

**The real gap, not glossed over.** `H5Fget_stream_schema()` reports a
path's *type and extent*, never its *value* — so a Blueprint descriptor like
`/topologies/mesh/type` (a string: `"structured"` vs `"unstructured"`) is
visible to a reader as "an attribute of this datatype exists" but its actual
content still has to arrive over `H5Fsubscribe()` + `H5Fget_subscribed_data()`,
which delivers on the *next write*, not retroactively. Nothing in the
subscription protocol replays a push that happened before a reader
subscribed, so a reader either has to attach before the writer's first step,
or the writer has to cheaply re-declare small Blueprint metadata every step
so a late joiner eventually catches one. Real, and worth solving properly if
this is picked up — not a one-line fix, since it would mean either a
retroactive-delivery mode for a subscription (a genuine protocol change) or
a documented convention that Blueprint descriptors are written every step.

**Precedent.** This is structurally what Ascent already does for in-situ
visualization: export a Blueprint-conforming Conduit node each cycle, hand
it to a VisIt or ParaView Catalyst back end. A vol-stream reader plugin
would be a different, reader-driven transport for the same Conduit-node
shape — replacing Ascent's typically co-located, in-memory hand-off with
`H5Fsubscribe()` over the wire — not a new data model.

## CI matrix

| Axis | Values | Why |
|---|---|---|
| HDF5 version | develop only (see note) | The documented failure mode; `H5VL_VERSION` is 3 and will move |
| MPI | MPICH · OpenMPI | Intercommunicator and dynamic-process behaviour differ in practice |
| Mercury NA plugin | `na+sm` (gating) · `ofi+tcp` (subset, non-gating) | Where transport bugs live; shared-memory and TCP run on any CI box. `ofi+verbs` needs RDMA hardware CI does not have |
| rank shapes | 3→2 · 7→3 (uneven) | Coprime counts are where M×N projection bugs surface |
| encode round-trip | byte-order assertion (see note) | The manifest leans on HDF5's encoders; prove them across byte order |
| Mochi stack | pinned releases, Flock from `main` plus a patch | Built from source in the transport job; Spack environments are planned, not in CI |
| Python | CPython 3.12, NumPy, CPU torch; installed with pip | The subscriber package, including its installed form |

**Cross-endian, without a big-endian machine (2026-08-15).** Reviewing the
format against this row turned up a real portability bug rather than
confirming one: the VL wire form's per-element length tag was `memcpy`'d from
a `uint64_t`, i.e. stored in the *host's* byte order. A file written on a
little-endian machine would have been unreadable on a big-endian one — every
tag byte-swapped, so lengths nonsense and the data either failing its bounds
check or decoding as garbage.

It was the only field at risk, which is why it was worth looking: FlatBuffers
is little-endian by specification, and the `H5Tencode`/`H5Sencode2`/
`H5Pencode2` blobs are HDF5's own portable encodings, so the hand-rolled tag
was the one place this project chose its own representation. It is now
written and read as explicit little-endian.

Note what this says about testing: the bug passed every round-trip check in
the suite, and would have on any single machine, because writer and reader
shared the host's byte order. `test/t_vl_roundtrip.c` therefore asserts the
*stored bytes* directly — it reads `.payload` back and requires the tag to be
little-endian — which fails on a regression regardless of the host it runs
on. That is real coverage of this row's intent; running an actual big-endian
leg (QEMU s390x) would additionally exercise HDF5's own encoders, and remains
open.

**HDF5 2.x is the only support target — this row is closed, not pending
(2026-08-15).** The original row called for develop, latest release, and
oldest supported, which read like unfinished work. It is not: 2.x is the
deliberate scope.

The code already reflects that and cannot easily do otherwise. The connector
uses `H5Tdecode2()` at 8 call sites — including the core manifest decode path
every replay runs through — and `H5Dread_chunk2()` at 5. Neither exists in
1.14.6, the latest release, checked against that tag's own headers rather
than inferred. A 1.14 job would not fail on some compatibility detail; it
would not compile.

So the row now says develop, matching the `build` job's own comment. Nothing
here is blocked on CI work; supporting 1.14 would mean writing a
compatibility layer for those APIs *and* changing what the project targets.
| sanitizers | ASan · UBSan · TSan | A progress thread plus a queue is the classic place for races |

## Residual risks

**The manifest inherits HDF5's encoding compatibility rules.** Leaning on
`H5Tencode` and `H5Sencode2` is the right trade, but it couples the wire format to
them, and `H5Sencode2` already changed once to widen selections to 64-bit. The
schema records `hdf5_version` from the first commit and CI runs a cross-endian
encode round-trip, so a future change is *detectable*. **Update:**
`H5VL__stream_replay_manifest()` now reads `hdf5_version` back and compares it
against the running library's own major.minor (ignoring the release/patch
digit, which HDF5's versioning policy never lets change encoding format) before
either decode call runs, refusing with a clear `stderr` diagnostic on a
mismatch rather than risking whatever `H5Tdecode2()`/`H5Sdecode()` do with
bytes from an incompatible encoder. Verified two ways: the full existing test
suite (writer and reader always share one build here, so this exercises the
no-op/compatible path end-to-end) and the comparison arithmetic itself checked
in isolation against the actual 1.10→1.12 precedent. **Not verified
end-to-end:** the mismatch-detected branch itself, which needs a second real
HDF5 build (a different minor series) to trigger for real — not available this
session; the CI matrix's own "HDF5 version" axis builds the *connector*
against different HDF5 series one at a time, it does not yet write with one and
read with another in the same test.

**Manifest metadata was fully re-encoded every step, with no dictionary.**
`H5VL__stream_build_manifest()` calls `H5Tencode`/`H5Sencode2` (and
`H5Pencode2` for create-kind entries) on *every* pending entry on *every*
step. `dcpl_enc` was never actually part of this cost for `DsetWrite` (its
`dcpl_id` is always `H5I_INVALID_HID` — only `DsetCreate`/`Attr` carry one,
and those happen once per object, not once per step); `type_enc` was the
real, live cost, since a `DsetWrite` re-sent it in full every step even
though a dataset's type never changes after creation. `space_enc` is left
alone — a write's selection legitimately varies step to step and is not
part of this fix.

**Update:** `type_enc` is now cached per path, on both the writer side
(`H5VL__stream_type_cache_lookup`/`_upsert`, omitting the field when it is
byte-identical to the last one sent for that path) and the replay side
(a mirror cache, resolving an omitted field back to real bytes — both sides
run in the same process, so no cross-process synchronization is needed).
No schema change: an omitted flatbuffers vector field decodes as a 0-length
result via `flatbuffers_uint8_vec_len()`, a sentinel no valid `H5Tencode()`
output can produce, so this is unambiguous and fully backward compatible.
Verified in `test/t_manifest_cache.c` by comparing the *persisted*
`/step/<n>/.manifest` dataset's on-disk size across three steps writing the
same path: step 0 (create + first write, nothing to omit) is measurably
larger than steps 1–2 (repeat writes, `type_enc` omitted), and steps 1–2
land at the identical, stable size — a black-box proof the omission is
real, not just a code-reading claim, plus a correctness check that the
cached path never changes what actually lands in the file.

**Existing tools on a live stream.** See the constraint section above.

**A resize across steps silently corrupted an earlier, already-committed
step (found 2026-08-15, verifying ADIOS2-comparable coverage rather than
reviewing code).** ADIOS2's ordinary pattern for a time-series array is an
unlimited-dimension variable extended and appended to once per step,
through the same handle the whole run. Nothing in this suite had ever
called `H5Dset_extent()`, so it went unexercised even though the same
live-cross-step-handle pattern had already been fixed once for plain
writes (see the M9 capture-gap section above).

`H5VL_stream_dataset_specific()` was an unconditional passthrough straight
to `under_object` for any non-placeholder dataset. That is fine for a
dataset that has never been through a step -- but for one that has,
`under_object` is not "the dataset" in any current sense: `H5VL__stream_
replay_manifest()` rewires it to point at a step's own real `/step/<n>/`
object exactly once, the step that first materializes a placeholder, and
never moves it again (every later step captures its own separate
synthesized copy instead, via `H5VL__stream_step_create_index()`). So
calling `H5Dset_extent()` on the still-open handle in a later step reaches
straight through to the *first* step's own committed real object and
resizes it in place -- silently, since dataset-specific ops carry no
capture/replay path of their own. Measured directly: a dataset grown by 2
elements per step across 4 steps left `/step/0/`'s own copy holding the
**final** extent (8), not the extent (2) it had when step 0 committed --
retroactively rewriting history that had already landed on disk, discovered
by actually running the pattern rather than reading the capture code and
assuming dataset-specific ops were out of scope.

First fixed the same way every other unsafe case in this connector is
handled: decline rather than silently corrupt. `H5VL_stream_dataset_
specific()` refused whenever `H5VL__stream_dset_capture()` -- the exact
predicate `H5VL_stream_dataset_write()` already uses -- said this object's
write would be captured. The working alternative needed no new code:
re-create the dataset each step at its new size, exactly dev-plan.md
decision #2's model (successive versions of one named object, landing
group-based at `/step/<n>/`) applied to a shape that grows instead of
staying fixed.

**Built for real, 2026-08-15, after the ADIOS2 comparison above prompted
the question directly: why does vol-stream not have ADIOS2's own
`Variable::SetShape()`/`SetSelection()`, and can it?** It can, and the
fix is the same idiom `H5VL_stream_dataset_write()` already uses for
plain writes to a live cross-step object -- defer, don't touch the real
object -- extended to cover the dataset-specific op that write-capture
never reached. `H5VL_stream_dataset_specific()`'s `SET_EXTENT` handling
now records the requested new shape on the handle itself
(`H5VL_stream_t::pending_resize_space`, a new field) instead of declining
or touching `under_object`. Two more places had to agree with it, since
"a resize that only vol-stream's own bookkeeping knows about" is not a
real resize from the application's point of view:

- `H5VL_stream_dataset_get()`'s `GET_SPACE` case returns the pending
  override when one is set, so `H5Dget_space()` reflects the resize
  immediately -- matching plain HDF5's contract for `H5Dset_extent()`,
  and exactly the existing idiom this function already uses for a
  *placeholder* dataset (answering from the pending entry's own copy
  rather than a nonexistent real object), now extended to a *live*
  one with a pending resize.
- `H5VL__stream_step_create_index()` -- the function that synthesizes
  this step's own copy when a write captures a live object -- reads the
  pending override instead of querying the real (never-actually-resized)
  object's own space, so the new step's replayed copy is created at the
  new size, not the old one.

`test/t_dset_resize.c`'s case 1 is the new exit gate: `H5Dset_extent()` +
a full rewrite (`0..n-1`) every step, through one persistent handle,
round-trips correctly at every size, `H5Dget_space()` reflects each
resize immediately, and the earlier step stays exactly as it was --
verified non-vacuous by reverting the fix and confirming the same case
fails. The re-create-each-step alternative (case 2) still round-trips
too, unaffected.

**A second, narrower gap found by building this, not assumed away: a
resize followed by a PARTIAL write (only the new tail, not a full
rewrite) leaves the carried-forward portion at HDF5's own fill value.**
Each step's own synthesized copy is still created fresh
(`H5VL__stream_step_create_index()`'s existing design, unchanged) and
populated only from that step's own captured write -- a resize changes
*how large* the fresh copy is, not *what already has real data in it* --
at the time this was written, the connector did not copy the previous
step's actual values forward into the newly-grown region. This was
`test/t_dset_resize.c`'s case 3, deliberately named the boundary rather
than silently omitted: not corruption (a different, earlier step's own
data was never touched, and HDF5's own well-defined fill value was not
silently wrong data, merely absent), but not a complete self-sufficient
snapshot either. **Closed the same day -- see "Closed 2026-08-15" below**,
prompted by the performance investigation that followed the ADIOS2
comparison, not by this correctness gap alone; the two turned out to be
the same fix. Full rewrite each step (case 1) and re-create each step
(case 2) were already complete before this; the tail-only pattern (case
3) is now complete too.

**Closed 2026-08-15, prompted by "is it still slower, how can we improve
it" after the `.payload`-staging finding above.** Profiling had already
shown the writer's own O(N^2) total-bytes-per-run cost (every step
re-sending the whole cumulative array, not just what changed) as a real,
separate lever from `.payload` compression -- closing the gap above was
the way to actually use it, since a tail-only write was exactly what the
gap left incomplete.

`H5VL__stream_carry_forward_resized()` runs once per synthesized create,
after the main replay loop finishes (every `replay_under[]` object
exists, every entry's own write has already landed). It needed no new
bookkeeping: `H5VL__stream_path_index_resolve()`, already populated by
every writer-side replay for reference resolution, finds the previous
step's own committed copy for the same path directly. It reads that
copy's full values and writes them into the new (larger) object's
matching `[0, old_extent)` region -- the region this step's own write
does not cover.

**Zero-waste by construction, not by a special case for case 1.** The
gate is a single equality: this step's own written-element total must
equal exactly the growth (new extent minus old). A full rewrite writes
the *whole* new extent, not just the growth, so it fails this check and
is correctly skipped -- case 1 pays nothing for a copy it does not need,
verified by the unchanged, still-passing case 1 assertions rather than
assumed. Best-effort throughout, matching every other opportunistic path
in this connector: a failure to open the previous copy, a type/rank
mismatch (scoped to 1-D, matching every real resize use case), or an
allocation failure all just skip carry-forward for that path rather than
failing the step's replay -- the gap this closes was never a correctness
bug, so nothing here is allowed to become one. `test/t_dset_resize.c`'s
case 3 is now a real regression guard (it FAILs if the gap reopens, not
just reports which state it found) -- verified non-vacuous by disabling
the call and confirming it fails with exactly the old message.

**Measured at two scales to confirm the O(N) claim isn't just asserted.**
Same benchmark shape as `test/b_stream_grow.c`, `VOL_STREAM_STAGE_
PAYLOAD=0` (the finding above) on both variants for a fair comparison of
this lever specifically:

| Steps | Pattern | Total bytes | Writer commit, mean | Total wall time |
|---|---|---|---|---|
| 50 | Full rewrite (O(N^2)) | 39.84 MiB | ~0.87 ms | ~46 ms |
| 50 | Tail-only (O(N)) | 1.56 MiB | ~0.62 ms | ~31-33 ms |
| 200 | Full rewrite (O(N^2)) | 628.12 MiB | ~2.84 ms | ~642 ms |
| 200 | Tail-only (O(N)) | 6.25 MiB | ~1.72 ms | ~341 ms |

Total bytes drops exactly as the complexity classes predict (200-step
tail-only moves 6.25 MiB regardless of run length's square; full rewrite
moves the sum of every step's then-current size). **The relative
advantage in total wall time grows with run length** -- roughly 1.4x at
50 steps, roughly 1.9x at 200 -- exactly what O(N) vs. O(N^2) predicts:
the gap should widen, not stay fixed, as the stream runs longer. A short
benchmark understates the win; a long-running stream is where this
matters most.

**One metric that does NOT improve, and why it doesn't mean what it looks
like it means: "aggregate throughput" (total bytes / wall time) is LOWER
for the tail-only pattern** (18-55 MiB/s vs. 870-980 MiB/s for full
rewrite at these scales) -- because total bytes shrank by design (25x at
50 steps) while the FIXED per-step cost (Mercury/Margo/Argobots RPC and
progress-engine overhead, ordinary HDF5 metadata operations -- the ~34%
of self-time this section's own profiling found, independent of payload
size) did not shrink proportionally. Throughput answers "how fast does
data move once it's moving"; it is the wrong lens for a change whose
entire point is moving less data in the first place. Total wall time and
writer commit latency -- both of which improved -- are the metrics that
answer "is the whole run faster," which is what was actually asked.

**The genuinely fair O(N)-vs-O(N) ADIOS2 comparison, once the carry-forward
fix made it possible: the RELATIVE gap widens, not closes.**
`benchmark/adios2_compare/adios2_bench_tail.cpp` (ADIOS2's own
`SetSelection()`-to-the-new-region idiom) against `test/b_stream_grow_tail.c`
(the tail-only pattern this section just closed), both O(N), same 50-step
workload:

| Pattern | vol-stream total wall (staging off) | ADIOS2 total wall | Gap |
|---|---|---|---|
| Full rewrite, O(N^2) | ~46 ms | ~13 ms | ~3.6x |
| Tail-only, O(N) | ~30-33 ms | ~2.05 ms | ~15.5x |

vol-stream's own total wall time improved a real 1.4-1.5x moving to the
tail-only pattern -- the win this section measured is real. But ADIOS2's
improved roughly 6x over the same change, because its per-step cost scales
down with less data far more than vol-stream's does. Once payload-
proportional costs are minimized (the point of this whole section), what
remains on vol-stream's side is a largely FIXED per-step cost -- Mercury/
Margo/Argobots RPC and progress-engine overhead, real HDF5 metadata
operations for a genuine per-step object -- that does not shrink with a
smaller payload the way ADIOS2's lighter-weight marshal-and-ship does. So a
smaller payload makes vol-stream's fixed cost *relatively* more dominant,
not less, which is exactly why the ratio moves the wrong way even though
the absolute numbers on vol-stream's own side improved. Closing that gap is
a different, harder problem than this section's O(N^2)-to-O(N) fix -- the
two options this session identified but did not build (making the
subscriber push asynchronous rather than blocking on `margo_forward_timed()`;
Margo/Argobots progress-engine tuning) are what it would take, and both need
their own investigation before assuming either actually helps. An earlier
version of `benchmark/adios2_compare/README.md` claimed `adios2_bench.cpp`
already used the tail-only idiom -- it did not (it writes a full rewrite
every step, same as `test/b_stream_grow.c`); that was a real documentation
error, corrected here and in `adios2_bench_tail.cpp`'s introduction.

#### The asynchronous subscriber push — shipped 2026-08-16, and it is worth ~6%, not 15x

The first of those two options is built. Measured before assuming, which
turned out to matter: **the blocking push was never big enough to explain
the ADIOS2 gap.** Instrumenting the forward directly (`VOL_STREAM_PUSH_STATS=1`,
reported by `vs_tr_stop()`) put it at 3.1–3.9 ms of a ~48 ms
`b_stream_grow_tail.c` run and 24–27 ms of a ~305 ms `b_stream_grow.c` run
— **7–9% of the writer's own time**, so even eliminating it entirely could
not have closed a 15.5x ratio. Anyone reading the paragraph above as "async
push is one of the two things that would close the gap" should read this
instead: it is a real but modest win, and the gap remains a fixed-per-step-
cost problem.

What made it worth doing anyway is that the same instrumentation showed the
cost was almost perfectly *hideable*. The writer work between a step's last
push and its `step_ready` broadcast — the rest of replay, discarding the
pending buffer, resolving the step's deferred-request completion — measured
~28 ms and ~237 ms against pushes costing ~3 ms and ~25 ms. An order of
magnitude more room than needed.

So each push is now issued with `margo_iforward_timed()` and parked on a
per-transport in-flight list (`vs_tr_inflight_t`), and the whole set is
settled once by `vs_tr_drain_pushes()` at the top of
`vs_tr_writer_broadcast_step_ready()` — before the step is announced.
Draining *there* rather than later is what keeps this a pure latency
overlap and not a change in what a subscriber observes: a reader woken by
`step_ready` still finds the step's data already queued, and pushes cannot
reorder across a step boundary because step N's are all complete before
step N+1's are issued. `vs_tr_stop()` drains too, for a writer that pushed
and closed without ending the step.

**No payload copy was needed, and that was verified rather than assumed.**
The design depends on Mercury serializing the input during
`margo_iforward()` rather than at wait time, because `H5Fend_step()` frees
the replay payload buffers between issuing these pushes and draining them —
a deferred read would send freed memory, which is silent corruption rather
than a crash. Confirmed directly with a standalone probe: forward a 1 MiB
payload (past the eager path, so the bulk/extra-buffer route is exercised
too), overwrite the source the instant `iforward()` returns, and the
receiver still sees every original byte.

Measured A/B, same binary, the only difference a one-line probe restoring
the inline wait (so build-to-build variance cannot explain it):

| | inline wait | async | |
|---|---|---|---|
| `b_stream_grow_tail.c`, total wall (median of 7) | 42.65 ms | 40.10 ms | **6.0% faster** |
| `b_stream_grow.c`, throughput (median of 5) | 146.64 MiB/s | 155.68 MiB/s | **6.2% faster** |
| time blocked in the push path, tail | 2.8–3.9 ms | 0.69–0.87 ms | ~4x less |
| time blocked in the push path, full | 24–26 ms | 4.4–4.7 ms | ~5.5x less |

The residual blocked time is issue cost — `margo_create()` plus the input
proc's copy of the payload into Mercury's own buffer — which is real and
does not go away.

**The fan-out win predicted for this change did not materialize, at least
not on this transport.** The reasoning was that the writer issues one push
per (subscriber, selection run, predicate run, chunk), so waiting inline
costs the *sum* of those round trips while draining once costs roughly the
slowest — a per-subscriber term that should collapse from a network round
trip to an issue cost. `test/b_push_fanout.c` was written to measure
exactly that (1/2/4/8 subscribers, each its own process, each verifying
every value it receives). It does not show it: the per-subscriber slope is
~0.08 ms inline and ~0.077 ms async — no meaningful difference. The reason
is visible in the same stats: on loopback `ofi+tcp` a push round trip is
only ~12–24 us here, so there is almost nothing per subscriber to overlap.
The prediction may still hold on a real fabric with real round trips; it is
not demonstrated, and should not be claimed until it is.

#### The push loop indexed a mutable rank space — found by `b_push_fanout`, fixed

Writing `test/b_push_fanout.c` surfaced a real data-loss defect that
predates the async push and is independent of it. At 4 and 8 subscribers
the writer issued **one fewer data push than it had (subscriber, step)
pairs** — 238 of an expected 240 — and the subscribers that lost one
blocked until their `H5Fget_subscribed_data()` timeout, having received
every earlier step correctly.

Worth recording how it was found, because the first two hypotheses were
both wrong and both were killed by measurement rather than argument. Not a
forward timeout: 239 pushes totalled ~11 ms, so nothing waited anywhere
near the 1 s bound. Not a stale group or subscription view either:
instrumenting the loop showed `group_size=9` with all 8 matching
subscriptions present at every single step. The prime suspect at that point
was `ssg_get_group_member_addr()`, since this same file already documents
that call as transiently failing and already retries it on the reader side
— a tidy story that turned out to be the wrong call entirely.

Instrumenting every `continue` in the loop named it exactly:
`ssg_get_group_member_id_from_rank()` failing for **ranks 7 and 8 at the
final step**, reproducibly, 3 runs out of 3. That is a
time-of-check/time-of-use bug on the rank space. The loop read
`ssg_get_group_size()` once and then walked ranks 0..N-1 doing blocking
network I/O in the body; a subscriber that receives its last push and calls
`H5Fclose()` leaves the SSG group *while the loop is still running*, the
group shrinks, and the top ranks stop existing. SSG was answering honestly
the whole time — asking a mutable rank space for a stable identity, across
blocking calls, was the mistake. **Not a third-party bug**, which is worth
stating plainly given how close the earlier `ssg_get_group_member_addr()`
theory came to being filed as one.

Fixed by iterating the subscription snapshot itself rather than the group's
ranks. The snapshot already holds exactly the members subscribed to this
path, and a `ssg_member_id_t` stays valid whether or not that member is
still present — `ssg_get_group_member_addr()` then reports a departure per
member, without touching anyone else's push. This also deletes the
`O(group_size * n_snapshot)` match loop, since the subscribers to one path
are typically far fewer than the group's members.

Verified the way a regression fix should be: the bug reproduced **3/3**
before and **0/4** after, on the same harness, using the inline-wait
variant where it fires most reliably. `b_push_fanout` is now registered as
a ctest (every subscriber verifies every value it receives, so it guards
this directly), and the suite is 43/43 across three consecutive runs.

**The whole class, not just the instance.** The push loop was one of *five*
rank-indexed loops in `tr_mercury.c`, and four of them had the identical
shape — read `ssg_get_group_size()` once, walk ranks 0..N-1, block on an
RPC in the body:

- `vs_tr_writer_broadcast_step_ready()` — same severity as the push bug. A
  reader closing mid-broadcast would cost *other* readers their step_ready
  notification, leaving them blocked in `H5Fwait_step_ready()`.
- `vs_tr_reader_get_current_step()`, `vs_tr_reader_ack_step()`'s fallback
  probe, and `vs_send_subscribe()`'s probe — all three hunt for the writer
  by asking every member in turn. Skipping the tail of the list can skip
  the writer itself, so a subscribe or ack fails for no visible reason.

All four now go through one helper, `vs_tr_snapshot_members()`, which
captures the member IDs in a single tight pass with no blocking work in it
and hands back a stable array. Rank indexing survives in exactly one place
— inside that helper, where the window it opens is empty.

The fifth, `vs_tr_writer_min_acked_step()`, was already safe and is left
alone: it walks `lag_table`, which is keyed by `ssg_member_id_t` already,
and its `ssg_get_group_member_rank()` call is only a liveness predicate
whose rank value is discarded. It did carry a `group_size` variable it
never used, removed here because it read like rank logic that was not
there.

Only the push path has a reproducer today — a departure has to land inside
a specific loop to matter, and `b_push_fanout` is the only test with enough
subscribers leaving at once to make that likely. The other three are fixed
by construction rather than by demonstration, which is worth stating
plainly rather than implying all four were caught red-handed.

**Benchmarked 2026-08-15: the growing time-series array, streamed for
real.** ADIOS2 has no published benchmark for this exact case either --
its own `testing/adios2/performance/` suite covers many-variable overhead
and metadata composition/installation/traversal cost, neither of which is
a resizable array -- so `test/b_stream_grow.c` reports vol-stream's own
numbers rather than a head-to-head. Same two-process shape as
`test/t_transport.c` (a real writer and reader in separate OS processes
over Mercury), `/series` re-created and rewritten in full each step,
growing from 8192 to 409600 elements (32 KiB to 1.6 MiB) across 50 steps.

Two things worth stating plainly rather than leaving to be inferred from
one aggregate number:

- **The end-to-end read path could not use `H5Fwait_step_ready()` +
  `H5Fbegin_step()` + `H5Dopen2()`** -- `test/t_manypaths.c`'s pattern,
  which works for a *finished* stream. `H5Fbegin_step()`'s reader-index
  build is a one-time scan of the underlying file's own group listing
  (`H5VL__stream_reader_build_index()`), the same class of native metadata
  read that leaves `h5ls`/`h5dump` reporting `**NOT FOUND**` against a live
  writer (this document's own "existing tools on a live stream" finding) --
  a second process's file handle simply does not see it while the writer
  still holds the file open. The benchmark's reader uses
  `H5Fsubscribe()`/`H5Fget_subscribed_data()` instead, which rides the same
  Mercury RPC round trip `H5Fwait_step_ready()` uses and never touches the
  underlying file's metadata at all -- confirming M8's subscription
  protocol is not just the differentiator (design-plan.md's ranking) but
  the *only* mechanism this connector has for a second process to observe
  live data, notification aside.
- **The subscriber routinely has the data before the writer's own
  `H5Fend_step()` call returns.** Measured 50/50 steps, mean 4.8ms early:
  the push is issued mid-replay, as pending entries are processed, not
  after the step finishes committing. Reporting "end-to-end latency" against
  `H5Fend_step()`'s return would have produced a negative number purely from
  measuring against the wrong reference point -- the benchmark instead times
  from `H5Fbegin_step()`, which is always well-defined.

Measured (`ofi+tcp`; `na+sm`'s zero-copy path needs `process_vm_readv()`
cross-memory attach, which this machine's `kernel.yama.ptrace_scope`
denies for a transfer this size even between `fork()`-related processes --
every other test here only ever moves a scalar, small enough to stay on
`na+sm`'s inline path and never trip this):

- start-to-receipt latency (`H5Fbegin_step()` to the subscriber's decoded
  data in hand): 0.2-2.0ms across the run, growing with the array itself.
- writer-side commit latency (`H5Fbegin_step()`..`H5Fend_step()` return):
  0.8ms at the first, smallest step to 9-12ms at the last, largest one --
  linear in the step's own size, as expected for a full re-encode and
  replay of the whole cumulative array.
- aggregate throughput over the whole run: ~145 MiB/s.
- **total bytes moved across the run (39.8 MiB) versus the final step
  alone (1.6 MiB, 3.9% of the total)**: the O(N^2) cost, made concrete, of
  the self-sufficient-snapshot convention re-creating each step gives you
  for free -- every step resends everything written so far, not just what
  changed since the last one. A tail-only-append variant (write just the
  new slice, accept that a single step's own copy is incomplete on its
  own) would trade that for O(N) total at the cost of the "state of group
  G as of step N" forward-fill already documented above -- not built here,
  since the point was to measure the supported pattern honestly, not to
  benchmark an alternative nobody asked for yet.

**A real, one-to-one comparison against ADIOS2 SST, same workload
(2026-08-15).** `benchmark/adios2_compare/` -- kept for reproducibility,
not part of this project's own build or CI, since ADIOS2 is not a
dependency here. Built ADIOS2 2.10.2 from Spack (`+mpi +sst`, every
unrelated engine/compressor off) specifically to answer this with real
numbers rather than the design-plan.md-level argument alone. Same
workload as the benchmark above, expressed idiomatically on each side
rather than forced through one shared code path: ADIOS2 has a real
mechanism vol-stream does not (`Variable::SetShape()`/`SetSelection()`
before each `Put()`, no redefinition needed), so it is used, not
vol-stream's forced recreate-each-step workaround. Same 50 steps, same
409600-element/1.6 MiB final size, same total bytes moved (39.84 MiB) --
the only thing that differs is which engine is doing the moving.

| Metric | vol-stream (`ofi+tcp`) | ADIOS2 SST |
|---|---|---|
| Total bytes streamed (50 steps) | 39.84 MiB | 39.84 MiB (identical workload) |
| Writer commit latency, mean | ~5.5-7 ms | ~0.2 ms |
| Writer commit latency, max | up to 87 ms (outlier observed) | ~0.5 ms |
| Start-to-receipt latency, mean | ~0.7-0.85 ms | ~0.6-2.7 ms (noisier run to run) |
| Aggregate throughput | ~145-148 MiB/s | ~2800-3500 MiB/s |

**The honest reading.** ADIOS2's writer-side commit is roughly 25-35x
faster, and aggregate throughput roughly 19-24x higher. This is not a bug
in vol-stream's benchmark or a tuning gap -- it is design-plan.md's own
opening argument ("matching ADIOS2 is the floor, not the goal"), now with
a number attached rather than left as a qualitative claim. SST's commit is
cheap because it marshals a buffer and ships it; vol-stream's is layered
on the full HDF5 data model faithfully -- `H5Tencode`/`H5Sencode2`/
`H5Pencode2` re-run for a fresh `DsetCreate` every step (the recreate
pattern this section's growing-array case forces), then a real
`H5Dwrite()` into the underlying file via replay, not just a memory
hand-off. That is the cost of what design-plan.md's differentiators are
built on top of (full data model, VOL-level reuse, no new wire format for
the parts HDF5 already encodes) -- worth stating in numbers, not just
asserting the trade-off exists.

**One metric did not go the way the aggregate numbers would predict.**
Start-to-receipt latency -- from the writer beginning its step to the
*other* process having decoded data in hand -- is roughly the same order
of magnitude on both sides, and vol-stream's mean is if anything slightly
lower across repeated runs. Plausible mechanism, not yet confirmed
further: vol-stream's subscription push is issued mid-replay (measured
above, 50/50 steps, ~4.8ms before the writer's own `H5Fend_step()`
returns), so a subscriber can have the bytes before the *writer's* slower
per-step bookkeeping (the manifest/replay cost the aggregate numbers
above are dominated by) finishes -- the aggregate throughput gap is real
and large, but it does not mean every dimension of "how fast does the
other side see it" moves in the same direction.

**What this does not claim.** No RDMA hardware exists on this machine, so
both sides are measured on plain sockets/TCP, not the RDMA path either
engine is actually designed for -- a facility with real RDMA could show a
different gap. This is one workload (single writer, single reader, no
M×N, no subscription narrowing, no predicate) -- several of design-plan.md's
differentiators (subscription-based partial marshaling, predicate
pushdown, per-subscriber precision) have no ADIOS2 equivalent to run this
same head-to-head measurement against at all. `benchmark/adios2_compare/
README.md` has the full build/run instructions, including a real GCC-15
build break in ADIOS2 2.10.2's bundled `thirdparty/yaml-cpp` (missing
`<cstdint>`, worked around with `-include cstdint` rather than patching
vendored source) and the `ADIOS2_USE_MPI` application-side opt-in macro
that a confusing compiler error gives no hint about needing.

**Found the dominant cost, 2026-08-15: it is not the manifest encoding this
section originally suspected -- it is `.payload` staging's compression,
and there is already a lever for it.** Asked directly whether the gap
above could be closed, so profiled the writer with `perf record` rather
than guessing further. The single largest cost by far was zlib
`deflate()` (via `H5Z__filter_deflate()` from `H5D__chunk_flush_entry()`)
-- not `H5Tencode`/`H5Sencode2`/`H5Pencode2`, which barely register.
`H5VL__stream_stage_payload()`'s own DCPL always chunks `/step/<n>/
.payload` as one chunk spanning the *whole* buffer, so for this
growing-array workload every step deflates an ever-larger payload from
scratch -- up to 1.6 MiB at the final step -- even at the already-fastest
level (1) that DCPL requests.

`VOL_STREAM_STAGE_PAYLOAD=0` (an existing, already-documented,
already-tested knob -- see `H5VL__stream_stage_payload()`'s own comment,
now updated with these numbers) turns this off entirely. Measured on the
identical benchmark:

| Metric | Staging ON (default) | Staging OFF (`VOL_STREAM_STAGE_PAYLOAD=0`) | ADIOS2 SST |
|---|---|---|---|
| Writer commit latency, mean | ~5.7-7.3 ms | ~0.87-0.89 ms | ~0.2 ms |
| Aggregate throughput | ~140-145 MiB/s | ~865-880 MiB/s | ~2800-3500 MiB/s |

**Roughly 6-8x faster, closing most of the gap this section measured** --
from ~20x slower than ADIOS2 to roughly 3-4x. Re-profiling with staging
off shows no single dominant cost left: what remains is Mercury's own RPC
byte-copying (`hg_proc_bytes` inside `vs_tr_writer_push_data`'s forward
call) and ordinary HDF5 metadata operations for the real replayed
object (property-list teardown, group traversal, the dataset create/write
itself) -- ordinary per-step costs of this architecture rather than one
avoidable hotspot, and a much harder floor to lower further.

**Why this wasn't the obvious first guess:** `.payload` was built and
measured for its **file-size** cost (dev-plan.md's own earlier write-up:
"a stream file is a little over twice the size of the data in it"), and
deflating it was framed as the fix *for that*. Nothing before this
session had measured its CPU cost against a workload where the payload
itself grows every step -- every existing test uses small, fixed-size
data, where deflate's absolute cost is negligible regardless of level.
The lever already existed; what was missing was measuring it against the
right workload to notice it was the dominant cost of a different kind.

**Still left ON by default, deliberately -- this finding does not change
that.** `H5VL__stream_stage_payload()`'s own long-standing rule holds: a
knob this consequential does not get its default flipped silently.
Recommendation for a throughput-sensitive deployment: measure the actual
workload with `VOL_STREAM_STAGE_PAYLOAD=0` before assuming the default is
the right choice -- for a growing array, or any workload whose payload
size is not small and fixed, it is very unlikely to be.

**VL and reference support (Decision #4) were never implemented, not just
deferred.** The residual-risk plan here was "ship them behind a property
defaulting to off" if M2 slipped — but no such property exists, and neither
`H5Rget_obj_name`/`H5Rcreate_object` (reference name translation) nor
`H5Dvlen_get_buf_size`/`H5Treclaim`-based deep serialization exist anywhere in
`src/`. The capture path is datatype-agnostic: it `memcpy`s whatever buffer
`H5Dwrite`/`H5Awrite` was given straight into the pending entry. For a
variable-length datatype, that buffer is an array of `{len, pointer}`
(`hvl_t`) structs, and the pointers are only valid in the writer's own
process memory — capturing them verbatim and replaying them later (a
different `H5Dwrite` call, against a different handle, however much later
`end_step()` runs) reads through stale pointers rather than the data itself.
This was a live correctness gap, not just a latency or cost concern: writing
VL data through this connector was unsafe, and nothing detected or rejected
it. **Update:** option (b) has landed —
`H5VL__stream_type_unsafe_to_capture()` rejects a `DsetWrite`/`Attr` capture
outright (return -1, clear HDF5 error) for `H5T_VLEN`, `H5T_REFERENCE`, and
variable-length-string types, checked recursively through `H5T_COMPOUND`/
`H5T_ARRAY` members so a struct or array *containing* one of these is caught
too. A rejected write does not poison the rest of the step — a plain write
to a different object in the same step still replays correctly (verified in
`test/t_vl_reject.c`, the exit gate for this fix). Real deep-serialization
(a) is still not built; this closes the silent-corruption failure mode, not
the missing feature. **Update 2026-08-14: (a) has now landed too** — see the
VL deep-serialization paragraph below. Reference translation, once built, should happen at
write-time (intercepting `H5Rcreate_object`/`H5Rcreate_region`) rather than
by walking the whole step's references synchronously inside `COMMITTING` —
the latter would stall the collective barrier in proportion to reference
count.

**VL deep-serialization landed 2026-08-14 — the last piece of Decision #4.**
A VL buffer is not self-contained (`H5T_VLEN` is an array of `hvl_t`
{len, pointer}; a VL string an array of `char *`), and this connector defers
the real write to `end_step()`, by which point the application is entitled to
have reclaimed all of it. So capture now follows those pointers immediately
and copies what they point at (`H5VL__stream_vl_serialize()`), and replay
rebuilds real pointers into fresh allocations before writing
(`H5VL__stream_vl_deserialize()`, released right after the write since HDF5
has copied by then). The wire form is, per element, `[uint64 tag][tag-1
bytes]`, where `tag == 0` marks a NULL pointer — the +1 bias is what keeps
NULL distinguishable from a legitimately empty sequence or `""`, which HDF5
round-trips differently.

Wired on both the dataset and attribute capture/replay paths. `test/t_vl_
roundtrip.c` covers ragged lengths (0, 1, many) and the empty string, checks
what actually landed through the *native* connector, and — the load-bearing
case — frees every application buffer **before** `end_step()` runs, so a
regression back to capturing pointers rather than bytes would read freed
memory and fail there.

Still rejected, deliberately: a VL *nested* inside a compound or array, and
a VL whose base type is itself variable-length. Deep-serializing those means
walking a nested layout member by member, which this does not do.
`test/t_vl_reject.c` was narrowed to assert exactly that remaining boundary
(it previously asserted the interim "reject all VL" behavior).

**Measured 2026-08-14 — reference support is feasible, but Decision #4
understates what it takes.** Three probe programs run against the real
connector (not code reading) establish:

1. `H5Rcreate_object(fid, "/target", …)` for a target created in the **same
   step** fails outright — `object 'target' doesn't exist`. Deferred writes
   mean the target is genuinely not in the underlying file until
   `end_step()` replays the manifest.
2. The same call for a target in a **prior, committed** step also fails, for
   a different reason: steps land group-based at `/step/<n>/`, so the
   application's logical `/target` is physically `/step/0/target` (confirmed
   with `h5dump -n`).
3. The same call given the **physical** path works, and `H5Rget_obj_name()`
   returns `/step/0/target` — so the HDF5 mechanism this decision rests on
   is sound.

The consequence: name translation is the easy half. `H5VL_stream_object_
specific()` is currently a pure passthrough, so making the natural call
`H5Rcreate_object(fid, "/target")` work needs a logical→physical path
translation layer in the object-lookup path; capture would then store the
logical path (step prefix stripped) so a reader rebuilds against its own
layout. And the same-step case is a real architectural limitation, not an
oversight — it needs either a documented "targets must come from an earlier
step" rule, or deferred reference resolution during `COMMITTING`, after
objects materialize but before the manifest closes.

**Status: landed.** `H5VL__stream_resolve_physical_path()`, writer-side
`path_index` population, and the `H5VL_OBJECT_LOOKUP` translation branch
make `H5Rcreate_object(fid, "/target")` work with the logical path
(`test/t_ref_path.c`, which also asserts that a same-step target is still
refused — the documented "earlier step" rule above). Capturing a dataset
*of* references and replaying it now works too (`test/t_ref_roundtrip.c`):
`H5VL__stream_type_unsafe_to_capture()` no longer rejects a *top-level*
reference, and an ordinary memcpy — the same path every other fixed-size
type already uses — carries it through capture and replay correctly.

Two capture designs were tried and abandoned before landing on the one that
works, and what each ruled out is worth recording, because the eventual fix
is the opposite of what the danger looked like it required.

*Attempt 1 — translate at capture with `H5Rget_obj_name()`.* Crashes
(`H5G_rootof: Assertion 'f->shared' failed`). Resolving a reference goes
through the `hid_t` an `H5R_ref_t` caches internally, which is the
application's own file id — i.e. this connector — so the call re-enters the
library from inside a VOL callback. Isolated with probes: the identical call
at application level, followed by a create and an `end_step` replay, is
fine; only the call from within a callback breaks.

*Attempt 2 — cache token → logical path, no HDF5 call at capture.* The
connector already sees the target's logical path at `H5Rcreate_object()`
time, as the name arriving in the `H5VL_OBJECT_LOOKUP` branch, and the token
that lookup returns is what ends up inside the reference (an `H5R_ref_t`
begins with its token, for every reference flavour). Capture becomes a pure
table lookup, with no re-entrant HDF5 call anywhere. The manifest payload
came out holding the right logical names, and nothing else regressed — but
`end_step()` still crashed at the same assertion, now at the very first
`ensure_group()` of the replay, with the *native* file object already
carrying a NULL `shared`. So the fault was never the re-entrancy attempt 1
found; it was something about translation itself.

*Attempt 2's crash, root-caused afterwards (M8.5.1 turned it up).* Its replay
half called `H5VLwrap_register(file_under_object, H5I_FILE)` to get an
`hid_t` for `H5Rcreate_object()`, then `H5Idec_ref()` to release it. That id
**owns** the object it wraps, so releasing it closed the underlying file
itself — which is exactly the `f->shared == NULL` seen at the next
`ensure_group()`. Nothing to do with translation, or with a reference
entry's payload being shorter than `n_elem × H5Tget_size()`. Worth stating
plainly because the same trap bit the M8.5.1 fast path independently: to
reach an under-VOL object from inside a callback, do not take an `hid_t` at
all — use the connector's own optional operations, which take the object
directly.

*What actually unblocked it: don't translate at all.* A raw memcpy diagnostic
— accept the write, copy the bytes verbatim, exactly like any other
fixed-size type — round-tripped correctly on the first try. The "unsafe"
premise from the original guard doesn't hold for the only case reachable
through this connector's own API: `H5Rcreate_object()` builds a reference
relative to its `loc_id`'s own file, so an application cannot name a
genuinely foreign file without a `loc_id` in *that* file — which would not
route through this connector at all. For a same-file reference
(`H5R_OBJECT2`/`H5R_DATASET_REGION2`/`H5R_ATTR`, confirmed via
`H5Rget_type()`), the cached filename pointer inside the private
`H5R_ref_priv_obj_t` is NULL — read directly at the byte level against
HDF5's own (private, `H5Rpkg.h`) layout. Nothing to dangle, so memcpy is
exactly as safe as for any other scalar. `H5VL__stream_type_unsafe_to_
capture()` now takes a `top_level` flag: a reference at the top level of a
write is exempt; one nested inside a compound or array still is not, since
translating that would mean rewriting the buffer member by member, which
nothing here does — `test/t_ref_roundtrip.c` asserts that boundary
explicitly, not just the positive case.

**The dependency risk is quality, not quantity.** Criteria before adding
anything: actively maintained, deployed at target facilities, Spack-installable,
bounded scope, stays out of the wire protocol. Mercury, Argobots, Margo and
flatcc pass all five. What needs enforcing is a pinned Spack environment checked
in from M0.

**Plugin signatures.** An HDF5 built with `-DHDF5_REQUIRE_SIGNED_PLUGINS=ON`
refuses to load an unsigned connector, with an error that reads like corruption
rather than a policy refusal. This affects the plugin path only — and the M0 exit
gate uses the plugin path. See [`plugin-signing.md`](plugin-signing.md).
