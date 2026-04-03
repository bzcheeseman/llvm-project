add_subdirectory(bridge)
# Add a dependency on the lldb_bridge for lldbPluginNativeInterpreter. This ensures that we always get the freshest version of the bridge.
add_dependencies(lldbPluginNativeInterpreter lldb_bridge)
