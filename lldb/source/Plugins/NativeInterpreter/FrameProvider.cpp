//===-- FrameProvider.cpp -------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Plugins/NativeInterpreter/FrameProvider.h"
#include "lldb/API/SBFrame.h"
#include "lldb/Core/Module.h"
#include "lldb/Symbol/Function.h"
#include "lldb/Symbol/Symbol.h"
#include "lldb/Symbol/Variable.h"
#include "lldb/Symbol/VariableList.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/Target.h"
#include "lldb/Target/Thread.h"
#include "lldb/Target/ThreadPlan.h"
#include "lldb/Utility/DataBufferHeap.h"
#include "lldb/Utility/DataBufferLLVM.h"
#include "lldb/Utility/LLDBLog.h"
#include "lldb/Utility/Log.h"
#include "lldb/Utility/ValueType.h"
#include "lldb/ValueObject/ValueObjectList.h"
#include "lldb/ValueObject/ValueObjectVariable.h"
#include "lldb/lldb-enumerations.h"
#include "lldb/lldb-forward.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/LogicalResult.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>

using namespace lldb;
using namespace lldb_private;

namespace {
// TODO: How do I make it so this is NOT called from within an expression?
class InterpretedFrame : public StackFrame {
public:
  using StackFrame::StackFrame;

  static char ID;
  bool isA(const void *ClassID) const override {
    return ClassID == &ID || StackFrame::isA(ClassID);
  }
  static bool classof(const StackFrame *obj) { return obj->isA(&ID); }

  /// Overload GetSymbolContext so that if the line entry was requested, we can
  /// fetch it from the inferior and resolve it before handing off to the base
  /// implementation.
  const SymbolContext &
  GetSymbolContext(lldb::SymbolContextItem resolve_scope) override {
    // If the line entry was requested, resolve that from our global variable.
    if (resolve_scope & lldb::eSymbolContextLineEntry) {
      // Only reset the line entry if we have to.
      if (!m_flags.Test(lldb::eSymbolContextLineEntry)) {
        auto line_entry_or = GetIBIDLineInfo();
        if (auto err = line_entry_or.takeError()) {
          LLDB_LOG_ERROR(GetLog(LLDBLog::Target), std::move(err),
                         "while getting interpreted line info: {0}");
        } else {
          // No error, so set the line entry and set the bit that says we did
          // it.
          m_sc.line_entry = *line_entry_or;
          m_flags.Set(lldb::eSymbolContextLineEntry);
        }
      }
    }

    return StackFrame::GetSymbolContext(resolve_scope);
  }

  /// Read an __ibid_string from the inferior. The string is a struct with data
  /// and length.
  llvm::Expected<std::string>
  ReadIBIDStringFromInferior(lldb_private::ValueObject *valobj) {
    auto len_sp = valobj->GetChildMemberWithName("len");
    if (!len_sp)
      return llvm::createStringError("no `len` child member");

    auto data_sp = valobj->GetChildMemberWithName("data");
    if (!data_sp)
      return llvm::createStringError("no `data` child member");

    unsigned length = len_sp->GetValueAsUnsigned(0);

    // Zero length or nullptr isn't an error, it means empty string.
    if (length == 0 || data_sp->GetValueAsUnsigned(0) == 0)
      return "";

    // Construct a memory buffer on this side with the correct length.
    lldb::WritableDataBufferSP buffer_sp =
        std::make_shared<lldb_private::DataBufferHeap>(length, '\0');
    Status s;
    auto [len, was_truncated] = data_sp->ReadPointedString(buffer_sp, s, false);
    if (!s.Success())
      return s.takeError();

    assert(!was_truncated);

    // And construct an std::string with it.
    std::string out{(const char *)buffer_sp->GetBytes(), length};
    LLDB_LOG(GetLog(LLDBLog::Target),
             "read string \"{0}\" (len: {1}) from the inferior", out, length);
    return out;
  }

