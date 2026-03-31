add_lldb_library(lldbPluginNativeInterpreter PLUGIN
  BreakpointResolver.cpp
  FrameProvider.cpp
  Plugin.cpp

  LINK_COMPONENTS
    BinaryFormat
    Object
    Support
  LINK_LIBS
    lldbCore
    lldbTarget
    lldbUtility
    lldbPluginProcessUtility
  )

add_subdirectory(bridge)
# Add a dependency on the lldb_bridge for lldbPluginNativeInterpreter. This ensures that we always get the freshest version of the bridge.
add_dependencies(lldbPluginNativeInterpreter lldb_bridge)
