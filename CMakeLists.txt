cmake_minimum_required(VERSION 3.18)
project(vol-stream C)

set(VOL_STREAM_VERSION 0.0.1)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# HDF5
#
# The connector is a plugin: it links libhdf5 and is dlopen'd by it. A build
# against the HDF5 the tests will run against is the only supported setup --
# H5VL_VERSION is compiled in, and a mismatch is the failure mode that broke
# the ADIOS2 connector when H5VL_VERSION moved from 0 to 2.
# ---------------------------------------------------------------------------
# Developing against an uninstalled HDF5 build tree is normal for anyone working
# on the library itself, and find_package cannot locate one. Pre-setting both
# variables skips discovery:
#
#   cmake -DHDF5_C_LIBRARIES=/path/hdf5/build/bin/libhdf5.so \
#         -DHDF5_INCLUDE_DIRS="/path/hdf5/src;/path/hdf5/build/src" ...
#
if(HDF5_C_LIBRARIES AND HDF5_INCLUDE_DIRS)
  message(STATUS "Using caller-supplied HDF5 (skipping find_package)")
  if(NOT HDF5_VERSION)
    set(HDF5_VERSION "caller-supplied")
  endif()
else()
  # HDF5 2.x only.  The connector calls H5Tdecode2() and H5Dread_chunk2(),
  # neither of which exists in 1.14.6, the latest 1.14 release -- so 1.14
  # fails at compile time, not at run time.  This floor used to read 1.14,
  # predating those calls; it never meant 1.14 was supported.  See the
  # version-support section of docs/user-guide.md.
  find_package(HDF5 2.0 REQUIRED COMPONENTS C)
endif()

# A parallel HDF5 pulls <mpi.h> in from H5public.h, so anything including
# hdf5.h needs the MPI include path -- including this connector. Not REQUIRED:
# a serial HDF5 neither needs nor wants it.
find_package(MPI COMPONENTS C)
if(MPI_C_FOUND)
  message(STATUS "MPI found: ${MPI_C_COMPILER}")
endif()

option(VOL_STREAM_ENABLE_LOGGING "Log every VOL callback to stdout" OFF)
option(VOL_STREAM_BUILD_TESTS "Build the test programs" ON)

# Third-party HDF5 filter plugins (bslz4 32008, zstd 32015, zfp 32013) live
# outside this project and outside HDF5's built-ins. b_detector_narrowing needs
# them: without them five of its twelve subscribers exit and it measures a
# writer cost far below the truth. Colon-separated, appended after the
# connector's own directory. Empty by default -- the benchmark still runs and
# still says which filters it could not find.
set(VOL_STREAM_EXTRA_PLUGIN_PATH "" CACHE STRING
  "Colon-separated HDF5 filter plugin directories appended to HDF5_PLUGIN_PATH for tests")
option(VOL_STREAM_BUILD_EXAMPLES "Build the example programs" ON)
option(VOL_STREAM_BUILD_PYTHON "Build the volstream Python package (needs Python >= 3.10 headers)" OFF)

# ---------------------------------------------------------------------------
# flatcc -- manifest framing (M2)
#
# The step manifest (src/vol_stream.fbs) is FlatBuffers, chosen so a later
# field (M8's predicate) is a compatible schema change rather than a wire
# break. flatcc has no exported CMake package config, so probe for a
# Spack-provided install first and fall back to vendoring via FetchContent.
#
# The vendored build needs two overrides confirmed against the actual
# v0.6.1 release, not assumed:
#   - CMAKE_POLICY_VERSION_MINIMUM=3.5: flatcc's own cmake_minimum_required
#     predates what modern CMake accepts.
#   - FLATCC_ALLOW_WERROR=OFF: flatccrt fails to build under GCC 15
#     (-Werror plus a -Wunterminated-string-initialization new in GCC 15,
#     in a bundled portable/grisu3_print.h shim flatcc doesn't control).
# ---------------------------------------------------------------------------
find_program(FLATCC_EXECUTABLE flatcc)
find_library(FLATCC_RUNTIME_LIBRARY flatccrt)
find_path(FLATCC_INCLUDE_DIR flatcc/flatcc_builder.h)

if(FLATCC_EXECUTABLE AND FLATCC_RUNTIME_LIBRARY AND FLATCC_INCLUDE_DIR)
  message(STATUS "flatcc: found (${FLATCC_EXECUTABLE})")
  set(VOL_STREAM_FLATCC_EXE ${FLATCC_EXECUTABLE})
  add_library(vol_stream_flatccrt UNKNOWN IMPORTED)
  set_target_properties(vol_stream_flatccrt PROPERTIES
    IMPORTED_LOCATION ${FLATCC_RUNTIME_LIBRARY}
    INTERFACE_INCLUDE_DIRECTORIES ${FLATCC_INCLUDE_DIR})
  set(VOL_STREAM_FLATCC_RT_TARGET vol_stream_flatccrt)
