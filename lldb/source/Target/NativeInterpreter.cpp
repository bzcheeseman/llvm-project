//===-- NativeInterpreter.cpp ---------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Target/NativeInterpreter.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/lldb-forward.h"

using namespace lldb;
using namespace lldb_private;

lldb::NativeInterpreterSP
NativeInterpreter::CreateInstance(lldb::ModuleSP module_to_elide) {
  // Just get the native interpreter plugin for a given name. We prefix with the
  // lowercase interpreter name to make the plugins easy to discover.
  auto callbacks =
      PluginManager::GetNativeInterpreterCreateCallbacks();
  return callbacks.empty() ? callbacks[0](module_to_elide) : nullptr;
}
