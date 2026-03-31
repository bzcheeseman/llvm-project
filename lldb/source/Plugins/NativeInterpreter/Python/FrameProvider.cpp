//===-- FrameProvider.cpp -------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Plugins/NativeInterpreter/Python/FrameProvider.h"
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
#include "lldb/ValueObject/ValueObjectList.h"
#include "lldb/ValueObject/ValueObjectVariable.h"
#include "lldb/lldb-enumerations.h"
#include "lldb/lldb-forward.h"
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
class PythonFrame : public StackFrame {
public:
  using StackFrame::StackFrame;

  static char ID;
  bool isA(const void *ClassID) const override {
    return ClassID == &ID || StackFrame::isA(ClassID);
  }
  static bool classof(const StackFrame *obj) { return obj->isA(&ID); }

  // TODO: I would prefer to overload GetSymbolContext to provide the line info.
  // However, I can't do that because that is called from the expression
  // evaluator, which in turn causes deadlocks. I tried the version that just
  // reads globals, but for some reason that worked very poorly.

  llvm::Expected<std::string>
  ReadStringFromInferior(lldb_private::ValueObject *valobj) {
    // nullptr isn't necessarily an error - just empty string.
    if (valobj->GetValueAsUnsigned(0xcafe) == 0) {
      return "";
    }

    // Construct a memory buffer.
    lldb::WritableDataBufferSP buffer_sp =
        std::make_shared<lldb_private::DataBufferHeap>(128, '\0');
    Status s;
    auto [len, was_truncated] = valobj->ReadPointedString(buffer_sp, s, false);
    if (!s.Success())
      return s.takeError();

    if (was_truncated) {
      // Make a larger memory buffer and try again if the string was truncated.
      buffer_sp = std::make_shared<lldb_private::DataBufferHeap>(1024, '\0');
      auto [upd_len, _] = valobj->ReadPointedString(buffer_sp, s, false);
      if (!s.Success())
        return s.takeError();
      len = upd_len;
    }

    // And construct a string with it. We have to get the length via strlen (we
    // know it's null-terminated) lest it contain garbage at the end.
    size_t string_length = strnlen((const char *)buffer_sp->GetBytes(), len);
    std::string out{(const char *)buffer_sp->GetBytes(), string_length};
    LLDB_LOG(GetLog(LLDBLog::Target), "read string \"{0}\" from the inferior",
             out);
    return out;
  }

  enum IBIDCallee { eFrameFunction, eFrameLineInfo, eFrameLocalNames };

  /// Evaluate the provided IBID callee in the inferior. The IBID callee should
  /// be of the form `__ibid_<...>`
  llvm::Expected<lldb::ValueObjectSP> EvaluateIBIDCallee(IBIDCallee callee) {
    EvaluateExpressionOptions options;
    // Set a timeout so we don't wait for locks forever.
    options.SetTimeout(std::chrono::microseconds{1000});
    // Don't unwind on error and ignore breakpoints.
    options.SetUnwindOnError(false);
    options.SetIgnoreBreakpoints(true);
    // The language for these expressions is always going to be C.
    options.SetLanguage(lldb::eLanguageTypeC11);
    // And we specifically don't want to run the target if we can avoid it.
    options.SetUseDynamic(lldb::eDynamicDontRunTarget);

    lldb::ValueObjectSP frame_info;
    // NOTE: DO NOT use GetFrameIndex here - that causes an infinite recursive
    // loop where we attempt to construct frame providers/etc. Use m_frame_index
    // here instead.
    std::string expr;
    switch (callee) {
    case eFrameFunction: {
      expr = llvm::formatv("__ibid_get_frame_function({0})",
                           m_frame_index - m_index_offset)
                 .str();
      break;
    }
    case eFrameLineInfo: {
      expr = llvm::formatv("__ibid_get_frame_line_info({0})",
                           m_frame_index - m_index_offset)
                 .str();
      break;
    }
    case eFrameLocalNames: {
      expr = llvm::formatv("__ibid_get_frame_local_names({0})",
                           m_frame_index - m_index_offset)
                 .str();
      break;
    }
    }

    LLDB_LOG(GetLog(LLDBLog::Target), "evaluating expression `{0}`", expr);

    // Evaluate the expression in the context of the process. Hopefully that'll
    // choose a thread other than this current one(?)
    auto result = target_sp->EvaluateExpression(expr, frame_sp.get(),
                                                frame_info, options);
    if (result != eExpressionCompleted) {
      return llvm::createStringError("expression `" + expr +
                                     "` failed: " + llvm::Twine(result));
    }
    return frame_info;
  }