else()
  message(STATUS "flatcc: not found, vendoring v0.6.1 via FetchContent")
  include(FetchContent)
  set(FLATCC_TEST OFF CACHE BOOL "" FORCE)
  set(FLATCC_CXX_TEST OFF CACHE BOOL "" FORCE)
  set(FLATCC_INSTALL OFF CACHE BOOL "" FORCE)
  set(FLATCC_ALLOW_WERROR OFF CACHE BOOL "" FORCE)
  set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

  # flatcc_cli is a build-time code generator, invoked below to produce our
  # own headers -- it is never linked into vol_stream. A sanitizer build
  # (-fsanitize=... in CMAKE_C_FLAGS, e.g. the CI sanitizer matrix) would
  # otherwise instrument it too, and running the instrumented tool trips
  # real, pre-existing UB in flatcc's own code (misaligned loads in its
  # bundled cmetrohash, a null passed to a never-null codegen parameter)
  # that aborts the codegen step outright -- unrelated to whether our own
  # code has any issues. Shadow CMAKE_C_FLAGS (non-cache set(), so it only
  # affects this directory scope and everything FetchContent pulls in
  # here) with the -fsanitize= token stripped, then restore it immediately
  # after so vol_stream itself still gets the real flags.
  set(_vol_stream_saved_c_flags "${CMAKE_C_FLAGS}")
  string(REGEX REPLACE "-fsanitize=[A-Za-z,]+" "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")

  FetchContent_Declare(flatcc
    GIT_REPOSITORY https://github.com/dvidelabs/flatcc.git
    GIT_TAG        v0.6.1
    GIT_SHALLOW    TRUE)
  FetchContent_MakeAvailable(flatcc)

  set(CMAKE_C_FLAGS "${_vol_stream_saved_c_flags}")
  unset(_vol_stream_saved_c_flags)
  unset(CMAKE_POLICY_VERSION_MINIMUM)
  set(VOL_STREAM_FLATCC_EXE $<TARGET_FILE:flatcc_cli>)
  set(VOL_STREAM_FLATCC_RT_TARGET flatccrt)
  # flatcc's own CMakeLists.txt uses directory-scoped include_directories()
  # rather than target_include_directories(), so linking against flatccrt
  # does not propagate its include path the way a normal target would --
  # add it explicitly.
  set(FLATCC_INCLUDE_DIR ${flatcc_SOURCE_DIR}/include)
endif()

# ---------------------------------------------------------------------------
# Mercury / Argobots / Margo / Flock -- transport, progress engine, and group
# membership (M4/M5)
#
# Optional. A build with none of these installed still produces a fully
# working connector: M0-M3's file-based capture/replay is untouched, and only
# the step_ready notification path and the group-based rendezvous
# (src/tr_mercury.c) are skipped. Detection is via pkg-config, which all of
# them ship -- borrow by default, per dev-plan.md, rather than a bespoke find
# module.
#
# Data still moves through the replicated /step/<n>/ file at these
# milestones, not over Mercury; Mercury/Margo carry the control-plane
# notification that a step committed, and Flock (M5) carries group membership
# and failure detection so a reader can join, leave, or die without a
# hand-rolled attach protocol or liveness tracking. See docs/dev-plan.md's
# M4 and M5 sections.
#
# Group membership was mochi-ssg through M10. SSG was declared deprecated by
# its maintainers in August 2026 in favour of Flock, which is the maintained
# successor and supports the same SWIM failure detection we depend on. Flock
# splits what SSG did into a server side (flock-server: being a group member)
# and a client side (flock-client: reading a group's view), so both are
# required.
# ---------------------------------------------------------------------------
set(VOL_STREAM_ENABLE_MERCURY "AUTO" CACHE STRING
  "Build the Mercury/Margo/Flock transport (M4/M5): ON, OFF, or AUTO (default; enable if found)")
set_property(CACHE VOL_STREAM_ENABLE_MERCURY PROPERTY STRINGS AUTO ON OFF)

# Minimum versions, enforced below rather than merely documented. Both floors
# are load-bearing, and a build against anything older does not fail at
# configure time or link time -- it builds and then crashes at teardown, which
# is the worst way to learn about it:
#
#   margo 0.24.2  -- earlier releases cannot complete margo_finalize() with a
#                    Flock provider registered. Measured on 0.24.0: a fix for a
#                    race on (pre-)finalize callbacks landed in 0.24.1/0.24.2
#                    and is required. On margo 0.17 the process either hangs in
#                    __margo_timer_list_free() or segfaults in
#                    __margo_abt_destroy(), 0 clean runs out of 320.
#   flock 0.8.0   -- earlier releases crash when two group members call
#                    flock_provider_destroy() concurrently, which is the normal
#                    case here: every reader and the writer tear down together
#                    at H5Fclose(). Fixed upstream by "make provider teardown
#                    safe against concurrent RPCs and finalize" and the SWIM
#                    RPC-deregistration race. On 0.7.1 our fan-out benchmarks
#                    failed most runs; on 0.8.0 they are clean.
set(VOL_STREAM_MARGO_MIN_VERSION "0.24.2")
set(VOL_STREAM_FLOCK_MIN_VERSION "0.8.0")

