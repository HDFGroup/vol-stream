# Real use cases, not tests -- see each subdirectory's own README.md.
# Requires the transport (subscription push has no non-transport fallback),
# same gate as the transport-dependent targets in test/CMakeLists.txt.
if(VOL_STREAM_HAVE_MERCURY)
  add_subdirectory(heat_diffusion)
  add_subdirectory(reaction_diffusion)
  add_subdirectory(narrowing_demo)
  add_subdirectory(detector_pipeline)
  add_subdirectory(fatigue_crack)
endif()

# The VFD SWMR proof of concept is the opposite case: it needs NO transport
# (it demonstrates the file path), but does need an HDF5 that has VFD SWMR --
# which mainline 2.x does not. Gated separately for that reason.
if(VOL_STREAM_HAVE_VFD_SWMR)
  add_subdirectory(vfd_swmr)
endif()
