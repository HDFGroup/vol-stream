foreach(t t_smoke t_step t_replay t_reader t_deferred t_vl_reject t_manifest_cache t_ref_path t_ref_roundtrip
           t_vl_roundtrip t_attr_convert t_payload_compress t_step_rewrite t_reregister t_memspace t_manypaths
           t_dset_resize t_retention t_config t_flush_refresh t_open_step_objects t_overlay
           t_error_messages t_placeholder_extend t_group_attrs t_exit_midstep t_failed_replay t_links)
  add_executable(${t} ${t}.c)
  target_link_libraries(${t} PRIVATE vol_stream)
endforeach()

# Archival step-count scaling: measures, does not gate. Not registered with
# ctest -- 10k steps is too slow for every push; run it by hand.
add_executable(b_step_scale b_step_scale.c)
target_link_libraries(b_step_scale PRIVATE vol_stream)

add_test(NAME smoke COMMAND t_smoke)
# Per-file settings through H5Pset_fapl_stream(), their env-var overrides and
# the HDF5_VOL_CONNECTOR string form.
add_test(NAME config COMMAND t_config)
# What flush and refresh do inside and outside a step -- what reporting
# H5VL_CAP_FLAG_FLUSH_REFRESH promises.
add_test(NAME flush_refresh COMMAND t_flush_refresh)
# Library walks of open objects (a file close's flush, object counts, a second
# handle) while a dataset created in the open step is still open.
add_test(NAME open_step_objects COMMAND t_open_step_objects)
# The /stream overlay: a timeline view for tools that read the file natively.
add_test(NAME overlay COMMAND t_overlay)
# A failed step-API call names its reason on the error stack.
add_test(NAME error_messages COMMAND t_error_messages)
add_test(NAME step COMMAND t_step)
add_test(NAME replay COMMAND t_replay)
add_test(NAME reader COMMAND t_reader)
add_test(NAME deferred COMMAND t_deferred)
add_test(NAME vl_reject COMMAND t_vl_reject)
add_test(NAME manifest_cache COMMAND t_manifest_cache)
add_test(NAME ref_path COMMAND t_ref_path)
add_test(NAME ref_roundtrip COMMAND t_ref_roundtrip)
add_test(NAME vl_roundtrip COMMAND t_vl_roundtrip)
add_test(NAME attr_convert COMMAND t_attr_convert)
add_test(NAME payload_compress COMMAND t_payload_compress)
add_test(NAME step_rewrite COMMAND t_step_rewrite)
# H5Dset_extent() on a dataset created in the open step: a NeXus detector
# writer's create-empty-then-extend. See t_placeholder_extend.c.
add_test(NAME placeholder_extend COMMAND t_placeholder_extend)
# Group attributes written outside a step (a NeXus skeleton) reach the next
# step. See t_group_attrs.c.
add_test(NAME group_attrs COMMAND t_group_attrs)
# A writer that exits mid-step, a dataset it created still open, exits
# normally. See t_exit_midstep.c.
add_test(NAME exit_midstep COMMAND t_exit_midstep)
# A step whose replay fails is removed; the next commits in its place.
add_test(NAME failed_replay COMMAND t_failed_replay)
# Links made in a step (NeXus's /entry/data/data) go into the stream.
add_test(NAME links COMMAND t_links)
add_test(NAME reregister COMMAND t_reregister)
# Guards the capture path against ignoring mem_space_id -- see t_memspace.c's
# own comment. Asserts bracketed and unbracketed writes agree, which is the
# same equivalence t_step.c's byte-identity gate makes, extended to the axis
# the suite was uniform in.
add_test(NAME memspace COMMAND t_memspace)
# Exercises the path -> index hash map (H5VL_stream_pathmap_t) past its
# initial capacity, with colliding long-common-prefix keys. Every other test
# uses a handful of paths, so the map never grows or collides in them.
add_test(NAME manypaths COMMAND t_manypaths)
# H5Dset_extent() on a live cross-step handle -- the ADIOS2-style growing
# time-series array. Guards against a real bug found by running this: the
# resize wasn't captured at all, so it landed directly on an EARLIER step's
# already-committed real object, silently rewriting history. Now refused;
# see H5VL_stream_dataset_specific()'s own comment and t_dset_resize.c.
add_test(NAME dset_resize COMMAND t_dset_resize)
# Step retention (H5Fset_stream_retention_policy): vol-stream as a live
# transit pipe rather than an archive. Asserts the newest K steps survive,
# that they keep their own data, and that a policy set mid-stream bounds the
# steps committed BEFORE the call too -- the seeding path, which a
# policy-set-up-front test cannot reach. Needs no transport.
add_test(NAME retention COMMAND t_retention)