set(VOL_STREAM_HAVE_MERCURY FALSE)
if(NOT VOL_STREAM_ENABLE_MERCURY STREQUAL "OFF")
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    # Probe unversioned first, purely so the diagnostics below can name the
    # version actually installed rather than just saying "not found".
    pkg_check_modules(VOL_STREAM_MERCURY QUIET mercury)
    pkg_check_modules(VOL_STREAM_ARGOBOTS QUIET argobots)
    pkg_check_modules(VOL_STREAM_MARGO_ANY QUIET margo)
    pkg_check_modules(VOL_STREAM_FLOCK_SERVER_ANY QUIET flock-server)
    pkg_check_modules(VOL_STREAM_FLOCK_CLIENT_ANY QUIET flock-client)

    pkg_check_modules(VOL_STREAM_MARGO QUIET "margo>=${VOL_STREAM_MARGO_MIN_VERSION}")
    pkg_check_modules(VOL_STREAM_FLOCK_SERVER QUIET "flock-server>=${VOL_STREAM_FLOCK_MIN_VERSION}")
    pkg_check_modules(VOL_STREAM_FLOCK_CLIENT QUIET "flock-client>=${VOL_STREAM_FLOCK_MIN_VERSION}")

    # Distinguish "too old" from "absent" -- they need different fixes, and
    # silently disabling the transport for someone who has margo installed but
    # a version behind is exactly the confusing case worth spelling out.
    set(VOL_STREAM_TRANSPORT_TOO_OLD "")
    if(VOL_STREAM_MARGO_ANY_FOUND AND NOT VOL_STREAM_MARGO_FOUND)
      list(APPEND VOL_STREAM_TRANSPORT_TOO_OLD
           "margo ${VOL_STREAM_MARGO_ANY_VERSION} (need >= ${VOL_STREAM_MARGO_MIN_VERSION})")
    endif()
    if(VOL_STREAM_FLOCK_SERVER_ANY_FOUND AND NOT VOL_STREAM_FLOCK_SERVER_FOUND)
      list(APPEND VOL_STREAM_TRANSPORT_TOO_OLD
           "flock ${VOL_STREAM_FLOCK_SERVER_ANY_VERSION} (need >= ${VOL_STREAM_FLOCK_MIN_VERSION})")
    endif()
  endif()

  if(VOL_STREAM_MERCURY_FOUND AND VOL_STREAM_MARGO_FOUND AND VOL_STREAM_ARGOBOTS_FOUND
     AND VOL_STREAM_FLOCK_SERVER_FOUND AND VOL_STREAM_FLOCK_CLIENT_FOUND)
    set(VOL_STREAM_HAVE_MERCURY TRUE)

    # pkg_check_modules()'s ordinary (non-static) result variables only
    # cover each module's own direct Libs -- not the Requires.private/
    # Libs.private closure (na, mercury_util, mchecksum, libfabric,
    # json-c, ...). That closure is needed here even though everything is
    # built shared: this system's linker does not resolve an executable's
    # undefined symbols by walking a directly-linked shared library's own
    # NEEDED entries at build time (only the dynamic loader does that,
    # later, at load time), so vol_stream's own link line has to carry the
    # full transitive closure. `pkg-config --static` computes exactly that;
    # FindPkgConfig has no equivalent option in this CMake version, so ask
    # the pkg-config binary directly -- all four modules in one call, so
    # pkg-config dedupes/orders the merged graph itself.
    execute_process(
      COMMAND ${PKG_CONFIG_EXECUTABLE} --static --libs mercury margo argobots flock-server flock-client
      OUTPUT_VARIABLE VOL_STREAM_TRANSPORT_STATIC_LIBS
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE VOL_STREAM_TRANSPORT_PKGCONFIG_RC)
    execute_process(
      COMMAND ${PKG_CONFIG_EXECUTABLE} --static --cflags mercury margo argobots flock-server flock-client
      OUTPUT_VARIABLE VOL_STREAM_TRANSPORT_STATIC_CFLAGS
      OUTPUT_STRIP_TRAILING_WHITESPACE)

    if(NOT VOL_STREAM_TRANSPORT_PKGCONFIG_RC EQUAL 0)
      message(FATAL_ERROR "pkg-config --static failed for mercury/margo/argobots/flock-server/flock-client")
    endif()

    separate_arguments(VOL_STREAM_TRANSPORT_STATIC_LIBS UNIX_COMMAND "${VOL_STREAM_TRANSPORT_STATIC_LIBS}")
    separate_arguments(VOL_STREAM_TRANSPORT_STATIC_CFLAGS UNIX_COMMAND "${VOL_STREAM_TRANSPORT_STATIC_CFLAGS}")
  elseif(VOL_STREAM_TRANSPORT_TOO_OLD)
    # Installed but behind the floor. Always fatal, even under AUTO: silently
    # building without the transport because a dependency is one release too
    # old looks like the connector "just not supporting streaming", and the
    # alternative -- building against it anyway -- crashes at teardown.
    string(REPLACE ";" ", " VOL_STREAM_TRANSPORT_TOO_OLD_STR "${VOL_STREAM_TRANSPORT_TOO_OLD}")
    message(FATAL_ERROR
      "vol-stream transport: dependency too old -- ${VOL_STREAM_TRANSPORT_TOO_OLD_STR}.\n"
      "These floors are not cosmetic. Older margo cannot complete "
      "margo_finalize() with a Flock provider registered, and flock before "
      "0.8.0 crashes when group members tear down concurrently, which happens "
      "on every H5Fclose(). Both build and link fine and then fail at run "
      "time, so this is checked here instead.\n"
      "Upgrade, or configure with -DVOL_STREAM_ENABLE_MERCURY=OFF to build "
      "without the streaming transport.")
  elseif(VOL_STREAM_ENABLE_MERCURY STREQUAL "ON")
    message(FATAL_ERROR
      "VOL_STREAM_ENABLE_MERCURY=ON but mercury/margo(>=${VOL_STREAM_MARGO_MIN_VERSION})/"
      "argobots/flock-server(>=${VOL_STREAM_FLOCK_MIN_VERSION})/flock-client "
      "were not all found via pkg-config")
  endif()
