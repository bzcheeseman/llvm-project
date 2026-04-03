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
class IBIDInterpreterPlugin : public NativeInterpreter {
public:
  IBIDInterpreterPlugin(lldb::ModuleSP module_to_elide)
      : m_module_to_elide(std::move(module_to_elide)) {}

  static void Initialize() {
    PluginManager::RegisterPlugin(
        GetPluginNameStatic(),
        "plugin to handle interpreted binary debugging via IBID",
        IBIDInterpreterPlugin::CreateInstance);
  }
  static void Terminate() {
    PluginManager::UnregisterPlugin(IBIDInterpreterPlugin::CreateInstance);
  }

  static llvm::StringRef GetPluginNameStatic() {
    return "ibid-NativeInterpreter";
  }

  llvm::StringRef GetPluginName() override { return "ibid-NativeInterpreter"; }

  // This plugin is actually really simple - just construct one and pass it out.
  static NativeInterpreterSP CreateInstance(lldb::ModuleSP module_to_elide) {
    return std::make_shared<IBIDInterpreterPlugin>(std::move(module_to_elide));
  }

  // Construct a frame provider.
  lldb::SyntheticFrameProviderSP
  GetFrameProvider(lldb::StackFrameListSP frame_list) override {
    auto frame_provider_sp_or = InterpretedFrameProvider::CreateInstance(
        std::move(frame_list), m_module_to_elide);
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

private:
  lldb::ModuleSP m_module_to_elide;
};
} // namespace

// Forward the native interpreter plugin to the IBID interpreter plugin.
namespace lldb_private {
void lldb_initialize_NativeInterpreter() {
  IBIDInterpreterPlugin::Initialize();
}

void lldb_terminate_NativeInterpreter() {
  IBIDInterpreterPlugin::Terminate();
}
} // namespace lldb_private
