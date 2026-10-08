# Streaming in HDF5: beating ADIOS2, not adopting it

Design research behind `vol-stream`. A typeset version with diagrams is in
[`design-plan.pdf`](design-plan.pdf); both date from 2026-08-09, before any of
the implementation. For what was built, see [`user-guide.md`](user-guide.md).

HDF5 has five partial answers to streaming and no complete one. The gap is not
transport — it is that **HDF5 has no step**. Matching ADIOS2 is the floor, not
the goal, and the two constraints turn out to be one decision: you cannot beat
SST while running on top of SST.

## Recommendation

Build streaming as a **VOL connector owning its own wire protocol**, with a thin
internal transport abstraction. Add the step as registered *optional operations*
rather than new core API. Make the protocol **reader-driven** — the thing ADIOS2
structurally cannot do, and the reason the protocol has to be ours.

Two pieces a streaming engine needs are already in the VOL layer:
`H5VL_request_class_t` provides `wait`/`notify`/`cancel` against `H5ES` event
sets, and the VOL `read`/`write` callbacks already take a `count` array of
datasets. That is ADIOS2's deferred `Put` and its `EndStep` batch, unused. And
the hardest part of M×N redistribution is in the tree and battle-tested by VDS:
`H5Sselect_project_intersection` computes exactly the writer-selection to
reader-selection projection redistribution needs.

## Why not depend on ADIOS2

SST is mature and solves real problems: RDMA, UCX, MPI, Mercury and WAN
transport, rendezvous, queue policy, M×N redistribution, step atomicity. On a
pure build-versus-borrow reading, it wins.

It stops winning the moment the goal is to be *better* than ADIOS2 rather than
equal to it. Every differentiator below needs something SST's protocol has no
room for — a back-channel from reader to writer, negotiated precision per
subscriber, a step cadence that is not globally monotone. You cannot add a
reverse channel to a protocol you do not own. The dependency is ruled out on
technical grounds before the organizational ones are reached.

**This is an argument about one category of dependency, not about dependencies.**
Nobody should write their own RDMA, their own user-level threading, or their own
lossy compressor. What makes ADIOS2 different is not that it is a dependency but
that it is a *peer framework which owns the protocol*.

## Where ADIOS2 is genuinely beatable

Ranked by differentiation × how much HDF5 already has.

### 1. Reader-driven subscription — new protocol

An ADIOS2 SST writer marshals everything it puts in a step and queues it until
readers pull the parts their selections need. SST thereby avoids delivering all
data to every reader, but the writer learns what is wanted only after it has
prepared everything, and a reader cannot change the representation it gets.
`StepDistributionMode=OnDemand` chooses *which* reader gets a step; it cannot
change what the step contains. (For a plain box selection SST's bytes on the
wire are comparable to a subscription's; the wire-byte gain is over streams that
deliver whole frames. Corrected 2026-10-08: an earlier version said SST ships
everything and readers discard the rest.)

Let readers declare interest and have writers marshal only what someone
subscribed to. This cuts the writer's serialization work (and, against
whole-frame streams, wire bytes), and makes per-subscriber precision natural: full fidelity to the checkpoint sink,
downsampled to the live viz, from one `end_step()`.

### 2. Stream and archive as one object — onion VFD precedent

ADIOS2 makes you choose: BP5 to a file, or SST as a stream, different engines and
code paths. The onion VFD already models an append-only numbered revision
history with `H5FD_ONION_FAPL_INFO_REVISION_ID_LATEST`. A step that is
simultaneously a live stream element and a nameable, re-openable revision
collapses the choice.

### 3. Full data model and free type conversion — `H5T` already does it

ADIOS2 has Variables and Attributes over a flat `/`-delimited namespace, a fixed
type set, and largely assumes writer and reader agree on representation. HDF5
streaming inherits real groups, compound and enum types, opaque types, references
and dimension scales. More usefully, `H5T`'s conversion engine already handles
endianness and precision differences between peers, so a heterogeneous stream
works without the application knowing.

### 4. Independent step cadence per group — semantics work

An ADIOS2 step is global and all-variables-together. A mesh written once every
hundred field steps forces re-`Put`ing it or abusing `FirstTimestepPrecious`.
Allow per-group cadences with an explicit dependency: field step *N* references
mesh revision *M*.

### 5. Spill: a third queue-full policy — Cache VOL precedent

ADIOS2 offers exactly two answers when the queue fills: `Block`, which stalls the
simulation, or `Discard`, which loses science. Both are bad, and the choice is
forced because the queue lives in memory. Node-local NVMe makes a third answer
available: **spill** and keep accepting steps. Every exascale machine has the
hardware and the Cache VOL already stages HDF5 data on that tier.

### 6. Predicate pushdown — format extension

A reader subscribing with a predicate, so filtering happens before bytes are
marshalled. Flagged honestly as the expensive one: `H5D_chunk_rec_t` carries
`scaled`, `nbytes`, `filter_mask` and `chunk_addr` and **no statistics**, and
there is no `H5Q`/`H5X` query API in the tree. This needs per-chunk stats added
to the format. Roadmap, not v1.

