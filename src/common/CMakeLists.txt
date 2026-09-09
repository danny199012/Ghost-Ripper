# ghost-common: shared capture format, math and logging used by the hook DLL,
# the viewer and the command-line tools.
add_library(ghost-common STATIC
    src/attribute.cpp
    src/attribute_guess.cpp
    src/capture_reader.cpp
    src/capture_writer.cpp
)

target_include_directories(ghost-common PUBLIC include)

# Keep the hook DLL dependency-free: no external libs here, only the STL.
set_target_properties(ghost-common PROPERTIES POSITION_INDEPENDENT_CODE ON)