  llvm::Expected<std::string> GetIBIDFunction() {
    auto fn_name_sp = frame_info_sp->GetChildMemberWithName("function");
    if (!fn_name_sp) {
      return llvm::createStringError("no child member named function");
    }
    // Read the value from memory. That's the function.
    return ReadIBIDStringFromInferior(fn_name_sp.get());
  }

  llvm::Expected<LineEntry> GetIBIDLineInfo() {
    auto filename_sp = frame_info_sp->GetChildMemberWithName("filename");
    if (!filename_sp) {
      return llvm::createStringError("no child member named filename");
    }
    // Read the value from memory.
    auto string_or = ReadIBIDStringFromInferior(filename_sp.get());
    if (auto err = string_or.takeError())
      return err;

    auto line_sp = frame_info_sp->GetChildMemberWithName("line");
    if (!line_sp) {
      return llvm::createStringError("no child member named line");
    }
    auto col_sp = frame_info_sp->GetChildMemberWithName("column");
    if (!col_sp) {
      return llvm::createStringError("no child member named column");
    }

    LineEntry entry;
    entry.file_sp =
        std::make_shared<lldb_private::SupportFile>(FileSpec{*string_or});
    entry.line = line_sp->GetValueAsUnsigned(0);
    entry.column = col_sp->GetValueAsUnsigned(0);
    entry.synthetic = true;
    return entry;
  }

  llvm::Expected<lldb::ValueObjectSP> DoEvaluateExpression(std::string expr) {
    LLDB_LOG(GetLog(LLDBLog::Target), "evaluating expression `{0}`", expr);

    EvaluateExpressionOptions options;
    // Set a timeout so we don't wait for locks forever.
    options.SetTimeout(std::chrono::milliseconds{10});
    // Don't unwind on error and ignore breakpoints.
    options.SetUnwindOnError(false);
    options.SetIgnoreBreakpoints(true);
    // The language for these expressions is always going to be C.
    options.SetLanguage(lldb::eLanguageTypeC11);
    options.SetUseDynamic(lldb::eDynamicDontRunTarget);

    lldb::ValueObjectSP out;
    // Evaluate the expression in the context of the process. Hopefully that'll
    // choose a thread other than this current one(?)
    auto result =
        target_sp->EvaluateExpression(expr, frame_sp.get(), out, options);
    if (result != eExpressionCompleted) {
      return llvm::createStringError("expression `" + expr +
                                     "` failed: " + llvm::Twine(result));
    }

    return out;
  }

  llvm::Expected<std::vector<std::string>> GetIBIDLocalNames() {
    // NOTE: DO NOT use GetFrameIndex here - that causes an infinite recursive
    // loop where we attempt to construct frame providers/etc. Use m_frame_index
    // here instead.
    std::string expr = llvm::formatv("__ibid_get_frame_local_names({0})",
                                     m_frame_index - m_index_offset);

    auto locals_or = DoEvaluateExpression(expr);
    if (auto err = locals_or.takeError())
      return err;

    // Read the value from memory.
    auto string_or = ReadIBIDStringFromInferior(locals_or->get());
    if (auto err = string_or.takeError())
      return err;

    // Now, parse the string.
    auto json = llvm::json::parse(*string_or);
    if (auto err = json.takeError())
      return err;

    // Construct the list.
    std::vector<std::string> out;
    auto *arr = json->getAsArray();
    for (auto &v : *arr)
      out.emplace_back(v.getAsString().value_or(""));

    return out;
  }

  llvm::Expected<lldb::ValueObjectSP>
  EvaluateIBIDExpression(llvm::StringRef user_expr) {
    // Construct the expression to be executed.
    std::string expr =
        llvm::formatv("__ibid_evaluate_expression_in_frame({0}, \"{1}\")",
                      m_frame_index - m_index_offset, user_expr);

    return DoEvaluateExpression(expr);
  }