endif()

if(VOL_STREAM_HAVE_MERCURY)
  message(STATUS "Mercury transport: enabled (mercury ${VOL_STREAM_MERCURY_VERSION}, "
                  "margo ${VOL_STREAM_MARGO_VERSION}, argobots ${VOL_STREAM_ARGOBOTS_VERSION}, "
                  "flock ${VOL_STREAM_FLOCK_SERVER_VERSION})")
else()
  message(STATUS "Mercury transport: disabled (set VOL_STREAM_ENABLE_MERCURY=ON to require it)")
endif()

# ---------------------------------------------------------------------------
# Which transport implementation backs the vs_tr_*() surface (EVALUATION).
#
# Both src/tr_mercury.c and src/tr_diaspora.cpp define the same symbols, so
# exactly one is linked -- hence a configure-time choice, not a runtime one.
# Making it runtime-selectable would mean putting vs_tr_*() behind a vtable,
# which is a refactor worth doing only once the second backend has earned its
# place.
#
#   mercury  -- src/tr_mercury.c. Mercury/Margo/Flock directly. The default,
#               and the only backend that implements the whole surface.
#   diaspora -- src/tr_diaspora.cpp, on the Diaspora Stream API, which
#               reaches Mofka and (through the same API) Kafka/Redpanda/MSK
#               plus a built-in "files" driver for testing. Eight of the 24
#               vs_tr_*() entry points have no counterpart in a
#               consumer-oblivious broker model -- notably the five
#               writer-side narrowing callbacks, vs_tr_writer_min_acked_step()
#               (which the whole queue policy is driven from) and
#               vs_tr_reader_get_current_step() (late-joiner seeding). They
#               return failure rather than a plausible-looking wrong answer.
#               See src/tr_diaspora.cpp's header comment.
#
# Deliberately NOT dropping the Mercury dependencies from a diaspora build,
# even though tr_diaspora.cpp does not use them: tr_mercury.h includes
# <margo.h> for the one type vs_tr_get_mid() returns, and keeping the
# dependency environment identical between the two backends means an A/B
# comparison isolates the transport implementation rather than the whole dep
# closure. A production diaspora build would guard that include and drop
# them.
#
# VOL_STREAM_HAVE_MERCURY stays the compile define in both cases. It gates 32
# call sites in H5VLstream.c and has always meant "a transport is present"
# rather than "Mercury specifically"; renaming it would mean editing
# H5VLstream.c, and whether H5VLstream.c needs editing is precisely the claim
# this backend exists to test.
# ---------------------------------------------------------------------------
set(VOL_STREAM_TRANSPORT "mercury" CACHE STRING
  "Transport backend behind vs_tr_*(): mercury (default) or diaspora")
set_property(CACHE VOL_STREAM_TRANSPORT PROPERTY STRINGS mercury diaspora)

if(NOT VOL_STREAM_TRANSPORT MATCHES "^(mercury|diaspora)$")
  message(FATAL_ERROR
    "VOL_STREAM_TRANSPORT must be 'mercury' or 'diaspora', got '${VOL_STREAM_TRANSPORT}'")
endif()