The first four need no new file format and no new dependency. That is what makes
this a plan rather than an aspiration.

## What HDF5 already has

| Mechanism | Gives you | Where it stops |
|---|---|---|
| Classic SWMR | One writer, many readers, no locking; `H5Dflush`/`H5Drefresh`, `h5watch` | Needs POSIX write ordering — not NFS/SMB. No structural change in SWMR mode. No VL data. Readers poll; no step atomicity |
| VFD SWMR | Tick-based metadata snapshots via a shadow file; writers may create/delete objects | Shared-filesystem design, no transport. VL still incompatible; Windows unsupported. No M×N, no back-pressure |
| Mirror VFD | Real network streaming today over TCP; advertises `H5FD_FEAT_SUPPORTS_SWMR_IO` | Replicates byte ranges, not data. The far end materializes a random-access file — why it works and why it cannot become a stream |
| Onion VFD | Append-only numbered revision history | File-local, page-diff granularity, no transport or notification |
| `H5Sselect_project_intersection` | Projects one selection through another — the M×N mapping, exercised by VDS | Needs wiring into a step manifest. The algorithmic core is done |
| Async VOL + `H5ES` | Deferred-operation machinery, already shipped. Plus multi-dataset VOL callbacks | No "complete everything before this point" barrier — but a connector owns its own requests, so it does not need one |

## Why this cannot be a VFD

HDF5 shipped a stream VFD once. `H5FDstream.c`, `H5Pset_fapl_stream` and
`H5FD_STREAM` were removed in the 1.8 series, and `H5Fmodule.h` still carries the
note.

Below the VOL, the write has been flattened into byte ranges, and the layers
above keep returning to offsets they wrote earlier — the chunk index rebalances,
the free-space manager updates, the superblock and object headers are patched,
and the metadata cache reads its own writes back. Every one is a seek into data a
stream has already sent. The mirror VFD is the proof: it only functions because
the far end reconstitutes a fully seekable file.

Two further reasons. M×N needs dataspace knowledge to decide which writer rank's
data satisfies which reader's selection, and a driver sees no dataspaces at all.
And back-pressure must be applied where the application's step boundary is
visible, so a blocked queue stalls the simulation at a defined point rather than
inside an arbitrary metadata flush.

## What to borrow

The Mochi stack covers most of it, and Mercury was developed jointly by Argonne
*and The HDF Group*.

| Job | Borrowed from | Instead of |
|---|---|---|
| datatype encoding | `H5Tencode` | a type description format |
| space + selection encoding | `H5Sencode2` | a hyperslab wire format |
| chunking + filter description | `H5Pencode2` | a layout descriptor |
| M×N projection | `H5Sselect_project_intersection` | redistribution algebra |
| manifest framing + evolution | flatcc (FlatBuffers in C) | a hand-rolled envelope |
| transport + RDMA | Mercury | socket and verbs plumbing |
| progress engine, threading | Argobots + Margo | a hand-written progress thread |
| rendezvous, membership | SSG, replaced by mochi-flock after SSG's deprecation | contact files and a join protocol |
| queue spill to node-local | BAKE | an on-disk spill format |
| precision / compression | ZFP · SZ via `H5PL_TYPE_FILTER` | a lossy codec |
| conformance testing | HDF5 `test/API` | a data-model test suite |
| build + deployment | Spack (`mochi-margo` is builtin) | per-facility build instructions |

The dependency risk is quality, not quantity. Criteria before adding anything:
actively maintained, already deployed at target facilities, Spack-installable,
bounded scope, and stays out of the wire protocol.

## Sources

- [ADIOS2 — SST engine parameters](https://github.com/ornladios/ADIOS2/blob/master/docs/user_guide/source/engines/sst.rst)
- [ADIOS2 — supported engines](https://adios2.readthedocs.io/en/latest/engines/engines.html)
- [ADIOS2 — HDF5 API support through VOL](https://adios2.readthedocs.io/en/latest/ecosystem/h5vol.html)
- [Eisenhauer et al. — Streaming Data in HPC Workflows Using ADIOS](https://arxiv.org/pdf/2410.00178)
- [Mercury](https://mercury-hpc.github.io/) · [Mochi](https://mochi.readthedocs.io/) · [flatcc](https://github.com/dvidelabs/flatcc)
- [HDF5 SWMR User's Guide](https://support.hdfgroup.org/releases/hdf5/documentation/features/SWMR/HDF5_SWMR_Users_Guide.pdf)
- [VFD SWMR User's Guide](https://github.com/LifeboatLLC/hdf5_swmr/blob/feature/vfd_swmr/doc/vfd-swmr-user-guide.md)
- [Async I/O VOL](https://hdf5-vol-async.readthedocs.io/en/latest/) · [Cache VOL](https://github.com/HDFGroup/vol-cache)