  /// Resolve all the requisite interpreted symbol information.
  void ResolveIBIDSymbolInfo() {
    // Resolve the line entry once for each instance of a stack frame. We don't
    // have to resolve it multiple times.
    if (!m_sc.line_entry.IsValid()) {
      auto lineinfo_or = GetIBIDLineInfo();
      if (auto err = lineinfo_or.takeError())
        LLDB_LOG_ERROR(GetLog(LLDBLog::Target), std::move(err),
                       "resolving line entry: {0}");
      else
        m_sc.line_entry = *lineinfo_or;
    }

    // Resolve the function name, but only once. It's always the same for any
    // given frame.
    if (m_function_name.empty()) {
      auto fn_or = GetIBIDFunction();
      if (auto err = fn_or.takeError())
        LLDB_LOG_ERROR(GetLog(LLDBLog::Target), std::move(err),
                       "resolving function name : {0}");
      else
        m_function_name = *fn_or;
    }
  }

  const char *GetFunctionName() override {
    // Resolve all the interpreted symbol information.
    ResolveIBIDSymbolInfo();
    return m_function_name.c_str();
  }

  const char *GetDisplayFunctionName() override { return GetFunctionName(); }

  void PopulateIBIDFrameLocals() {
    // This is *EXTREMELY* expensive, so don't do it often.
    if (m_frame_locals_sp->GetSize() != 0)
      return;

    auto local_names_or = GetIBIDLocalNames();
    if (auto err = local_names_or.takeError()) {
      LLDB_LOG_ERROR(GetLog(LLDBLog::Target), std::move(err),
                     "getting local variable names: {0}");
      return;
    }

    m_frame_locals_sp.reset(new ValueObjectList());
    for (llvm::StringRef name : *local_names_or) {
      auto result_or = EvaluateIBIDExpression(name);
      if (auto err = result_or.takeError()) {
        LLDB_LOG_ERROR(GetLog(LLDBLog::Target), std::move(err),
                       "getting value for local variable {1}: {0}", name);
        return;
      }
      // Set the name to the correct name.
      (*result_or)->SetName(ConstString{name});

      m_frame_locals_sp->Append(std::move(*result_or));
    }
  }

  void PopulateVariableList() {
    PopulateIBIDFrameLocals();
    if (!m_frame_locals_sp)
      return;

    // Reset the pointer - clear out whatever was in there already.
    m_variable_list_sp.reset(new VariableList());
    for (uint32_t i = 0, e = m_frame_locals_sp->GetSize(); i < e; ++i) {
      ValueObjectSP v = m_frame_locals_sp->GetValueObjectAtIndex(i);
      if (!v)
        continue;

      // Construct the value type as an synthetic verison of what the value
      // type is. That'll allow the user to tell the scope and the
      // 'synthetic-ness' of the variable.
      lldb::ValueType vt = GetSyntheticValueType(v->GetValueType());

      // Just make up a variable - the frame variable dumper just passes it
      // back in to GetValueObjectForFrameVariable, so we really just need to
      // make sure the name and type are correct.
      auto var = std::make_shared<lldb_private::Variable>(
          (lldb::user_id_t)i, v->GetName().GetCString(),
          v->GetName().GetCString(), nullptr, vt,
          /*owner_scope=*/nullptr,
          /*scope_range=*/Variable::RangeList{},
          /*decl=*/nullptr, DWARFExpressionList{}, /*external=*/false,
          /*artificial=*/true, /*location_is_constant_data=*/false);

      // Only append the variable if we have one (had already, or just created).
      if (var)
        m_variable_list_sp->AddVariable(var);
    }
  }

  VariableList *GetVariableList(bool get_file_globals,
                                bool include_synthetic_vars,
                                lldb_private::Status *error_ptr) override {
    if (!include_synthetic_vars) {
      return frame_sp->GetVariableList(get_file_globals, include_synthetic_vars,
                                       error_ptr);
    }

    PopulateVariableList();
    return m_variable_list_sp.get();
  }

