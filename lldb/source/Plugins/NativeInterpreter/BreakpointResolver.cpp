//===-- BreakpointResolver.cpp --------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Plugins/NativeInterpreter/BreakpointResolver.h"

#include "lldb/Breakpoint/BreakpointLocation.h"
#include "lldb/Target/StackFrame.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/raw_ostream.h"

using namespace lldb;
using namespace lldb_private;

lldb_private::Searcher::CallbackReturn
lldb_private::InterpretedBreakpointResolver::SearchCallback(
    SearchFilter &filter, SymbolContext &context, Address *addr) {
  // No module, continue I guess?
  if (!context.module_sp)
    return eCallbackReturnContinue;

  // Add the address to the debugger anchor.
  const auto *symbol = context.module_sp->FindFirstSymbolWithNameAndType(
      ConstString{"__ibid_debugger_anchor"}, lldb::eSymbolTypeCode);
  if (!symbol)
    return eCallbackReturnContinue;

  // If the symbol is valid, we've found our callback. Time to stop the
  // iteration and add a location to the breakpoint, and add/store the facade.
  if (symbol->GetAddress().IsValid()) {
    m_real_loc = AddLocation(symbol->GetAddress());
    m_facade = GetBreakpoint()->AddFacadeLocation();
    return eCallbackReturnStop;
  }

  return eCallbackReturnContinue;
}

lldb::BreakpointLocationSP lldb_private::InterpretedBreakpointResolver::WasHit(
    lldb::StackFrameSP frame_sp, lldb::BreakpointLocationSP bp_loc_sp) {
  llvm::errs() << "WAS HIT:\n";

  // TODO: we could instead write the breakpoint locations to the inferior and
  // check in the bridge module. Then, if the location matches, we call a
  // __ibid_breakpoint_hit function that we set as the real address here. Then,
  // it's unconditional that we hit *a* breakpoint, and we need to just check
  // *which* we hit. Add a global functions: __ibid_add_breakpoint that
  // returns a breakpoint ID, and then the ID could be passed as an argument to
  // the __ibid_breakpoint_hit so we can tell if it was hit or not.

  // TODO: alternatively, I could simply have this *also* call the IBID
  // functions, though that might be slow as well.

  // See if the frame's function name matches one of our names. If so, return
  // the facade.
  auto *frame_name = frame_sp->GetFunctionName();
  auto found = llvm::find_if(function_names,
                             [&](auto name) { return name == frame_name; });
  if (found != function_names.end()) {
    // Update the file/line too.
    auto &sc = frame_sp->GetSymbolContext(lldb::eSymbolContextLineEntry);
    file = sc.line_entry.GetFile();
    line = sc.line_entry.line;
    return m_facade;
  }

  // File/line/col matches, return facade.
  auto &sc = frame_sp->GetSymbolContext(lldb::eSymbolContextLineEntry);
  auto line_entry = sc.line_entry;
  bool file_match = line_entry.GetFile().FileEquals(file);
  // If our input file path is absolute, the directories must also be equal.
  if (file.IsAbsolute())
    file_match &= line_entry.GetFile().DirectoryEquals(file);

  llvm::errs() << "  file: ";
  line_entry.GetFile().Dump(llvm::errs());
  llvm::errs() << " vs ";
  file.Dump(llvm::errs());
  llvm::errs() << "\n";

  // Line and column match are always the same - line matches exactly, column
  // only if provided.
  bool line_match = line_entry.line == line;
  llvm::errs() << "  line: " << line_entry.line << "\n";
  bool col_match = col == 0 || line_entry.column == col;
  if (file_match && line_match && col_match) {
    // Set the function name now that we have it.
    function_names.push_back(frame_sp->GetFunctionName());
    llvm::errs() << "  yep, hit it\n";
    return m_facade;
  }

  llvm::errs() << "  no hit\n";

  // Nothing was hit, so return an empty location.
  return {};
}
