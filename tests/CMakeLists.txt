# Unit tests. All of these are headless: capture-format round-trips, the BC
# texture decoders and the exporters, so they run on any platform.
add_executable(test_capture_format test_capture_format.cpp)
target_link_libraries(test_capture_format PRIVATE ghost-common)
add_test(NAME capture_format COMMAND test_capture_format)

add_executable(test_bc_decode test_bc_decode.cpp)
target_link_libraries(test_bc_decode PRIVATE ghost_viewer_core)
add_test(NAME bc_decode COMMAND test_bc_decode)

add_executable(test_gltf_export test_gltf_export.cpp)
target_link_libraries(test_gltf_export PRIVATE ghost_viewer_core)
add_test(NAME gltf_export COMMAND test_gltf_export)

add_executable(test_viewer_assets test_viewer_assets.cpp)
target_link_libraries(test_viewer_assets PRIVATE ghost_viewer_core)
add_test(NAME viewer_assets COMMAND test_viewer_assets)
