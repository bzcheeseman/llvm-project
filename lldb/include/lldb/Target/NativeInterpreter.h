//===-- NativeInterpreter.h -----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Core/PluginInterface.h"
#include "lldb/lldb-forward.h"
#include "llvm/Support/Error.h"

namespace lldb_private {
class Thread;
} // namespace lldb_private

namespace lldb_private {
class NativeInterpreter : public PluginInterface {
public:
  /// Create an instance of the native interpreter plugin.
  static lldb::NativeInterpreterSP
  CreateInstance(lldb::ModuleSP module_to_elide);

  /// An implementation of this plugin will be able to provide a
  /// SyntheticFrameProvider that can be used to provide interpreter frames.
  virtual lldb::SyntheticFrameProviderSP
  GetFrameProvider(lldb::StackFrameListSP frame_list) = 0;

  /// An implementation of this plugin will be able to provide a
  /// BreakpointResolver that can be used to resolve interpreter breakpoints for
  /// one of a set of function names.
  virtual lldb::BreakpointResolverSP GetBreakpointResolverForFunctionNames(
      const lldb::BreakpointSP &bkpt,
      std::vector<std::string> function_names) = 0;

  /// An implementation of this plugin will be able to provide a
  /// BreakpointResolver that can be used to resolve interpreter breakpoints for
  /// a given file/line/col location. If the column is provided, we match on
  /// file + line + col, otherwise just file + col. If the filename is an
  /// absolute path, then we match on the full path, otherwise we match on just
  /// the filename.
  virtual lldb::BreakpointResolverSP
  GetBreakpointResolverForSourceLoc(const lldb::BreakpointSP &bkpt,
                                    FileSpec file, unsigned line,
                                    unsigned col) = 0;

  /// Return a thread plan that steps over one source line in an interpreted
  /// frame, or nullptr if the current frame is not an interpreted frame.
  virtual lldb::ThreadPlanSP CreateStepOverPlan(Thread &thread) {
    return nullptr;
  }
};
} // namespace lldb_private
