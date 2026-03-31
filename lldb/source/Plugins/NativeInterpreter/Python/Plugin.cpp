#include "lldb/Target/NativeInterpreter.h"

#include "FrameProvider.h"
// #include "BreakpointResolver.h"

#include "lldb/Core/Module.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/Symbol/Function.h"
#include "lldb/Target/Target.h"
#include "lldb/Utility/LLDBLog.h"
#include "lldb/Utility/Log.h"
#include "lldb/lldb-enumerations.h"
#include "lldb/lldb-forward.h"
#include "llvm/Support/Error.h"

using namespace lldb;
using namespace lldb_private;

namespace {
class PythonInterpreterPlugin : public NativeInterpreter {
public:
  static void Initialize() {
    PluginManager::RegisterPlugin(GetPluginNameStatic(),
                                  "plugin to handle python debugging",
                                  PythonInterpreterPlugin::CreateInstance);
  }
  static void Terminate() {
    PluginManager::UnregisterPlugin(PythonInterpreterPlugin::CreateInstance);
  }

  static llvm::StringRef GetPluginNameStatic() {
    return "python-NativeInterpreter";
  }

  llvm::StringRef GetPluginName() override {
    return "python-NativeInterpreter";
  }

  // This plugin is actually really simple - just construct one and pass it out.
  static NativeInterpreterSP CreateInstance() {
    return std::make_shared<PythonInterpreterPlugin>();
  }

  // Construct a frame provider.
  lldb::SyntheticFrameProviderSP
  GetFrameProvider(lldb::StackFrameListSP frame_list) override {
    auto frame_provider_sp_or =
        PythonFrameProvider::CreateInstance(std::move(frame_list));
    if (auto err = frame_provider_sp_or.takeError()) {
      LLDB_LOG_ERROR(GetLog(LLDBLog::Platform), std::move(err), "{0}");
      return nullptr;
    }
    return *frame_provider_sp_or;
  }

  lldb::BreakpointSP
  CreateAnchorBreakpoint(lldb_private::Target &target,
                         lldb_private::BreakpointResolver &resolver) override {
    auto module_sp = target.GetExecutableModule();

    const auto *symbol = module_sp->FindFirstSymbolWithNameAndType(
        ConstString{"__ibid_debugger_anchor"}, lldb::eSymbolTypeCode);
    if (!symbol)
      return nullptr;

    // This works (and we don't need to skip the prologue) because we don't
    // actually need anything *inside* the function - we can just break as soon
    // as we hit it.
    return target.CreateBreakpoint(symbol->GetAddress(), true, false);
  }

  lldb::BreakpointResolverSP
  GetBreakpointResolver(lldb::ThreadSP thread) override {
    return nullptr;
  }
};
} // namespace

// Forward the native interpreter plugin to the python interpreter plugin.
namespace lldb_private {
void lldb_initialize_NativeInterpreter() {
  PythonInterpreterPlugin::Initialize();
}

void lldb_terminate_NativeInterpreter() {
  PythonInterpreterPlugin::Terminate();
}
} // namespace lldb_private