set(VOL_STREAM_HAVE_DIASPORA FALSE)
if(VOL_STREAM_TRANSPORT STREQUAL "diaspora")
  if(NOT VOL_STREAM_HAVE_MERCURY)
    message(FATAL_ERROR
      "VOL_STREAM_TRANSPORT=diaspora still needs the Mercury/Margo/Flock probe to "
      "succeed, for <margo.h> and for dependency parity with the mercury backend "
      "(see the comment above this check)")
  endif()

  # The Diaspora Stream API is C++17; the connector is otherwise pure C.
  enable_language(CXX)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)

  # Unversioned on purpose: the installed diaspora-stream-api.pc has an empty
  # Version field, so any version constraint here fails to match at all.
  pkg_check_modules(VOL_STREAM_DIASPORA QUIET diaspora-stream-api)
  if(NOT VOL_STREAM_DIASPORA_FOUND)
    message(FATAL_ERROR
      "VOL_STREAM_TRANSPORT=diaspora but diaspora-stream-api was not found by "
      "pkg-config. Add its prefix's lib/pkgconfig to PKG_CONFIG_PATH.")
  endif()
  set(VOL_STREAM_HAVE_DIASPORA TRUE)
  message(STATUS "Transport backend:  diaspora (src/tr_diaspora.cpp, "
                  "diaspora-stream-api at ${VOL_STREAM_DIASPORA_PREFIX})")
else()
  message(STATUS "Transport backend:  mercury (src/tr_mercury.c)")
endif()

# ---------------------------------------------------------------------------
# M7's Spill queue policy: BAKE (a Mochi provider for node-local
# byte-addressable regions) embedded on the same margo instance
# tr_mercury.c already drives -- src/tr_bake.c. Separate from the Mercury
# block above (its own AUTO/ON/OFF, its own pkg-config --static closure)
# rather than folded into it: BAKE's build hard-requires libpmemobj (PMDK),
# a real, heavier dependency than anything Mercury/Margo/Argobots/SSG need,
# so Block and Discard must stay fully usable on a system that has the
# transport but not BAKE. See docs/dev-plan.md's M7 section.
# ---------------------------------------------------------------------------
set(VOL_STREAM_ENABLE_BAKE "AUTO" CACHE STRING
  "Build the BAKE-backed Spill queue policy (M7): ON, OFF, or AUTO (default; enable if found and Mercury is enabled)")
set_property(CACHE VOL_STREAM_ENABLE_BAKE PROPERTY STRINGS AUTO ON OFF)

set(VOL_STREAM_HAVE_BAKE FALSE)
# BAKE is excluded on the diaspora backend regardless of what is installed:
# tr_bake.c embeds a BAKE provider on the transport's margo instance, and
# tr_diaspora.cpp has none to give (vs_tr_get_mid() returns
# MARGO_INSTANCE_NULL). Under Mofka, persistence is the partition manager's
# job (Warabi + abt-io), so the right end state on that path is for
# tr_bake.c not to be built at all rather than ported.
if(VOL_STREAM_HAVE_MERCURY AND NOT VOL_STREAM_HAVE_DIASPORA
   AND NOT VOL_STREAM_ENABLE_BAKE STREQUAL "OFF")
  if(PkgConfig_FOUND)
    pkg_check_modules(VOL_STREAM_BAKE_CLIENT QUIET bake-client)
    pkg_check_modules(VOL_STREAM_BAKE_SERVER QUIET bake-server)
    pkg_check_modules(VOL_STREAM_ABTIO QUIET abt-io)
  endif()

  if(VOL_STREAM_BAKE_CLIENT_FOUND AND VOL_STREAM_BAKE_SERVER_FOUND AND VOL_STREAM_ABTIO_FOUND)
    set(VOL_STREAM_HAVE_BAKE TRUE)

    # Same transitive-closure reasoning as the Mercury/Margo/Argobots/SSG
    # block above, kept as a separate pkg-config --static call so BAKE stays
    # optional independent of that stack.
    execute_process(
      COMMAND ${PKG_CONFIG_EXECUTABLE} --static --libs bake-client bake-server abt-io
      OUTPUT_VARIABLE VOL_STREAM_BAKE_STATIC_LIBS
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE VOL_STREAM_BAKE_PKGCONFIG_RC)
    execute_process(
      COMMAND ${PKG_CONFIG_EXECUTABLE} --static --cflags bake-client bake-server abt-io
      OUTPUT_VARIABLE VOL_STREAM_BAKE_STATIC_CFLAGS
      OUTPUT_STRIP_TRAILING_WHITESPACE)

    if(NOT VOL_STREAM_BAKE_PKGCONFIG_RC EQUAL 0)
      message(FATAL_ERROR "pkg-config --static failed for bake-client/bake-server/abt-io")
    endif()

    separate_arguments(VOL_STREAM_BAKE_STATIC_LIBS UNIX_COMMAND "${VOL_STREAM_BAKE_STATIC_LIBS}")
    separate_arguments(VOL_STREAM_BAKE_STATIC_CFLAGS UNIX_COMMAND "${VOL_STREAM_BAKE_STATIC_CFLAGS}")
  elseif(VOL_STREAM_ENABLE_BAKE STREQUAL "ON")
    message(FATAL_ERROR
      "VOL_STREAM_ENABLE_BAKE=ON but bake-client/bake-server/abt-io were not all found via pkg-config")
  endif()