# M4 exit gate: the two-process Mercury round trip. M5 exit gate: SSG
# rendezvous, late joiners, and a departing reader that does not stall the
# writer. M7 exit gate: Block/Discard/Spill under a deliberately slow
# reader. M8 exit gate (first increment): the writer marshals only what is
# subscribed. M8.5: per-subscriber precision -- the subscription's own DCPL
# actually re-filters what goes on the wire. M9: predicate pushdown -- the
# writer evaluates a subscriber's value test and sends only what matches, or
# nothing. M10: live schema discovery -- a reader with no prior knowledge of
# the stream learns what it carries and subscribes from that alone. All need
# the transport, so all are only built/run when
# VOL_STREAM_HAVE_MERCURY is on (see ../CMakeLists.txt's
# VOL_STREAM_ENABLE_MERCURY option).
if(VOL_STREAM_HAVE_MERCURY)
  foreach(t t_transport t_rendezvous t_queue_policy t_subscribe t_precision t_precision_dual
           t_chunk_zerocopy t_chunk_shape_split t_subvolume_nd t_strided_write t_predicate t_subvolume_strided
           t_subvolume_grow t_rendezvous_barrier t_schema t_step_grouping t_eos)
    add_executable(${t} ${t}.c)
    target_link_libraries(${t} PRIVATE vol_stream)
  endforeach()
  add_test(NAME transport COMMAND t_transport)
  # The protocol-level fix for the subscription start-up race
  # (H5Fwait_subscribers): the writer blocks on its own subscription table
  # rather than on a ready-sentinel FILE, which is what keeps rendezvous free
  # of any shared filesystem. Deliberately contains no sentinel: the reader
  # dawdles before subscribing, so a writer that did not wait would lose step
  # 0 every time rather than flake.
  add_test(NAME rendezvous_barrier COMMAND t_rendezvous_barrier)
  add_test(NAME rendezvous COMMAND t_rendezvous)
  add_test(NAME queue_policy COMMAND t_queue_policy)
  add_test(NAME subscribe COMMAND t_subscribe)
  add_test(NAME precision COMMAND t_precision)
  add_test(NAME chunk_zerocopy COMMAND t_chunk_zerocopy)
  # M8.5 follow-up: a subscriber's requested chunk shape is honored, not
  # forced to a single chunk spanning the whole run -- see t_chunk_shape_
  # split.c's own top comment.
  add_test(NAME chunk_shape_split COMMAND t_chunk_shape_split)
  add_test(NAME subvolume_nd COMMAND t_subvolume_nd)
  add_test(NAME strided_write COMMAND t_strided_write)
  add_test(NAME predicate COMMAND t_predicate)
  add_test(NAME subvolume_strided COMMAND t_subvolume_strided)
  # The same column case as subvolume_strided, but on a GROWING extent and
  # with a selection needing more runs than the push budget -- the two gates
  # that silently disabled selection-exact routing on every streaming
  # dataset. See the test's own comment.
  add_test(NAME subvolume_grow COMMAND t_subvolume_grow)
  add_test(NAME schema COMMAND t_schema)
  # A step's pushes are all queued before its step-ready arrives, and never
  # interleave with the next step's -- the contract per-step consumers
  # (docs/python-plan.md's P0) rely on. See the test's own comment.
  add_test(NAME step_grouping COMMAND t_step_grouping)
  # A subscriber sees the writer leaving the group as the end of the stream,
  # after every step it announced. See the test's own comment.
  add_test(NAME eos COMMAND t_eos)
  set_tests_properties(transport rendezvous queue_policy subscribe precision chunk_zerocopy chunk_shape_split subvolume_nd strided_write predicate subvolume_strided subvolume_grow schema step_grouping eos PROPERTIES
    ENVIRONMENT "HDF5_PLUGIN_PATH=$<TARGET_FILE_DIR:vol_stream>"
    TIMEOUT 60
  )
  # eos's killed-writer case waits for SWIM to declare the writer dead.
  set_tests_properties(eos PROPERTIES TIMEOUT 150)

  # Phase 1: the same tests with every push payload sent by Mercury bulk
  # (VOL_STREAM_BULK_THRESHOLD=0) instead of inline. Their payloads are all
  # below the default threshold, so without these the bulk path -- borrowed
  # source bytes, the refilter's owned output, chunk-split slices, predicate
  # runs -- would only ever see bulk_push's one large payload.
  foreach(t subscribe precision chunk_zerocopy chunk_shape_split subvolume_nd strided_write predicate
            subvolume_strided subvolume_grow step_grouping)
    add_test(NAME ${t}_bulk COMMAND t_${t})
    set_tests_properties(${t}_bulk PROPERTIES
      ENVIRONMENT "HDF5_PLUGIN_PATH=$<TARGET_FILE_DIR:vol_stream>;VOL_STREAM_BULK_THRESHOLD=0"
      TIMEOUT 60
    )
  endforeach()

  # A reader can tell a finished stream from one that has not started: the
  # writer's "<file>.vsdone" marker.
  add_executable(t_writer_done t_writer_done.c)
  target_link_libraries(t_writer_done PRIVATE vol_stream)
  add_test(NAME writer_done COMMAND t_writer_done)
  set_tests_properties(writer_done PROPERTIES
    ENVIRONMENT "HDF5_PLUGIN_PATH=$<TARGET_FILE_DIR:vol_stream>"
    TIMEOUT 90
  )

  # Backfill for a late joiner: H5Fsubscribe_from() delivers the missed steps
  # from the writer's file, then the live ones, each once and in order.
  add_executable(t_backfill t_backfill.c)
  target_link_libraries(t_backfill PRIVATE vol_stream)
  add_test(NAME backfill COMMAND t_backfill)
  set_tests_properties(backfill PROPERTIES
    ENVIRONMENT "HDF5_PLUGIN_PATH=$<TARGET_FILE_DIR:vol_stream>"
    TIMEOUT 90
  )

  # The transport turned on per file through H5Pset_fapl_stream(), with
  # VOL_STREAM_NA unset.
  add_executable(t_config_na t_config_na.c)
  target_link_libraries(t_config_na PRIVATE vol_stream)
  add_test(NAME config_na COMMAND t_config_na)
  set_tests_properties(config_na PROPERTIES
    ENVIRONMENT "HDF5_PLUGIN_PATH=$<TARGET_FILE_DIR:vol_stream>"
    TIMEOUT 60
  )

  # A variable-length dataset is not pushed: its pointers mean nothing in
  # another process, and the buffer holding them was already freed.
  add_executable(t_vl_push t_vl_push.c)
  target_link_libraries(t_vl_push PRIVATE vol_stream)
  add_test(NAME vl_push COMMAND t_vl_push)
  set_tests_properties(vl_push PROPERTIES
    ENVIRONMENT "HDF5_PLUGIN_PATH=$<TARGET_FILE_DIR:vol_stream>"
    TIMEOUT 60
  )

  # One bulk registration serves all of a step's pushes: 32 bulk pushes of a
  # column, one registration of the step's staging buffer.
  add_executable(t_bulk_regions t_bulk_regions.c)
  target_link_libraries(t_bulk_regions PRIVATE vol_stream)
  add_test(NAME bulk_regions COMMAND t_bulk_regions)
  set_tests_properties(bulk_regions PROPERTIES
    ENVIRONMENT "HDF5_PLUGIN_PATH=$<TARGET_FILE_DIR:vol_stream>"
    TIMEOUT 60
  )

  # Phase 1's gate: one large push (256 MiB by default; T_BULK_MIB overrides,
  # e.g. 1024 for the RFC's 1 GiB) goes by bulk and arrives intact, while a
  # small push in the same step stays inline.
  add_executable(t_bulk_push t_bulk_push.c)
  target_link_libraries(t_bulk_push PRIVATE vol_stream)
  add_test(NAME bulk_push COMMAND t_bulk_push)
  set_tests_properties(bulk_push PROPERTIES
    ENVIRONMENT "HDF5_PLUGIN_PATH=$<TARGET_FILE_DIR:vol_stream>;VOL_STREAM_STAGE_PAYLOAD=0"
    TIMEOUT 180
  )

  # Benchmark, not a correctness gate in the usual sense: the growing
  # time-series array (t_dset_resize.c's supported re-create-each-step
  # pattern) streamed for real over na+sm, reporting commit latency,
  # end-to-end latency, and throughput. See its own top comment.
  add_executable(b_stream_grow b_stream_grow.c)
  target_link_libraries(b_stream_grow PRIVATE vol_stream)
  add_test(NAME stream_grow_benchmark COMMAND b_stream_grow)
  set_tests_properties(stream_grow_benchmark PROPERTIES
    ENVIRONMENT "HDF5_PLUGIN_PATH=$<TARGET_FILE_DIR:vol_stream>"
    TIMEOUT 120
  )

  # The O(N) counterpart: same growing array, but H5Dset_extent() + a
  # tail-only write through one persistent handle (t_dset_resize.c's case
  # 3) instead of a full rewrite every step. See its own top comment.
  add_executable(b_stream_grow_tail b_stream_grow_tail.c)
  target_link_libraries(b_stream_grow_tail PRIVATE vol_stream)
  add_test(NAME stream_grow_tail_benchmark COMMAND b_stream_grow_tail)
  set_tests_properties(stream_grow_tail_benchmark PROPERTIES
    ENVIRONMENT "HDF5_PLUGIN_PATH=$<TARGET_FILE_DIR:vol_stream>"
    TIMEOUT 120
  )

  # Writer-side cost against subscriber count (1/2/4/8), which is what the
  # asynchronous data push actually changes -- the two benchmarks above both
  # measure a single subscriber, the case where it matters least. See its
  # own top comment.
  #
  # Registered as a test, not just built: writing it exposed a real bug
  # (the writer indexed the SSG rank space across blocking pushes, so one
  # subscriber closing mid-loop silently dropped other subscribers' data --
  # see docs/dev-plan.md), and because every subscriber here verifies every
  # value it receives, this is the regression guard for that fix. It
  # reproduced the bug 3/3 before and 0/4 after.
  add_executable(b_push_fanout b_push_fanout.c)
  target_link_libraries(b_push_fanout PRIVATE vol_stream)
  add_test(NAME push_fanout_benchmark COMMAND b_push_fanout)
  set_tests_properties(push_fanout_benchmark PROPERTIES
    ENVIRONMENT "HDF5_PLUGIN_PATH=$<TARGET_FILE_DIR:vol_stream>"
    TIMEOUT 300
  )

  # PER-SUBSCRIBER COMPRESSION on a live detector stream, with real
  # facility geometry (an EIGER2 module written frame-by-frame into
  # /entry/data/data) and the real facility filter set (bslz4, zstd, zfp).
  # Filters beyond HDF5's built-in deflate are optional -- a subscriber
  # whose filter is missing reports itself unavailable rather than failing
  # -- so this passes on a stock HDF5 with no plugins installed.
  #
  # Registered as a test, not just built: every lossless subscriber verifies
  # what it receives against the scene it recomputes, so a narrowing that
  # dropped or corrupted data fails here rather than looking like a
  # spectacular compression ratio.
  add_executable(b_detector_narrowing b_detector_narrowing.c)
  target_link_libraries(b_detector_narrowing PRIVATE vol_stream)
  add_test(NAME detector_narrowing_benchmark COMMAND b_detector_narrowing)
  # ctest's ENVIRONMENT property REPLACES the inherited variable, it does not
  # extend it -- so pinning HDF5_PLUGIN_PATH to the connector directory made it
  # impossible to run this benchmark against bslz4/zstd/zfp at all, however the
  # caller's environment was set up. The codec-comparison benchmark measured no
  # codecs: five of its twelve subscribers exit when their filter is missing, and
  # it silently reported "filter NNNNN unavailable -- not measured" while the
  # remaining seven produced a writer cost roughly 2.5x too low on sparse scenes
  # (1.40x measured vs 2.27x with all twelve).
  #
  # The connector's own directory must stay first: the tests load vol-stream as
  # a plugin from there. Colon-separated, and deliberately NOT a CMake list --
  # a ';' would split the ENVIRONMENT property into two entries.
  if(VOL_STREAM_EXTRA_PLUGIN_PATH)
    set(_vs_plugin_path "$<TARGET_FILE_DIR:vol_stream>:${VOL_STREAM_EXTRA_PLUGIN_PATH}")
  else()
    set(_vs_plugin_path "$<TARGET_FILE_DIR:vol_stream>")
  endif()
  set_tests_properties(detector_narrowing_benchmark PROPERTIES
    ENVIRONMENT "HDF5_PLUGIN_PATH=${_vs_plugin_path}"
    TIMEOUT 900
  )

  # Every test above registers the connector explicitly, which leaves the
  # process default VOL native -- and that is exactly what hid the connector's
  # own scratch files inheriting vol-stream as their VOL and recursing back
  # into a replay in progress. This runs one of them the other way, through
  # HDF5_VOL_CONNECTOR, and asserts on the artifacts. See the script's own
  # comment.
  add_test(NAME plugin_scratch
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_plugin_scratch_test.sh
            $<TARGET_FILE:t_precision> $<TARGET_FILE_DIR:vol_stream>)
  set_tests_properties(plugin_scratch PROPERTIES TIMEOUT 90)
  # t_precision_dual.c meets the M8/M8.5 exit gate's literal wording (two
  # simultaneous subscribers, different precisions, one end_step) with three
  # real OS processes -- see its own top comment for the two real bugs found
  # and fixed while building it. Not part of the default `ctest` run: a
  # separate, deeper, already-diagnosed residual (Mercury's progress ULT can
  # block in an unbounded libfabric connect() to an already-departed peer
  # during teardown, well outside this project to fix) makes it occasionally
  # (not always -- correctness never fails) run past a normal timeout.
  # DISABLED, not omitted, so it stays discoverable in `ctest -N`/listings
  # even though ctest itself will not run a DISABLED test even under an
  # explicit `-R` filter -- run the built binary directly on demand:
  # test/t_precision_dual (from the build dir).
  add_test(NAME precision_dual COMMAND t_precision_dual)
  set_tests_properties(precision_dual PROPERTIES
    ENVIRONMENT "HDF5_PLUGIN_PATH=$<TARGET_FILE_DIR:vol_stream>"
    TIMEOUT 60
    DISABLED TRUE
  )
  # M9: h5stream `tail` follows a *live* writer, which is only possible over
  # the transport -- a writer's open file cannot be read by another process
  # at all. So unlike list/export it belongs here, with the other
  # transport-dependent tests.
  add_executable(t_h5stream_tail t_h5stream_tail.c)
  target_link_libraries(t_h5stream_tail PRIVATE vol_stream)
  add_test(NAME h5stream_tail COMMAND t_h5stream_tail $<TARGET_FILE:h5stream>)
  set_tests_properties(h5stream_tail PROPERTIES
    ENVIRONMENT "HDF5_PLUGIN_PATH=$<TARGET_FILE_DIR:vol_stream>"
    TIMEOUT 90
  )

  # t_queue_policy.c gates its Spill scenario on this at compile time -- the
  # library's own VOL_STREAM_HAVE_BAKE compile definition is PRIVATE to the
  # vol_stream target, so a consumer target needs its own copy to see it.
  if(VOL_STREAM_HAVE_BAKE)
    target_compile_definitions(t_queue_policy PRIVATE VOL_STREAM_HAVE_BAKE)
  endif()