  lldb::VariableListSP
  GetInScopeVariableList(bool get_file_globals, bool include_synthetic_vars,
                         bool must_have_valid_location = false) override {
    // The only type of variable we have is synthetic ones.
    if (!include_synthetic_vars) {
      return frame_sp->GetInScopeVariableList(
          get_file_globals, include_synthetic_vars, must_have_valid_location);
    }

    PopulateVariableList();
    return m_variable_list_sp;
  }

  lldb::ValueObjectSP
  GetValueObjectForFrameVariable(const lldb::VariableSP &variable_sp,
                                 lldb::DynamicValueType use_dynamic) override {
    // If we have no locals, then look to the underlying frame.
    if (m_frame_locals_sp->GetSize() == 0)
      return frame_sp->GetValueObjectForFrameVariable(variable_sp, use_dynamic);

    return m_frame_locals_sp->FindValueObjectByValueName(
        variable_sp->GetName().AsCString(nullptr));
  }

  lldb::ValueObjectSP FindVariable(ConstString name) override {
    // If we have no locals, then look to the underlying frame.
    if (m_frame_locals_sp->GetSize() == 0)
      return frame_sp->FindVariable(name);

    return m_frame_locals_sp->FindValueObjectByValueName(name.AsCString(nullptr));
  }

  lldb::ValueObjectSP GetValueForVariableExpressionPath(
      llvm::StringRef var_expr, lldb::DynamicValueType use_dynamic,
      uint32_t options, lldb::VariableSP &var_sp, Status &error, lldb::DILMode dilMode) override {
    LLDB_LOG(GetLog(LLDBLog::Target), "attempting to run expression {0}",
             var_expr);
    // Evaluate the user expression.
    auto result_or = EvaluateIBIDExpression(var_expr);
    if (auto err = result_or.takeError()) {
      error.FromError(std::move(err));
      return nullptr;
    }

    // Construct the value type as an synthetic verison of what the value
    // type is. That'll allow the user to tell the scope and the
    // 'synthetic-ness' of the variable.
    lldb::ValueType vt = GetSyntheticValueType((*result_or)->GetValueType());

    // Construct the variable and hand it back.
    var_sp = std::make_shared<lldb_private::Variable>(
        (lldb::user_id_t)(*result_or)->GetID(),
        (*result_or)->GetName().GetCString(),
        (*result_or)->GetName().GetCString(), nullptr, vt,
        /*owner_scope=*/nullptr,
        /*scope_range=*/Variable::RangeList{},
        /*decl=*/nullptr, DWARFExpressionList{}, /*external=*/false,
        /*artificial=*/true, /*location_is_constant_data=*/false);

    return *result_or;
  }

  lldb::TargetSP target_sp;
  lldb::StackFrameSP frame_sp;
  lldb::ProcessSP process_sp;

  lldb::ValueObjectSP frame_info_sp;

  std::string m_function_name = {};

  lldb::VariableListSP m_variable_list_sp = std::make_shared<VariableList>();
  lldb::ValueObjectListSP m_frame_locals_sp =
      std::make_shared<ValueObjectList>();

  uint32_t m_index_offset = 0;
};

// LLVM RTTI support.
char InterpretedFrame::ID;
} // namespace

llvm::Expected<lldb::SyntheticFrameProviderSP>
InterpretedFrameProvider::CreateInstance(lldb::StackFrameListSP input_frames,
                                         lldb::ModuleSP module_to_elide) {
  if (!input_frames)
    return llvm::createStringError(
        "failed to create interpreted frame provider: invalid input frames");

  return std::make_shared<InterpretedFrameProvider>(input_frames,
                                                    std::move(module_to_elide));
}

std::string InterpretedFrameProvider::GetDescription() const {
  // TODO
  return "interpreted frame provider";
}