endif()

if(VOL_STREAM_HAVE_BAKE)
  message(STATUS "BAKE spill:        enabled (bake-client ${VOL_STREAM_BAKE_CLIENT_VERSION}, "
                  "abt-io ${VOL_STREAM_ABTIO_VERSION})")
else()
  message(STATUS "BAKE spill:        disabled (Spill policy falls back to Discard; "
                  "set VOL_STREAM_ENABLE_BAKE=ON to require it)")
endif()

set(VOL_STREAM_FBS ${CMAKE_CURRENT_SOURCE_DIR}/src/vol_stream.fbs)
set(VOL_STREAM_FBS_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(VOL_STREAM_FBS_GEN_HDR ${VOL_STREAM_FBS_GEN_DIR}/vol_stream_builder.h)

add_custom_command(
  OUTPUT ${VOL_STREAM_FBS_GEN_HDR}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${VOL_STREAM_FBS_GEN_DIR}
  COMMAND ${VOL_STREAM_FLATCC_EXE} -a -o ${VOL_STREAM_FBS_GEN_DIR} ${VOL_STREAM_FBS}
  DEPENDS ${VOL_STREAM_FBS} $<$<TARGET_EXISTS:flatcc_cli>:flatcc_cli>
  COMMENT "Generating flatcc headers for vol_stream.fbs"
  VERBATIM)
add_custom_target(vol_stream_fbs_gen DEPENDS ${VOL_STREAM_FBS_GEN_HDR})

set(VOL_STREAM_SOURCES src/H5VLstream.c)
if(VOL_STREAM_HAVE_MERCURY)
  # Exactly one of these defines the vs_tr_*() symbols -- see
  # VOL_STREAM_TRANSPORT above.
  if(VOL_STREAM_HAVE_DIASPORA)
    list(APPEND VOL_STREAM_SOURCES src/tr_diaspora.cpp)
  else()
    list(APPEND VOL_STREAM_SOURCES src/tr_mercury.c)
  endif()
endif()
if(VOL_STREAM_HAVE_BAKE)
  list(APPEND VOL_STREAM_SOURCES src/tr_bake.c)
endif()

# Mercury/Margo/Argobots/Flock (and BAKE) arrive as "-L<dir> -l<name>" flags
# from `pkg-config --static`, not as full library paths, so CMake has no way
# to infer an RPATH for them -- every binary then fails at load time with
# "libflock-server.so.0: cannot open shared object file" unless the caller
# happens to have LD_LIBRARY_PATH set.  Feed those directories in explicitly.
#
# Set before any target is created, so every target below (the connector, the
# tests, the tools, the examples) inherits it.
if(VOL_STREAM_HAVE_MERCURY OR VOL_STREAM_HAVE_BAKE)
  set(VOL_STREAM_DEP_LIBDIRS
      ${VOL_STREAM_MERCURY_LIBRARY_DIRS}  ${VOL_STREAM_MERCURY_STATIC_LIBRARY_DIRS}
      ${VOL_STREAM_MARGO_LIBRARY_DIRS}    ${VOL_STREAM_MARGO_STATIC_LIBRARY_DIRS}
      ${VOL_STREAM_ARGOBOTS_LIBRARY_DIRS} ${VOL_STREAM_ARGOBOTS_STATIC_LIBRARY_DIRS}
      ${VOL_STREAM_FLOCK_SERVER_LIBRARY_DIRS} ${VOL_STREAM_FLOCK_SERVER_STATIC_LIBRARY_DIRS}
      ${VOL_STREAM_FLOCK_CLIENT_LIBRARY_DIRS} ${VOL_STREAM_FLOCK_CLIENT_STATIC_LIBRARY_DIRS}
      ${VOL_STREAM_BAKE_CLIENT_LIBRARY_DIRS} ${VOL_STREAM_BAKE_CLIENT_STATIC_LIBRARY_DIRS}
      ${VOL_STREAM_BAKE_SERVER_LIBRARY_DIRS} ${VOL_STREAM_BAKE_SERVER_STATIC_LIBRARY_DIRS}
      ${VOL_STREAM_ABTIO_LIBRARY_DIRS}       ${VOL_STREAM_ABTIO_STATIC_LIBRARY_DIRS}
      ${VOL_STREAM_DIASPORA_LIBRARY_DIRS}    ${VOL_STREAM_DIASPORA_STATIC_LIBRARY_DIRS})
  if(VOL_STREAM_DEP_LIBDIRS)
    list(REMOVE_DUPLICATES VOL_STREAM_DEP_LIBDIRS)
    list(APPEND CMAKE_BUILD_RPATH ${VOL_STREAM_DEP_LIBDIRS})
    message(STATUS "  dep RPATH:       ${VOL_STREAM_DEP_LIBDIRS}")
  endif()
endif()

add_library(vol_stream SHARED ${VOL_STREAM_SOURCES})
add_dependencies(vol_stream vol_stream_fbs_gen)

target_include_directories(vol_stream
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${VOL_STREAM_FBS_GEN_DIR}
    ${FLATCC_INCLUDE_DIR}
)

if(HDF5_INCLUDE_DIRS)
  target_include_directories(vol_stream PUBLIC ${HDF5_INCLUDE_DIRS})
endif()

target_link_libraries(vol_stream PUBLIC ${HDF5_C_LIBRARIES})
target_link_libraries(vol_stream PRIVATE ${VOL_STREAM_FLATCC_RT_TARGET})

if(MPI_C_FOUND)
  target_link_libraries(vol_stream PUBLIC MPI::MPI_C)
endif()

if(VOL_STREAM_HAVE_MERCURY)
  # tr_mercury.c's reader-side wait/notify queue is guarded by a pthread
  # mutex/condvar, not an Argobots primitive -- see the note at the top of
  # that file for why.
  find_package(Threads REQUIRED)

  target_compile_definitions(vol_stream PRIVATE VOL_STREAM_HAVE_MERCURY)
  # PUBLIC: the full `pkg-config --static` closure (flags and libraries
  # alike) needs to reach consumers' own link lines too -- see the comment
  # above where these are computed.
  target_compile_options(vol_stream PUBLIC ${VOL_STREAM_TRANSPORT_STATIC_CFLAGS})
  target_link_libraries(vol_stream PUBLIC ${VOL_STREAM_TRANSPORT_STATIC_LIBS} Threads::Threads)
endif()

if(VOL_STREAM_HAVE_DIASPORA)
  target_compile_definitions(vol_stream PRIVATE VOL_STREAM_HAVE_DIASPORA)
  target_include_directories(vol_stream PRIVATE ${VOL_STREAM_DIASPORA_INCLUDE_DIRS})
  target_link_libraries(vol_stream PUBLIC ${VOL_STREAM_DIASPORA_LDFLAGS})
endif()

if(VOL_STREAM_HAVE_BAKE)
  target_compile_definitions(vol_stream PRIVATE VOL_STREAM_HAVE_BAKE)
  target_compile_options(vol_stream PUBLIC ${VOL_STREAM_BAKE_STATIC_CFLAGS})
  target_link_libraries(vol_stream PUBLIC ${VOL_STREAM_BAKE_STATIC_LIBS})
endif()

if(VOL_STREAM_ENABLE_LOGGING)
  target_compile_definitions(vol_stream PRIVATE ENABLE_STREAM_LOGGING)
endif()

# HDF5's plugin loader looks for an unversioned shared object on the search
# path, so do not decorate the name.
set_target_properties(vol_stream PROPERTIES
  OUTPUT_NAME vol_stream
  C_VISIBILITY_PRESET hidden
)

# ---------------------------------------------------------------------------
# Report the H5VL_VERSION being built against.
#
# This is pinned deliberately: the connector declares a struct version at
# compile time, and silently running against a library expecting a different
# one is the documented way connectors break.
# ---------------------------------------------------------------------------
include(CheckCSourceCompiles)
set(CMAKE_REQUIRED_INCLUDES ${HDF5_INCLUDE_DIRS})
if(MPI_C_FOUND)
  list(APPEND CMAKE_REQUIRED_INCLUDES ${MPI_C_INCLUDE_DIRS})
endif()
# Compile-only: this is a preprocessor fact, so there is nothing to link or run
# and no rpath to get wrong.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
check_c_source_compiles("
#include \"hdf5.h\"
#if H5VL_VERSION < 3
#error \"H5VL_VERSION < 3\"
#endif
int probe(void) { return H5VL_VERSION; }
" VOL_STREAM_H5VL_VERSION_OK)
unset(CMAKE_TRY_COMPILE_TARGET_TYPE)

if(NOT VOL_STREAM_H5VL_VERSION_OK)
  message(WARNING
    "Could not confirm H5VL_VERSION >= 3 against this HDF5. vol-stream is "
    "developed against 3; a connector built for a different VOL class struct "
    "version will fail to load -- the failure that broke the ADIOS2 connector "
    "when H5VL_VERSION moved. See docs/dev-plan.md, CI matrix.")
endif()

# Whether this HDF5 is a parallel build. Finding MPI is not enough to build the
# parallel tests: a serial HDF5 on a machine that also has MPI on its PATH has
# no H5Pset_fapl_mpio(), and t_parallel then fails to compile.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
check_c_source_compiles("
#include \"hdf5.h\"
#ifndef H5_HAVE_PARALLEL
#error \"serial HDF5\"
#endif
int probe(void) { return 0; }
" VOL_STREAM_HDF5_IS_PARALLEL)
unset(CMAKE_TRY_COMPILE_TARGET_TYPE)
if(MPI_C_FOUND AND NOT VOL_STREAM_HDF5_IS_PARALLEL)
  message(STATUS "MPI found but HDF5 is serial: the parallel tests are skipped")
endif()

# ---------------------------------------------------------------------------
# VFD SWMR (optional, examples only)
#
# VFD SWMR is not in mainline HDF5 2.x -- it lives on the feature/vfd-swmr-port
# branch. examples/vfd_swmr/ demonstrates that vol-stream stacks on top of it
# (VOL over VFD) and that a second process can then read a live stream's file,
# which is otherwise impossible. Nothing in the connector itself uses or needs
# it, so this is a pure examples gate: absent, the example is skipped and
# everything else builds exactly as before.
#
# See docs/user-guide.md's "Composing with VFD SWMR" section for the measured
# results and the md_file_path/md_file_name trap.
# ---------------------------------------------------------------------------
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
check_c_source_compiles("
#include \"hdf5.h\"
int probe(void) {
    H5F_vfd_swmr_config_t cfg;
    cfg.version = H5F__CURR_VFD_SWMR_CONFIG_VERSION;
    (void)H5Pset_vfd_swmr_config(H5P_DEFAULT, &cfg);
    (void)H5Fvfd_swmr_end_tick(H5I_INVALID_HID);
    return 0;
}
" VOL_STREAM_HAVE_VFD_SWMR)
unset(CMAKE_TRY_COMPILE_TARGET_TYPE)


# ---------------------------------------------------------------------------
# Plugin signing
#
# HDF5 verifies plugin signatures before dlopen'ing them, and a build with
# enforcement on rejects an unsigned connector outright:
#
#   H5PL__read_and_validate_footer(): not a signed HDF5 plugin
#
# That makes signing a hard requirement for the HDF5_VOL_CONNECTOR path, which
# is how the M0 exit gate drives HDF5's test/API suite -- not an optional
# hardening step. Point VOL_STREAM_SIGN_KEY at a private key to sign as part of
# the build; readers then need HDF5_PLUGIN_KEYSTORE set to a directory holding
# the matching public key. See docs/plugin-signing.md.
# ---------------------------------------------------------------------------
set(VOL_STREAM_SIGN_KEY "" CACHE FILEPATH "RSA private key (PEM) used to sign the built plugin")
find_program(H5SIGN_EXECUTABLE h5sign HINTS ${HDF5_ROOT}/bin)

if(VOL_STREAM_SIGN_KEY)
  if(NOT H5SIGN_EXECUTABLE)
    message(FATAL_ERROR "VOL_STREAM_SIGN_KEY is set but h5sign was not found; set H5SIGN_EXECUTABLE.")
  endif()
  # -f so an incremental rebuild re-signs instead of failing on the old signature.
  add_custom_command(TARGET vol_stream POST_BUILD
    COMMAND ${H5SIGN_EXECUTABLE} -f -p $<TARGET_FILE:vol_stream> -k ${VOL_STREAM_SIGN_KEY}
    COMMENT "Signing $<TARGET_FILE_NAME:vol_stream> with ${VOL_STREAM_SIGN_KEY}"
    VERBATIM)
  message(STATUS "Plugin signing:  enabled")
else()
  message(STATUS "Plugin signing:  disabled (set VOL_STREAM_SIGN_KEY to enable)")
  message(STATUS "                 an unsigned plugin cannot be loaded via HDF5_VOL_CONNECTOR")
  message(STATUS "                 when the HDF5 in use enforces signatures")
endif()

add_subdirectory(tools)

if(VOL_STREAM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(test)
endif()

if(VOL_STREAM_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(VOL_STREAM_BUILD_PYTHON)
  add_subdirectory(python)
endif()

install(TARGETS vol_stream LIBRARY DESTINATION lib)
install(FILES src/H5VLstream.h DESTINATION include)

message(STATUS "")
message(STATUS "vol-stream ${VOL_STREAM_VERSION}  (M7 - queue policy and BAKE spill)")
message(STATUS "  HDF5:            ${HDF5_VERSION}")
message(STATUS "  flatcc:          ${VOL_STREAM_FLATCC_EXE}")
message(STATUS "  mercury:         ${VOL_STREAM_HAVE_MERCURY}")
message(STATUS "  bake:            ${VOL_STREAM_HAVE_BAKE}")
message(STATUS "  transport:       ${VOL_STREAM_TRANSPORT}")
message(STATUS "  vfd swmr:        ${VOL_STREAM_HAVE_VFD_SWMR} (examples/vfd_swmr only)")
message(STATUS "  logging:         ${VOL_STREAM_ENABLE_LOGGING}")
message(STATUS "  tests:           ${VOL_STREAM_BUILD_TESTS}")
message(STATUS "  examples:        ${VOL_STREAM_BUILD_EXAMPLES}")
message(STATUS "  python:          ${VOL_STREAM_BUILD_PYTHON}")
message(STATUS "  h5sign:          ${H5SIGN_EXECUTABLE}")
message(STATUS "")
