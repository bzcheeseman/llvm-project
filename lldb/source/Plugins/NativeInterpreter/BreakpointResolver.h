//===-- BreakpointResolver.h ------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Breakpoint/BreakpointResolver.h"
#include "lldb/Core/Module.h"
#include "lldb/Core/SearchFilter.h"
#include "lldb/Symbol/SymbolContext.h"

#include "lldb/lldb-enumerations.h"
#include "lldb/lldb-forward.h"

#include <string>

namespace lldb_private {

class InterpretedBreakpointResolver : public BreakpointResolver {
public:
  InterpretedBreakpointResolver(const lldb::BreakpointSP &bkpt,
                                std::vector<std::string> function_names,
                                FileSpec file, unsigned line, unsigned col)
      : BreakpointResolver(bkpt, BreakpointResolver::InterpreterResolver),
        function_names(function_names), file(file), line(line), col(col) {}

  ~InterpretedBreakpointResolver() override = default;

  StructuredData::ObjectSP SerializeToStructuredData() override {
    /*
    TODO: Maybe we need this, maybe we don't?

    StructuredData::DictionarySP options_dict_sp(
        new StructuredData::Dictionary());

    options_dict_sp->AddStringItem(GetKey(OptionNames::PythonClassName),
                                    m_class_name);
    if (m_args.IsValid())
      options_dict_sp->AddItem(GetKey(OptionNames::ScriptArgs),
                              m_args.GetObjectSP());

    return WrapOptionsDict(options_dict_sp);
    */
    return {};
  }

  Searcher::CallbackReturn SearchCallback(SearchFilter &filter,
                                          SymbolContext &context,
                                          Address *addr) override;

  lldb::SearchDepth GetDepth() override { return lldb::eSearchDepthModule; }

  void GetDescription(Stream *s) override {
    s->PutCString("IBID Breakpoint Resolver");
  }

  lldb::BreakpointLocationSP
  WasHit(lldb::StackFrameSP frame_sp,
         lldb::BreakpointLocationSP bp_loc_sp) override;

  void Dump(Stream *s) const override {}

  /// Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const InterpretedBreakpointResolver *) {
    return true;
  }
  static inline bool classof(const BreakpointResolver *V) {
    return V->getResolverID() == BreakpointResolver::InterpreterResolver;
  }

  lldb::BreakpointResolverSP
  CopyForBreakpoint(lldb::BreakpointSP &breakpoint) override {
    return std::make_shared<InterpretedBreakpointResolver>(
        breakpoint, function_names, file, line, col);
  }

private:
  std::vector<std::string> function_names;
  FileSpec file;
  unsigned line;
  unsigned col = 0;

  // TODO: Do I need an impl for NotifyBreakpointSet?

  // TODO: private constructor to copy these over?
  lldb::BreakpointLocationSP m_facade;
  lldb::BreakpointLocationSP m_real_loc;

  // Set after the bridge has been told about this breakpoint via
  // __ibid_add_breakpoint. UINT_MAX means not yet registered (slow path).
  unsigned m_id = UINT_MAX;
  // Location on __ibid_breakpoint_hit added once m_id is known (fast path).
  lldb::BreakpointLocationSP m_hit_loc;

  bool RegisterInBridge(lldb::StackFrameSP frame_sp);
  bool CurrentPythonLocationMatches(lldb::ProcessSP process_sp,
                                    lldb::TargetSP target_sp);

  InterpretedBreakpointResolver(const InterpretedBreakpointResolver &) = delete;
  const InterpretedBreakpointResolver &
  operator=(const InterpretedBreakpointResolver &) = delete;
};

} // namespace lldb_private
