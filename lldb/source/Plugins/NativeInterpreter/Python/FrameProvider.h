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
class PythonFrameProvider : public SyntheticFrameProvider {
public:
  PythonFrameProvider(lldb::StackFrameListSP input_frames);
  ~PythonFrameProvider() override = default;

  // This isn't a plugin, but we provide these anyway
  llvm::StringRef GetPluginName() override { return "PythonFrameProvider"; }
  static llvm::StringRef GetPluginNameStatic() { return "PythonFrameProvider"; }

  /// Create an instance of the python frame provider.
  static llvm::Expected<lldb::SyntheticFrameProviderSP>
  CreateInstance(lldb::StackFrameListSP input_frames);

  std::string GetDescription() const override;

  llvm::Expected<lldb::StackFrameSP> GetFrameAtIndex(uint32_t idx) override;

private:
  /// Get the number of python frames.
  unsigned GetNumPythonFrames(lldb::StackFrameSP anchor_frame);

  uint32_t m_index_offset = 0;
  // Save the current number of python frames per-stop. This works because the
  // provider is re-constructed at every stop point.
  uint32_t m_num_python_frames = 0;
};
} // namespace lldb_private
