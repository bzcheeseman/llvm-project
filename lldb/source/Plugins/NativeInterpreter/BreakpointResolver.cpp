//===-- BreakpointResolver.cpp --------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Plugins/NativeInterpreter/BreakpointResolver.h"

#include "lldb/Breakpoint/BreakpointLocation.h"
#include "lldb/Expression/UserExpression.h"
#include "lldb/Symbol/VariableList.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/StackFrame.h"
#include "lldb/Target/Target.h"
#include "lldb/Target/Thread.h"
#include "lldb/Utility/DataBufferHeap.h"
#include "lldb/Utility/Status.h"
#include "lldb/ValueObject/ValueObject.h"
#include "lldb/ValueObject/ValueObjectVariable.h"
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

namespace {
std::string EscapeForCString(llvm::StringRef s) {
  std::string out;
  for (char c : s) {
    if (c == '"' || c == '\\')
      out += '\\';
    out += c;
  }
  return out;
}
} // namespace

bool lldb_private::InterpretedBreakpointResolver::RegisterInBridge(
    StackFrameSP frame_sp) {
  auto thread_sp = frame_sp->GetThread();
  auto process_sp = thread_sp->GetProcess();
  auto &target = process_sp->GetTarget();

  std::string filename_str = file.GetPath();
  std::string expr;
  llvm::raw_string_ostream(expr)
      << "(unsigned)__ibid_add_breakpoint(\""
      << EscapeForCString(filename_str) << "\", "
      << filename_str.size() << "ul, " << line << "u, " << col << "u)";

  EvaluateExpressionOptions options;
  options.SetLanguage(eLanguageTypeC_plus_plus);
  options.SetUnwindOnError(true);
  options.SetIgnoreBreakpoints(true);

  ValueObjectSP result_sp;
  ExpressionResults result =
      target.EvaluateExpression(expr, frame_sp.get(), result_sp, options);
  if (result != eExpressionCompleted || !result_sp)
    return false;

  m_id = (unsigned)result_sp->GetValueAsUnsigned(UINT_MAX);
  if (m_id == UINT_MAX)
    return false;

  // Add a fast-path physical breakpoint on __ibid_breakpoint_hit.
  if (m_real_loc) {
    if (ModuleSP mod = m_real_loc->GetAddress().GetModule()) {
      const Symbol *sym = mod->FindFirstSymbolWithNameAndType(
          ConstString{"__ibid_breakpoint_hit"}, eSymbolTypeCode);
      if (sym && sym->GetAddress().IsValid())
        m_hit_loc = AddLocation(sym->GetAddress());
    }
    // Disable the slow-path anchor; we now only need __ibid_breakpoint_hit.
    llvm::consumeError(m_real_loc->SetEnabled(false));
  }

  return true;
}

bool lldb_private::InterpretedBreakpointResolver::CurrentPythonLocationMatches(
    ProcessSP process_sp, TargetSP target_sp) {
  VariableList variable_list;
  target_sp->GetImages().FindGlobalVariables(
      ConstString("__ibid_current_backtrace"), 1, variable_list);
  if (variable_list.GetSize() != 1)
    return false;

  ExecutionContextScope *exe_scope = process_sp.get();
  ValueObjectSP frame_list_sp = ValueObjectVariable::Create(
      exe_scope, variable_list.GetVariableAtIndex(0));
  if (!frame_list_sp)
    return false;

  auto type = frame_list_sp->GetCompilerType();
  auto pointee_size_or = type.GetPointeeType().GetByteSize(exe_scope);
  if (!pointee_size_or) {
    llvm::consumeError(pointee_size_or.takeError());
    return false;
  }

  ValueObjectSP frame_info_sp = frame_list_sp->GetSyntheticChildAtOffset(
      0, type.GetPointeeType(), /*can_create=*/true);
  if (!frame_info_sp)
    return false;

  auto filename_sp = frame_info_sp->GetChildMemberWithName("filename");
  if (!filename_sp)
    return false;

  auto len_sp = filename_sp->GetChildMemberWithName("len");
  auto data_sp = filename_sp->GetChildMemberWithName("data");
  if (!len_sp || !data_sp)
    return false;

  std::string filename;
  unsigned length = len_sp->GetValueAsUnsigned(0);
  if (length > 0 && data_sp->GetValueAsUnsigned(0) != 0) {
    lldb::WritableDataBufferSP buffer_sp =
        std::make_shared<lldb_private::DataBufferHeap>(length, '\0');
    Status s;
    data_sp->ReadPointedString(buffer_sp, s, false);
    if (s.Success())
      filename = std::string((const char *)buffer_sp->GetBytes(), length);
  }

  auto line_sp = frame_info_sp->GetChildMemberWithName("line");
  if (!line_sp)
    return false;
  unsigned current_line = line_sp->GetValueAsUnsigned(0);

  FileSpec current_file(filename);
  bool file_match = current_file.FileEquals(file);
  if (file.IsAbsolute())
    file_match &= current_file.DirectoryEquals(file);

  bool col_match = col == 0;
  if (!col_match) {
    auto col_sp = frame_info_sp->GetChildMemberWithName("column");
    if (col_sp)
      col_match = col_sp->GetValueAsUnsigned(0) == col;
  }

  return file_match && current_line == line && col_match;
}

lldb::BreakpointLocationSP lldb_private::InterpretedBreakpointResolver::WasHit(
    lldb::StackFrameSP frame_sp, lldb::BreakpointLocationSP bp_loc_sp) {

  auto thread_sp = frame_sp->GetThread();
  auto process_sp = thread_sp->GetProcess();
  auto target_sp = process_sp->GetTarget().shared_from_this();

  // Fast path: read __ibid_hit_id from the inferior and compare with our ID.
  if (m_id != UINT_MAX) {
    SymbolContextList sc_list;
    target_sp->GetImages().FindSymbolsWithNameAndType(
        ConstString("__ibid_hit_id"), eSymbolTypeAny, sc_list);
    if (sc_list.GetSize() == 0)
      return {};

    SymbolContext sc;
    sc_list.GetContextAtIndex(0, sc);
    if (!sc.symbol)
      return {};

    lldb::addr_t addr =
        sc.symbol->GetAddress().GetLoadAddress(target_sp.get());
    if (addr == LLDB_INVALID_ADDRESS)
      return {};

    Status error;
    unsigned hit_id = (unsigned)process_sp->ReadUnsignedIntegerFromMemory(
        addr, sizeof(unsigned), UINT_MAX, error);
    if (error.Fail())
      return {};

    return hit_id == m_id ? m_facade : lldb::BreakpointLocationSP{};
  }

  // First anchor hit: register with the bridge to enable the fast path.
  RegisterInBridge(frame_sp);

  // The bridge checks g_source_breakpoints *before* calling
  // __ibid_debugger_anchor, so if the first trace event lands exactly on our
  // target line the bridge saw an empty registry and skipped the match. Check
  // the current interpreter location now so we don't miss that first hit.
  if (m_id != UINT_MAX && CurrentPythonLocationMatches(process_sp, target_sp))
    return m_facade;

  return {};
}
