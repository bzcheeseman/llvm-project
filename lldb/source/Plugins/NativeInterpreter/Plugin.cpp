#include "lldb/Target/NativeInterpreter.h"

#include "BreakpointResolver.h"
#include "FrameProvider.h"

#include "lldb/Core/Module.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/Symbol/Function.h"
#include "lldb/Target/Target.h"
#include "lldb/Utility/FileSpec.h"
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
      : m_module_to_elide(std::move(module_to_elide)) {
    // TODO: We can use this:
    // https://docs.python.org/3.14/howto/remote_debugging.html#remote-debugging
    // to add our tracer to the interpreter state at process attach so the user
    // doesn't have to (but only for python, obviously)
  }

  static void Initialize() {
    PluginManager::RegisterPlugin(
        GetPluginNameStatic(),
        "plugin to handle interpreted binary debugging via IBID",
        IBIDInterpreterPlugin::CreateInstance);
  }
  static void Terminate() {
    PluginManager::UnregisterPlugin(IBIDInterpreterPlugin::CreateInstance);
  }

  static llvm::StringRef GetPluginNameStatic() { return "NativeInterpreter"; }

  llvm::StringRef GetPluginName() override { return GetPluginNameStatic(); }

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

  /// An implementation of this plugin will be able to provide a
  /// BreakpointResolver that can be used to resolve interpreter breakpoints for
  /// a given function name.
  lldb::BreakpointResolverSP GetBreakpointResolverForFunctionNames(
      const lldb::BreakpointSP &bkpt,
      std::vector<std::string> function_names) override {
    if (function_names.empty())
      return nullptr;
    return std::make_shared<InterpretedBreakpointResolver>(
        bkpt, std::move(function_names), FileSpec{}, 0, 0);
  }

  /// An implementation of this plugin will be able to provide a
  /// BreakpointResolver that can be used to resolve interpreter breakpoints for
  /// a given file/line/col location.
  lldb::BreakpointResolverSP
  GetBreakpointResolverForSourceLoc(const lldb::BreakpointSP &bkpt,
                                    FileSpec file, unsigned line,
                                    unsigned col = 0) override {
    return std::make_shared<InterpretedBreakpointResolver>(
        bkpt, std::vector<std::string>{}, file, line, col);
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

void lldb_terminate_NativeInterpreter() { IBIDInterpreterPlugin::Terminate(); }
} // namespace lldb_private
