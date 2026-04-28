//===-- ThreadPlanStepOverInterpreted.h -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "lldb/Target/ThreadPlan.h"
#include "lldb/lldb-private.h"

namespace lldb_private {

/// Thread plan that steps over one source line in an interpreted (Python)
/// frame. It arms the IBID bridge's step-over state, creates an internal
/// breakpoint at __ibid_step_hit, and completes when that breakpoint fires
/// (meaning the bridge detected a line change at the original call depth or
/// shallower).
class ThreadPlanStepOverInterpreted : public ThreadPlan {
public:
  ThreadPlanStepOverInterpreted(Thread &thread);
  ~ThreadPlanStepOverInterpreted() override;

  void GetDescription(Stream *s, lldb::DescriptionLevel level) override;
  bool ValidatePlan(Stream *error) override;
  bool ShouldStop(Event *event) override;
  bool WillStop() override { return true; }
  bool StopOthers() override { return true; }
  lldb::StateType GetPlanRunState() override { return lldb::eStateRunning; }
  bool DoWillResume(lldb::StateType resume_state, bool current_plan) override;
  bool MischiefManaged() override;
  void DidPop() override;

protected:
  bool DoPlanExplainsStop(Event *event) override;

private:
  bool AtStepHit();
  bool ArmStepOver();

  lldb::addr_t m_step_hit_addr = LLDB_INVALID_ADDRESS;
  lldb::break_id_t m_step_hit_bp_id = LLDB_INVALID_BREAK_ID;
  bool m_valid = false;
};

} // namespace lldb_private
