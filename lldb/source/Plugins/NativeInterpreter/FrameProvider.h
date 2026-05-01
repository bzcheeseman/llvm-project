//===-- FrameProvider.h ---------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Target/SyntheticFrameProvider.h"
#include "lldb/lldb-forward.h"

#include <string>

namespace lldb_private {
class InterpretedFrameProvider : public SyntheticFrameProvider {
public:
  InterpretedFrameProvider(lldb::StackFrameListSP input_frames,
                           lldb::ModuleSP module_to_elide);
  ~InterpretedFrameProvider() override = default;

  // This isn't a plugin, but we provide these anyway
  llvm::StringRef GetPluginName() override {
    return "InterpretedFrameProvider";
  }
  static llvm::StringRef GetPluginNameStatic() {
    return "InterpretedFrameProvider";
  }

  /// Create an instance of the interpreter frame provider.
  static llvm::Expected<lldb::SyntheticFrameProviderSP>
  CreateInstance(lldb::StackFrameListSP input_frames,
                 lldb::ModuleSP module_to_elide);

  std::string GetDescription() const override;

  llvm::Expected<lldb::StackFrameSP> GetFrameAtIndex(uint32_t idx) override;

  /// Consume the stale-frame reset signal. Returns true (and clears the flag)
  /// when the frame list should be cleared and rebuilt from index 0 — this
  /// happens the first time interpreter frames are discovered after a concrete C
  /// frame was already cached at an earlier index.
  bool ShouldReset() override {
    bool r = m_should_reset;
    m_should_reset = false;
    return r;
  }

private:
  /// Get the number of interpreter frames.
  unsigned GetNumInterpretedFrames(lldb::ProcessSP process_sp);

  /// Return the concrete native-frame index of the n-th non-elided frame.
  /// Walks all concrete frames, skips those whose function name matches
  /// __ibid_function_elision_regex (interpreter engine internals), and returns
  /// the index of the n-th surviving frame. Returns UINT32_MAX when the stack
  /// is exhausted before reaching n.
  uint32_t GetNonElidedNativeFrameIdx(lldb::ProcessSP process_sp, uint32_t n);

  /// Frame index offset: always 0 once interpreter frames are available, meaning
  /// synthetic frame i is returned for provider index i.
  uint32_t m_index_offset = UINT32_MAX;

  // Save the current number of interpreted frames per-stop. This works because
  // the provider is re-constructed at every stop point.
  uint32_t m_num_interpreted_frames = 0;

  // Set when interpreter frames are first discovered at an index > 0, indicating
  // that stale concrete frames cached earlier must be discarded.
  bool m_should_reset = false;

  // Elision regex string read from __ibid_function_elision_regex in the
  // inferior. Keyed by stop ID: when the process advances to a new stop the
  // cached value is evicted and re-read.
  std::string m_elision_regex;
  uint32_t m_elision_regex_stop_id = UINT32_MAX;
};
} // namespace lldb_private
