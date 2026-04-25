# Requires Python3 development libs.
find_package(Python3 REQUIRED COMPONENTS Development)

add_library(lldb_bridge SHARED module.cpp)
target_link_libraries(lldb_bridge PRIVATE ${Python3_LIBRARIES})
target_include_directories(lldb_bridge PRIVATE ${Python3_INCLUDE_DIRS})
# Has to have no `lib` prefix and it has to use the `.so` suffix.
set_target_properties(lldb_bridge PROPERTIES PREFIX "")
set_target_properties(lldb_bridge PROPERTIES SUFFIX ".so")

# Configure the bridge wrapper file.
configure_file(bridge_wrapper.py.in ${CMAKE_BINARY_DIR}/bridge_wrapper.py @ONLY)