InterpretedFrameProvider::InterpretedFrameProvider(
    lldb::StackFrameListSP input_frames, lldb::ModuleSP module_to_elide)
    : SyntheticFrameProvider(input_frames) {
  m_modules_to_elide.push_back(std::move(module_to_elide));
}

unsigned
InterpretedFrameProvider::GetNumInterpretedFrames(lldb::ProcessSP process_sp) {
  // Save the current number of interpreted frames per-stop. This works because
  // the provider is re-constructed at every stop point.
  if (m_num_interpreted_frames != 0)
    return m_num_interpreted_frames;

  // Get the __ibid_num_frames global variable.
  // TODO: This refuses to find any value but zero UNLESS we set the breakpoint
  // AFTER the process starts. If the process hasn't started, none of the
  // machinery works at all.
  auto &target = process_sp->GetTarget();
  // TODO: should maybe cache this search? Though if the loaded module list
  // changes that could be bad...
  VariableList variable_list;
  target.GetImages().FindGlobalVariables(ConstString("__ibid_num_frames"), 1,
                                         variable_list);
  if (variable_list.GetSize() != 1) {
    return 0;
  }

  ExecutionContextScope *exe_scope = process_sp.get();
  ValueObjectSP num_frames = ValueObjectVariable::Create(
      exe_scope, variable_list.GetVariableAtIndex(0));
  assert(num_frames);
  m_num_interpreted_frames = num_frames->GetValueAsUnsigned(0);
  LLDB_LOG(GetLog(LLDBLog::Target), "Found {0} interpreted frames",
           m_num_interpreted_frames);
  return m_num_interpreted_frames;
}

