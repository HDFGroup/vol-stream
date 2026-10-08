# A real in-situ fatigue experiment replayed through the connector: the
# TomoBank Al7075 fatigue-corrosion series, one checkpoint per step, with
# crack_monitor.py subscribing to the rows around the crack. See README.md.
add_executable(tomo_fatigue_writer tomo_fatigue_writer.c)
target_link_libraries(tomo_fatigue_writer PRIVATE vol_stream)
