add_lldb_library(lldbPluginNativeInterpreter PLUGIN
  BreakpointResolver.cpp
  FrameProvider.cpp
  Plugin.cpp
  ThreadPlanStepOverInterpreted.cpp

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

if (LLDB_ENABLE_PYTHON)
  add_subdirectory(Python)
endif()