llvm::Expected<lldb::StackFrameSP>
InterpretedFrameProvider::GetFrameAtIndex(uint32_t idx) {
  // TODO: Part of the issue for Python is that all I have is the trace anchor.
  // If I had the anchor on the function that was wrapped around the call to an
  // instruction, then I'd be able to traceback from crashes and stuff in
  // extensions too.

  // Let's think this through. First step: I need to elide anything from the
  // python interpreter or the tracer bridge. Then, as a replacement for *those
  // frames* I need to return synthetic frames. The implementation should allow
  // me to check the ibid stuff anywhere inside the process, so that should be
  // fine.

  // Get the *concrete* frame at this index. This will call into the unwinder
  // (in theory). If the concrete frame here isn't in the interpreter,
  // return it. If it *is* in the interpreter, then we want to replace it
  // with the actual interpreted frames.
  auto frame_at_index_sp = m_input_frames->GetFrameAtIndex(idx);
  // If we have a frame here, check some things about it to see if we should
  // elide it or not.
  if (frame_at_index_sp) {
    // If it's an interpreted frame already, return it.
    if (llvm::isa<InterpretedFrame>(frame_at_index_sp.get()))
      return frame_at_index_sp;

    // Otherwise, get the symbol context so we can pull out the module. If the
    // module does not match the module we want to elide, return the frame.
    // TODO: This is not working - it's eliding just the modules from the bridge
    // extension but not the actual python frames...maybe a filter function
    // would work better than a single module and an equality check?
    const auto &frame_sc = frame_at_index_sp->GetSymbolContext(
        lldb::eSymbolContextModule | lldb::eSymbolContextFunction);
    if (frame_sc.module_sp) {
      //   if (llvm::StringRef(frame_sc.GetFunctionName())
      //           .contains("__ibid_debugger_trace_anchor")) {
      //     // If it's the anchor, we want to elide anything from that module.
      //     if (!llvm::is_contained(m_modules_to_elide, frame_sc.module_sp))
      //       m_modules_to_elide.push_back(frame_sc.module_sp);
      //   }
      //   // TODO: Why is it only eliding modules from the trace anchor module
      //   and not the base goddamn module that I passed in at the beginning?

      //   // If it's not one of the modules we want to elide, return the frame.
      //   if (!llvm::is_contained(m_modules_to_elide, frame_sc.module_sp))
      //     return frame_at_index_sp;

      // It is one of the modules/frames we want to elide.
    }
  } else {
    // If we can't get *anything* from m_input_frames, then use the zeroth frame
    // to figure out how many interpreted frames there are.
    frame_at_index_sp = m_input_frames->GetFrameWithConcreteFrameIndex(0);
  }

  // Still no frame, return an error (should never happen).
  if (!frame_at_index_sp)
    return llvm::createStringError("no frame at index 0?");

  // At this point we're going to elide the frame below. If we haven't
  // already set the index offset, set it. That index offset is the offset
  // of the first synthetic frame in the top-level frame list.
  if (m_index_offset == UINT32_MAX)
    m_index_offset = idx;

  // Now we have a concrete frame. Let's use that to produce the rest of the
  // frame info.

  ThreadSP thread_sp = GetThread().shared_from_this();
  ProcessSP process_sp = thread_sp->GetProcess();

  // We're in a frame, so figure out how many interpreted frames we even have.
  unsigned num_frames = GetNumInterpretedFrames(process_sp);

  // No interpreted frames, return the input frame.
  if (num_frames == 0)
    return frame_at_index_sp;

  // If we have some interpreted frames, make sure it's at an index we can
  // support.
  if (idx - m_index_offset >= num_frames)
    return llvm::createStringError("not enough interpreted frames (offset: " +
                                   llvm::Twine(m_index_offset) +
                                   ", num frames: " + llvm::Twine(num_frames) +
                                   ")");

  // Produce a fake frame. That frame will call into the inferior to
  // produce the function name, etc.
  TargetSP target_sp = process_sp->GetTarget().shared_from_this();

  VariableList variable_list;
  target_sp->GetImages().FindGlobalVariables(ConstString("__ibid_current_backtrace"), 1,
                                             variable_list);
  if (variable_list.GetSize() != 1) {
    return frame_at_index_sp;
  }

  ExecutionContextScope *exe_scope = process_sp.get();
  ValueObjectSP frame_list_sp = ValueObjectVariable::Create(
      exe_scope, variable_list.GetVariableAtIndex(0));
  auto type = frame_list_sp->GetCompilerType();
  // Get the sie of the pointee in bytes. We need that to compute the offset (in
  // bytes) to index into the frame list.
  auto pointee_size_or = type.GetPointeeType().GetByteSize(exe_scope);
  if (auto err = pointee_size_or.takeError())
    return err;

  ValueObjectSP frame_info_sp = frame_list_sp->GetSyntheticChildAtOffset(
      idx * *pointee_size_or, type.GetPointeeType(), /*can_create=*/true);

  const lldb::addr_t cfa = LLDB_INVALID_ADDRESS;
  const bool cfa_is_valid = false;
  // It *is* artificial, but I guess in the english sense not the LLDB sense.
  const bool artificial = false;
  const bool behaves_like_zeroth_frame = false;

  // Provide a basic symbol context.
  SymbolContext sc;
  sc.target_sp = target_sp;

  // Set up the interpreted frame using the StackFrame constructor.
  auto interpreted_frame = std::make_shared<InterpretedFrame>(
      thread_sp, idx, idx, cfa, cfa_is_valid, LLDB_INVALID_ADDRESS,
      StackFrame::Kind::Synthetic, artificial, behaves_like_zeroth_frame, &sc);

  // Then populate the various stuff we need to call into the inferior to
  // get information *about* the frame.
  interpreted_frame->frame_info_sp = frame_info_sp;
  interpreted_frame->target_sp = target_sp;
  interpreted_frame->process_sp = process_sp;
  interpreted_frame->frame_sp = frame_at_index_sp;
  interpreted_frame->m_index_offset = m_index_offset;
  return interpreted_frame;
}