endif()

# M6 exit gate (first increment): collective step boundaries and a
# parallel writer/reader over MPI, coprime rank counts. Needs MPI (the
# same MPI_C_FOUND ../CMakeLists.txt already links vol_stream against when
# present), a parallel HDF5, and mpiexec/mpirun.
if(MPI_C_FOUND AND VOL_STREAM_HDF5_IS_PARALLEL AND MPIEXEC_EXECUTABLE)
  add_executable(t_parallel t_parallel.c)
  target_link_libraries(t_parallel PRIVATE vol_stream)
  add_test(NAME parallel
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_parallel_test.sh
            --mpirun     ${MPIEXEC_EXECUTABLE}
            --bin        $<TARGET_FILE:t_parallel>
            --plugin-dir $<TARGET_FILE_DIR:vol_stream>
  )
  set_tests_properties(parallel PROPERTIES TIMEOUT 120)

  # dev-plan.md's CI matrix asks for coprime rank shapes because that is where
  # M×N projection bugs surface, and 3->2 alone is a thin sample: with only
  # two readers, most remainder cases cannot arise at all. 7->3 is the next
  # shape the matrix names.
  #
  # Left at the default 4 elements/rank on purpose: 7*4 = 28 across 3 readers
  # does NOT divide, so the readers take 10/9/9 and neither decomposition
  # lines up with the other. That uneven case is the realistic one and used
  # to be impossible to run -- t_parallel refused it outright ("global_size
  # 28 not divisible by 3 ranks") until slab_of()/owner_of() replaced the
  # divide-and-hope arithmetic on both sides.
  add_test(NAME parallel_7x3
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_parallel_test.sh
            --mpirun       ${MPIEXEC_EXECUTABLE}
            --bin          $<TARGET_FILE:t_parallel>
            --plugin-dir   $<TARGET_FILE_DIR:vol_stream>
            --writer-ranks 7
            --reader-ranks 3
  )
  set_tests_properties(parallel_7x3 PROPERTIES TIMEOUT 180)

  # M6.5 concentrator topology, both aggregation shapes dev-plan.md claims
  # as verified (3->2, one 3-rank concentrator group; 7->3, two groups of
  # 3 plus a singleton) -- previously only ever run manually (see
  # reference_vol_stream_local_env.md-style local verification), not
  # CI-wired at all. run_parallel_test.sh's own --concentration check
  # (grep for "concentrator rank ... wrote ... on behalf of") is the gate
  # against this silently no-op'ing again.
  add_test(NAME parallel_concentration
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_parallel_test.sh
            --mpirun       ${MPIEXEC_EXECUTABLE}
            --bin          $<TARGET_FILE:t_parallel>
            --plugin-dir   $<TARGET_FILE_DIR:vol_stream>
            --concentration 2
  )
  set_tests_properties(parallel_concentration PROPERTIES TIMEOUT 120)

  add_test(NAME parallel_concentration_7x3
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_parallel_test.sh
            --mpirun       ${MPIEXEC_EXECUTABLE}
            --bin          $<TARGET_FILE:t_parallel>
            --plugin-dir   $<TARGET_FILE_DIR:vol_stream>
            --writer-ranks 7
            --reader-ranks 3
            --concentration 3
  )
  set_tests_properties(parallel_concentration_7x3 PROPERTIES TIMEOUT 180)

  # A parallel writer with a queue policy set -- the combination end_step()
  # used to ignore outright, dispatching to the parallel replay without ever
  # consulting the policy, so M7's backpressure was serial-only.
  #
  # Making it work required reducing the whole decision across the
  # communicator (H5VL__stream_queue_lag_view()/_agree_and()), and the failure
  # mode of getting that wrong is a HANG, not wrong data: one rank skipping an
  # MPI_Allreduce its peers are blocked in. So the assertion is that the run
  # completes at all and stays byte-exact -- a divergence surfaces as this
  # test's own timeout, which is why the timeout is deliberately tight.
  #
  # Scope, stated plainly: this covers the no-pressure path only. The transport
  # comes up (the .vsgroup sidecar proves it) so the policy region and both of
  # its collectives execute on every rank every step, which is what pins the
  # lockstep property. But with no reader attached, no rank has a tracked
  # reader, so the ranks cannot disagree about lag here even if the reduction
  # were removed. Covering the case that actually applies backpressure needs a
  # lagging reader acking to a single writer rank -- the configuration where
  # per-rank views genuinely diverge -- and is not built yet.
  if(VOL_STREAM_HAVE_MERCURY)
    # The case parallel_queue_policy below cannot reach: a real reader, so the
    # lag view is genuinely asymmetric across ranks (a reader acks to exactly
    # one of them). Without the reduction the un-acked ranks race ahead and the
    # run HANGS rather than answering wrongly, which is why the timeout is
    # tight and the writer asserts its own per-rank stall.
    add_executable(t_parallel_lag t_parallel_lag.c)
    target_link_libraries(t_parallel_lag PRIVATE vol_stream)
    add_test(NAME parallel_lag
      COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_parallel_lag_test.sh
              --mpirun       ${MPIEXEC_EXECUTABLE}
              --bin          $<TARGET_FILE:t_parallel_lag>
              --plugin-dir   $<TARGET_FILE_DIR:vol_stream>
              --writer-ranks 3
    )
    set_tests_properties(parallel_lag PROPERTIES TIMEOUT 90)

    add_test(NAME parallel_queue_policy
      COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_parallel_test.sh
              --mpirun       ${MPIEXEC_EXECUTABLE}
              --bin          $<TARGET_FILE:t_parallel>
              --plugin-dir   $<TARGET_FILE_DIR:vol_stream>
              --writer-ranks 3
              --reader-ranks 2
              --queue-policy block
    )
    set_tests_properties(parallel_queue_policy PROPERTIES
      ENVIRONMENT "VOL_STREAM_NA=na+sm"
      TIMEOUT 120
    )
  endif()