  llvm::Expected<std::string> GetPythonFunction() {
    auto frame_info_or = EvaluateIBIDCallee(eFrameFunction);
    if (auto err = frame_info_or.takeError())
      return std::move(err);

    // Read the value from memory. That's the function.
    return ReadStringFromInferior(frame_info_or->get());
  }

  llvm::Expected<LineEntry> GetPythonLineInfo() {
    auto frame_info_or = EvaluateIBIDCallee(eFrameLineInfo);
    if (auto err = frame_info_or.takeError())
      return std::move(err);

    // Read the value from memory.
    auto string_or = ReadStringFromInferior(frame_info_or->get());
    if (auto err = string_or.takeError())
      return err;

    // Now, parse the string.
    auto json = llvm::json::parse(*string_or);
    if (auto err = json.takeError())
      return err;

    auto *obj = json->getAsObject();
    auto filename = obj->getString("filename");
    auto line = obj->getInteger("line");
    auto col = obj->getInteger("col");

    LineEntry entry;
    entry.file_sp = std::make_shared<lldb_private::SupportFile>(
        FileSpec{filename.value_or("unknown")});
    entry.line = line.value_or(0);
    entry.column = col.value_or(0);
    entry.synthetic = true;
    return entry;
  }

  llvm::Expected<std::vector<std::string>> GetPythonLocalNames() {
    auto locals_or = EvaluateIBIDCallee(eFrameLocalNames);
    if (auto err = locals_or.takeError())
      return std::move(err);

    // Read the value from memory.
    auto string_or = ReadStringFromInferior(locals_or->get());
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
  EvaluatePythonExpression(llvm::StringRef user_expr) {
    EvaluateExpressionOptions options;
    // Set a timeout so we don't wait for locks forever.
    options.SetTimeout(std::chrono::microseconds{1000});
    // Don't unwind on error and ignore breakpoints.
    options.SetUnwindOnError(false);
    options.SetIgnoreBreakpoints(true);
    // The language for these expressions is always going to be C.
    options.SetLanguage(lldb::eLanguageTypeC11);
    // And we specifically don't want to run the target if we can avoid it.
    options.SetUseDynamic(lldb::eDynamicCanRunTarget);

    // Construct the expression to be executed.
    std::string expr =
        llvm::formatv("__ibid_evaluate_expression_in_frame({0}, \"{1}\")",
                      m_frame_index - m_index_offset, user_expr);

    LLDB_LOG(GetLog(LLDBLog::Target), "evaluating expression `{0}`", expr);

    lldb::ValueObjectSP result;
    auto expr_result =
        target_sp->EvaluateExpression(expr, frame_sp.get(), result, options);
    if (expr_result != eExpressionCompleted) {
      return llvm::createStringError("expression `" + expr +
                                     "` failed: " + llvm::Twine(expr_result));
    }
    return result;
  }

  /// Resolve all the requisite python symbol information.
  void ResolvePythonSymbolInfo() {
    // Resolve the line entry once for each instance of a stack frame. We don't
    // have to resolve it multiple times.
    if (!m_sc.line_entry.IsValid()) {
      auto lineinfo_or = GetPythonLineInfo();
      if (auto err = lineinfo_or.takeError())
        LLDB_LOG_ERROR(GetLog(LLDBLog::Target), std::move(err),
                       "resolving line entry: {0}");
      else
        m_sc.line_entry = *lineinfo_or;
    }

    // Resolve the function name, but only once. It's always the same for any
    // given frame.
    if (m_function_name.empty()) {
      auto fn_or = GetPythonFunction();
      if (auto err = fn_or.takeError())
        LLDB_LOG_ERROR(GetLog(LLDBLog::Target), std::move(err),
                       "resolving function name : {0}");
      else
        m_function_name = *fn_or;
    }
  }

  const char *GetFunctionName() override {
    // Resolve all the python symbol information.
    ResolvePythonSymbolInfo();
    return m_function_name.c_str();
  }

  const char *GetDisplayFunctionName() override { return GetFunctionName(); }

  void PopulatePythonFrameLocals() {
    // This is *EXTREMELY* expensive, so don't do it often.
    if (m_frame_locals_sp->GetSize() != 0)
      return;

    auto local_names_or = GetPythonLocalNames();
    if (auto err = local_names_or.takeError()) {
      LLDB_LOG_ERROR(GetLog(LLDBLog::Target), std::move(err),
                     "getting local variable names: {0}");
      return;
    }

    m_frame_locals_sp.reset(new ValueObjectList());
    for (llvm::StringRef name : *local_names_or) {
      auto result_or = EvaluatePythonExpression(name);
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
    PopulatePythonFrameLocals();
    if (!m_frame_locals_sp)
      return;

    // Reset the pointer - clear out whatever was in there already.
    m_variable_list_sp.reset(new VariableList());
    for (uint32_t i = 0, e = m_frame_locals_sp->GetSize(); i < e; ++i) {
      ValueObjectSP v = m_frame_locals_sp->GetValueObjectAtIndex(i);
      if (!v)
        continue;

      // Construct the value type as an extended verison of what the value
      // type is. That'll allow the user to tell the scope and the
      // 'extended-ness' of the variable.
      lldb::ValueType vt =
          lldb::ValueType(v->GetValueType() & ValueTypeExtendedMask);

      // Just make up a variable - the frame variable dumper just passes it
      // back in to GetValueObjectForFrameVariable, so we really just need to
      // make sure the name and type are correct.
      auto var = std::make_shared<lldb_private::Variable>(
          (lldb::user_id_t)v->GetID() + i, v->GetName().GetCString(),
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
                                bool include_extended_vars,
                                lldb_private::Status *error_ptr) override {
    if (!include_extended_vars) {
      return frame_sp->GetVariableList(get_file_globals, include_extended_vars,
                                       error_ptr);
    }

    PopulateVariableList();
    return m_variable_list_sp.get();
  }

  lldb::VariableListSP
  GetInScopeVariableList(bool get_file_globals, bool include_extended_vars,
                         bool must_have_valid_location = false) override {
    // The only type of variable we have is extended ones.
    if (!include_extended_vars) {
      return frame_sp->GetInScopeVariableList(
          get_file_globals, include_extended_vars, must_have_valid_location);
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
        variable_sp->GetName().AsCString());
  }

  lldb::ValueObjectSP FindVariable(ConstString name) override {
    // If we have no locals, then look to the underlying frame.
    if (m_frame_locals_sp->GetSize() == 0)
      return frame_sp->FindVariable(name);

    return m_frame_locals_sp->FindValueObjectByValueName(name.AsCString());
  }

  lldb::ValueObjectSP GetValueForVariableExpressionPath(
      llvm::StringRef var_expr, lldb::DynamicValueType use_dynamic,
      uint32_t options, lldb::VariableSP &var_sp, Status &error) override {
    LLDB_LOG(GetLog(LLDBLog::Target), "attempting to run expression {0}",
             var_expr);
    // Evaluate the user expression.
    auto result_or = EvaluatePythonExpression(var_expr);
    if (auto err = result_or.takeError()) {
      error.FromError(std::move(err));
      return nullptr;
    }

    // Construct the value type as an extended verison of what the value
    // type is. That'll allow the user to tell the scope and the
    // 'extended-ness' of the variable.
    lldb::ValueType vt =
        lldb::ValueType((*result_or)->GetValueType() & ValueTypeExtendedMask);

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

  std::string m_function_name = {};

  lldb::VariableListSP m_variable_list_sp = std::make_shared<VariableList>();
  lldb::ValueObjectListSP m_frame_locals_sp =
      std::make_shared<ValueObjectList>();

  uint32_t m_index_offset = 0;
};

// LLVM RTTI support.
char PythonFrame::ID;
} // namespace

llvm::Expected<lldb::SyntheticFrameProviderSP>
PythonFrameProvider::CreateInstance(lldb::StackFrameListSP input_frames) {
  if (!input_frames)
    return llvm::createStringError(
        "failed to create python frame provider: invalid input frames");

  return std::make_shared<PythonFrameProvider>(input_frames);
}

std::string PythonFrameProvider::GetDescription() const {
  // TODO
  return "python frame provider";
}

PythonFrameProvider::PythonFrameProvider(lldb::StackFrameListSP input_frames)
    : SyntheticFrameProvider(input_frames) {
  auto &target = GetThread().GetProcess()->GetTarget();
  (void)target;

  // TODO: Create the __ibid_debugger_anchor breakpoint and set it to
  //       auto-continue/etc so we can grab the frames and then move on. Or
  //       should I do that in the plugin?
}

unsigned
PythonFrameProvider::GetNumPythonFrames(lldb::StackFrameSP anchor_frame) {
  // Save the current number of python frames per-stop. This works because the
  // provider is re-constructed at every stop point.
  if (m_num_python_frames != 0)
    return m_num_python_frames;

  lldb::VariableSP var;
  Status err;
  // Make sure to call the *base* stack frame method. We use eNoDynamicValues
  // because it looks like if we allow dynamic values it causes us to construct
  // an execution context, which re-constructs the frame list. That said, looks
  // like eNoDynamicValues does the same thing...
  auto val_sp = anchor_frame->StackFrame::GetValueForVariableExpressionPath(
      "__ibid_num_frames", lldb::eNoDynamicValues, 0, var, err);
  assert(val_sp);
  m_num_python_frames = val_sp->GetValueAsUnsigned(0);
  LLDB_LOG(GetLog(LLDBLog::Target), "Found {0} python frames",
           m_num_python_frames);
  return m_num_python_frames;
}

llvm::Expected<lldb::StackFrameSP>
PythonFrameProvider::GetFrameAtIndex(uint32_t idx) {
  // Get the *concrete* frame at this index. This will call into the unwinder
  // (in theory). If the concrete frame here isn't in the python interpreter,
  // return it. If it *is* in the python interpreter, then we want to replace it
  // with the actual python frames.
  auto frame_at_index_sp = m_input_frames->GetFrameAtIndex(idx);
  if (frame_at_index_sp && !llvm::isa<PythonFrame>(frame_at_index_sp.get())) {
    auto &frame_sc = frame_at_index_sp->GetSymbolContext(
        lldb::eSymbolContextModule | lldb::eSymbolContextFunction);
    // If this is the debugger anchor function, populate the number of python
    // frames from it.
    if (llvm::StringRef(frame_sc.GetFunctionName())
            .contains("__ibid_debugger_anchor")) {
      (void)GetNumPythonFrames(frame_at_index_sp);
    }
    // Then, decide if the frame should be elided.
    if (frame_sc.module_sp) {
      auto file = frame_sc.module_sp->GetObjectFile()->GetFileSpec();
      llvm::StringRef file_path{file.GetPathAsConstString()};
      // This only works because we pre-populate (and then save) the number of
      // python frames at the anchor frame. If we didn't do that, we'd call
      // GetNumPythonFrames from within this function (from within
      // GetFrameWithStackID). The issue is that GetFrameWithStackID isn't
      // really recursion-safe. We're trying to restore the thread state after
      // the expression ran, and that's causing headaches because that calls
      // GetFrameWithStackID, which calls FetchFramesUpTo, which calls this
      // function (and) GetNumPythonFrames, which goes into ValueObject ->
      // ExecutionContext -> GetFrameWithStackID and we hit a deadlock because
      // FetchFramesUpTo holds a write lock that GetFrameWithStackID tries to
      // take. Basically, we have to avoid re-constructing the frame list from
      // within this function if we're running an expression in one of the
      // frames.
      if (!(file_path.ends_with("Python"))) {
        m_index_offset = idx + 1;
        return frame_at_index_sp;
      }
    }
  } else {
    // If we can't get *anything* from this, then use the zeroth frame to figure
    // out how many python frames there are.
    frame_at_index_sp = m_input_frames->GetFrameWithConcreteFrameIndex(0);
  }

  // Still no frame, return an error (should never happen).
  if (!frame_at_index_sp)
    return llvm::createStringError("no frame at index 0?");

  // Now we have the concrete frame. Let's use that to produce the rest of the
  // frame info.

  // Now, we're in the anchor so we can figure out how many python frames
  // there actually *are*.
  unsigned num_frames = GetNumPythonFrames(frame_at_index_sp);
  if (idx - m_index_offset >= num_frames)
    return llvm::createStringError("");

  // Produce a fake frame. That frame will call into the inferior to
  // produce the function name, etc.
  ThreadSP thread_sp = GetThread().shared_from_this();
  ProcessSP process_sp = thread_sp->GetProcess();
  TargetSP target_sp = process_sp->GetTarget().shared_from_this();

  const lldb::addr_t cfa = LLDB_INVALID_ADDRESS;
  const bool cfa_is_valid = false;
  const bool artificial = false; // ??? It *is* artificial?
  const bool behaves_like_zeroth_frame = false;

  // Provide a basic symbol context.
  SymbolContext sc;
  sc.target_sp = target_sp;

  // Set up the python frame using the StackFrame constructor.
  auto py_frame = std::make_shared<PythonFrame>(
      thread_sp, idx, idx, cfa, cfa_is_valid, LLDB_INVALID_ADDRESS,
      StackFrame::Kind::Synthetic, artificial, behaves_like_zeroth_frame, &sc);

  // Then populate the various stuff we need to call into the inferior to
  // get information *about* the frame.
  py_frame->target_sp = target_sp;
  py_frame->process_sp = process_sp;
  py_frame->frame_sp = frame_at_index_sp;
  py_frame->m_index_offset = m_index_offset;
  return py_frame;
}
