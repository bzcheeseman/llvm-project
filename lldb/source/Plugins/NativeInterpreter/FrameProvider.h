//===-- FrameProvider.h ---------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Target/SyntheticFrameProvider.h"
#include "lldb/lldb-forward.h"

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

  /// Create an instance of the python frame provider.
  static llvm::Expected<lldb::SyntheticFrameProviderSP>
  CreateInstance(lldb::StackFrameListSP input_frames,
                 lldb::ModuleSP module_to_elide);

  std::string GetDescription() const override;

  llvm::Expected<lldb::StackFrameSP> GetFrameAtIndex(uint32_t idx) override;

  /// Consume the stale-frame reset signal. Returns true (and clears the flag)
  /// when the frame list should be cleared and rebuilt from index 0 — this
  /// happens the first time Python frames are discovered after a concrete C
  /// frame was already cached at an earlier index.
  bool ShouldReset() override {
    bool r = m_should_reset;
    m_should_reset = false;
    return r;
  }

private:
  /// Get the number of python frames.
  unsigned GetNumInterpretedFrames(lldb::ProcessSP process_sp);

  /// Frame index offset: always 0 once Python frames are available, meaning
  /// synthetic frame i is returned for provider index i.
  uint32_t m_index_offset = UINT32_MAX;

  // Save the current number of interpreted frames per-stop. This works because
  // the provider is re-constructed at every stop point.
  uint32_t m_num_interpreted_frames = 0;

  // Set when Python frames are first discovered at an index > 0, indicating
  // that stale concrete frames cached earlier must be discarded.
  bool m_should_reset = false;
};
} // namespace lldb_private
