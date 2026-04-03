//===-- FrameProvider.h ---------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Target/SyntheticFrameProvider.h"
#include "lldb/lldb-forward.h"

#include "llvm/ADT/DenseSet.h"

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

private:
  /// Get the number of python frames.
  unsigned GetNumInterpretedFrames(lldb::StackFrameSP anchor_frame);

  /// The name of the module to elide. Any functions in this module will be
  /// elided.
  lldb::ModuleSP m_module_to_elide;

  /// Frame index offset - we store the index of the last elided frame here so
  /// that we can start providing the interpreted frames at the next index.
  uint32_t m_index_offset = 0;
  // Save the current number of interpreted frames per-stop. This works because
  // the provider is re-constructed at every stop point.
  uint32_t m_num_interpreted_frames = 0;
};
} // namespace lldb_private