endif()

# M9: h5stream's `list`, checked by running the tool and parsing what a user
# would actually see. Registered here rather than in tools/ so it sits with
# the rest of the suite; it takes the tool's path as an argument because the
# tool is a sibling target, not something on PATH.
add_executable(t_h5stream_list t_h5stream_list.c)
target_link_libraries(t_h5stream_list PRIVATE vol_stream)
add_test(NAME h5stream_list COMMAND t_h5stream_list $<TARGET_FILE:h5stream>)

add_executable(t_h5stream_export t_h5stream_export.c)
target_link_libraries(t_h5stream_export PRIVATE vol_stream)
add_test(NAME h5stream_export COMMAND t_h5stream_export $<TARGET_FILE:h5stream>)

# M9's last item: `h5stream history` turns each step into an addressable
# onion-VFD revision. Reads back with plain HDF5 and an onion fapl, no
# connector, since being readable without this project is the point.
add_executable(t_onion_history t_onion_history.c)
target_link_libraries(t_onion_history PRIVATE vol_stream)
add_test(NAME onion_history COMMAND t_onion_history $<TARGET_FILE:h5stream>)

set_tests_properties(h5stream_list h5stream_export onion_history PROPERTIES
  ENVIRONMENT "HDF5_PLUGIN_PATH=$<TARGET_FILE_DIR:vol_stream>"
  TIMEOUT 60
)

