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
class NativeInterpreter : public PluginInterface {
public:
  /// Create an instance of the native interpreter plugin for the provided name,
  /// e.g. "python". If no such plugin is provided, then we won't do anything.
  static lldb::NativeInterpreterSP CreateInstance(llvm::StringRef interpreter_name, lldb::ModuleSP module_to_elide);

  /// An implementation of this plugin will be able to provide a
  /// SyntheticFrameProvider that can be used to provide interpreter frames.
  virtual lldb::SyntheticFrameProviderSP
  GetFrameProvider(lldb::StackFrameListSP frame_list) = 0;

  /// Create an anchor breakpoint.
  virtual lldb::BreakpointSP CreateAnchorBreakpoint(lldb_private::Target &target, lldb_private::BreakpointResolver &resolver) = 0;

  /// An implementation of this plugin will be able to provide a
  /// BreakpointResolver that can be used to resolve interpreter breakpoints.
  virtual lldb::BreakpointResolverSP GetBreakpointResolver(lldb::ThreadSP) = 0;
};
} // namespace lldb_private
