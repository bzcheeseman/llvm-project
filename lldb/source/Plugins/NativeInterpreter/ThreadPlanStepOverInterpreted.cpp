//===-- ThreadPlanStepOverInterpreted.cpp ---------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ThreadPlanStepOverInterpreted.h"

#include "lldb/Breakpoint/BreakpointSite.h"
#include "lldb/Expression/UserExpression.h"
#include "lldb/Symbol/VariableList.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/RegisterContext.h"
#include "lldb/Target/StopInfo.h"
#include "lldb/Target/Target.h"
#include "lldb/Target/Thread.h"
#include "lldb/Utility/Status.h"
#include "lldb/Utility/Stream.h"
#include "lldb/ValueObject/ValueObjectVariable.h"

using namespace lldb;
using namespace lldb_private;

ThreadPlanStepOverInterpreted::ThreadPlanStepOverInterpreted(Thread &thread)
    : ThreadPlan(ThreadPlan::eKindGeneric, "step over interpreted frame",
                 thread, eVoteYes, eVoteNoOpinion) {
  ProcessSP process_sp = thread.GetProcess();
  if (!process_sp)
    return;

  Target &target = process_sp->GetTarget();

  // Find __ibid_step_hit to know where to break.
  SymbolContextList sc_list;
  target.GetImages().FindSymbolsWithNameAndType(ConstString("__ibid_step_hit"),
                                                eSymbolTypeAny, sc_list);
  if (sc_list.GetSize() == 0)
    return;

  SymbolContext sc;
  sc_list.GetContextAtIndex(0, sc);
  if (!sc.symbol)
    return;

  m_step_hit_addr = sc.symbol->GetAddress().GetLoadAddress(&target);
  if (m_step_hit_addr == LLDB_INVALID_ADDRESS)
    return;

  // Create an internal breakpoint so the process will stop at __ibid_step_hit.
  BreakpointSP bp_sp =
      target.CreateBreakpoint(m_step_hit_addr, /*internal=*/true,
                               /*hardware=*/false);
  if (!bp_sp)
    return;

  m_step_hit_bp_id = bp_sp->GetID();

  // Arm the step-over now, while the process is stopped and it is safe to
  // evaluate expressions. DoWillResume is called during Process::Resume()
  // while the run-lock is held, so expression evaluation is not safe there.
  if (ArmStepOver())
    m_valid = true;
}

ThreadPlanStepOverInterpreted::~ThreadPlanStepOverInterpreted() = default;

void ThreadPlanStepOverInterpreted::GetDescription(Stream *s,
                                                   DescriptionLevel level) {
  s->PutCString("step over");
}

bool ThreadPlanStepOverInterpreted::ValidatePlan(Stream *error) {
  if (!m_valid) {
    if (error)
      error->PutCString(
          "ThreadPlanStepOverInterpreted: could not find __ibid_step_hit");
    return false;
  }
  return true;
}

bool ThreadPlanStepOverInterpreted::ArmStepOver() {
  StackFrameSP frame_sp = GetThread().GetStackFrameAtIndex(0);
  if (!frame_sp)
    return false;

  Target &target = GetThread().GetProcess()->GetTarget();

  EvaluateExpressionOptions options;
  options.SetLanguage(eLanguageTypeC_plus_plus);
  options.SetUnwindOnError(true);
  options.SetIgnoreBreakpoints(true);

  ValueObjectSP result_sp;
  ExpressionResults result = target.EvaluateExpression(
      "__ibid_arm_step_over_current()", frame_sp.get(), result_sp, options);
  return result == eExpressionCompleted;
}

bool ThreadPlanStepOverInterpreted::DoWillResume(StateType resume_state,
                                                  bool current_plan) {
  return true;
}

bool ThreadPlanStepOverInterpreted::AtStepHit() {
  if (m_step_hit_bp_id == LLDB_INVALID_BREAK_ID)
    return false;

  StopInfoSP stop_info = GetThread().GetStopInfo();
  if (!stop_info || stop_info->GetStopReason() != eStopReasonBreakpoint)
    return false;

  auto site_id = static_cast<break_id_t>(stop_info->GetValue());
  BreakpointSiteSP site =
      GetThread().GetProcess()->GetBreakpointSiteList().FindByID(site_id);
  return site && site->IsBreakpointAtThisSite(m_step_hit_bp_id);
}

bool ThreadPlanStepOverInterpreted::DoPlanExplainsStop(Event *event) {
  return AtStepHit();
}

bool ThreadPlanStepOverInterpreted::ShouldStop(Event *event) {
  if (AtStepHit()) {
    SetPlanComplete(true);
    return true;
  }
  return false;
}

bool ThreadPlanStepOverInterpreted::MischiefManaged() {
  return IsPlanComplete();
}

void ThreadPlanStepOverInterpreted::DidPop() {
  if (m_step_hit_bp_id != LLDB_INVALID_BREAK_ID) {
    GetThread().GetProcess()->GetTarget().RemoveBreakpointByID(m_step_hit_bp_id);
    m_step_hit_bp_id = LLDB_INVALID_BREAK_ID;
  }
}