# VOL_STREAM_H5DIFF points t_replay (the M2 replay-invariant gate) at an
# h5diff binary when it is not on PATH -- e.g. an uninstalled HDF5 build
# tree, which is the normal setup when developing against the library
# itself (see the HDF5-discovery note at the top of ../CMakeLists.txt).
set(VOL_STREAM_H5DIFF "" CACHE FILEPATH "h5diff binary used by the t_replay test (default: PATH lookup)")

set_tests_properties(smoke step replay reader deferred vl_reject manifest_cache ref_path PROPERTIES
  ENVIRONMENT "HDF5_PLUGIN_PATH=$<TARGET_FILE_DIR:vol_stream>;VOL_STREAM_H5DIFF=${VOL_STREAM_H5DIFF}"
)

# The M0 exit gate: HDF5's own test/API suite, run through the connector and
# compared against the same suite run natively. Opt-in, because it needs a
# separate HDF5 source tree configured with HDF5_TEST_API.
if(VOL_STREAM_HDF5_API_TESTS)
  add_test(NAME api_conformance
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_api_suite.sh
            --api-bin  ${VOL_STREAM_HDF5_API_TESTS}
            --plugin-dir $<TARGET_FILE_DIR:vol_stream>
  )
  set_tests_properties(api_conformance PROPERTIES TIMEOUT 3600)
endif()
