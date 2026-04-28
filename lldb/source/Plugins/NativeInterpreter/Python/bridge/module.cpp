#include <Python.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

//===----------------------------------------------------------------------===//
// IBID Backtrace Implementation
//===----------------------------------------------------------------------===//

/// Length-delimited string. Much like llvm::StringRef, but does not rely on
/// LLVM at all.
struct __ibid_string {
  const char *data = nullptr;
  int64_t len = 0;

  __ibid_string() = default;
  __ibid_string(std::string_view str)
      : data(str.data()), len((int64_t)str.size()) {}
  __ibid_string(const char *str) : data(str), len(strlen(str)) {}

  operator std::string_view() const { return {data, (size_t)len}; }

  bool operator==(const __ibid_string &other) {
    return std::string_view(*this) == std::string_view(other);
  }
};

/// IBID program point representation. Each frame point has a function name
/// representing the parent function, and a filename, line, and column
/// representing the current instruction for the python interpreter.
struct __ibid_program_point {
  __ibid_string function;
  __ibid_string filename;
  unsigned line;
  unsigned column;

  bool matches(const __ibid_program_point &other) {
    if (function == other.function)
      return true;

    std::filesystem::path file(filename.data);
    std::filesystem::path other_file(other.filename.data);
    // Check if the two paths are equivalent.
    if (std::filesystem::equivalent(file, other_file) && line == other.line) {
      // If a column was provided, then those must also match.
      if (column)
        return column == other.column;

      return true;
    }

    return false;
  }
};

#define IBID_SYMBOL __attribute__((visibility("default"))) extern "C"

/// Provide the number of interpreter frames as a global variable the debugger
/// can look up.
IBID_SYMBOL volatile unsigned __ibid_num_frames = 0;

/// Provide a pointer to the current interpreter frames as a global variable the
/// debugger can look up. Counted by __ibid_num_frames.
IBID_SYMBOL volatile __ibid_program_point *__ibid_current_backtrace = nullptr;

/// Get the local variable names for frame at index `idx` as JSON.
IBID_SYMBOL __ibid_string __ibid_get_frame_local_names(unsigned idx);

/// Evaluate the expression `expr` in the frame at index `idx` and return the
/// result as a string.
IBID_SYMBOL __ibid_string __ibid_evaluate_expression_in_frame(unsigned idx,
                                                              const char *expr);

/// Provide a place for the debugger to set a breakpoint when an interpreter
/// breakpoint is requested. This is always empty.
IBID_SYMBOL void __ibid_debugger_anchor() { ; }

/// Anchor called only when a registered source-line breakpoint matches the
/// current Python location. LLDB places source-line breakpoints here instead
/// of on __ibid_debugger_anchor so it only stops on real matches.
IBID_SYMBOL void __ibid_breakpoint_hit() { ; }

/// ID of the source-line breakpoint that was just matched. Set immediately
/// before __ibid_breakpoint_hit() is called so WasHit can identify which
/// breakpoint fired without re-reading the full backtrace.
IBID_SYMBOL volatile unsigned __ibid_hit_id = UINT_MAX;

/// Register a source-line breakpoint with the bridge. Returns an opaque ID
/// that LLDB stores and later compares against __ibid_hit_id.
IBID_SYMBOL unsigned __ibid_add_breakpoint(const char *filename,
                                            size_t filename_len,
                                            unsigned line, unsigned col);

/// Anchor called when a step-over lands at a new line. LLDB places an
/// internal breakpoint here to detect step completion.
IBID_SYMBOL void __ibid_step_hit() { ; }

/// Arm a step-over using the bridge's current frame state. Called by LLDB's
/// thread plan before resuming after a stop. The step fires __ibid_step_hit
/// when execution returns to the starting depth (or shallower) at a new line.
IBID_SYMBOL void __ibid_arm_step_over_current();

//===----------------------------------------------------------------------===//
// Source-line breakpoint registry
//===----------------------------------------------------------------------===//

namespace {
struct IBIDSourceBP {
  std::string requested_filename; // as provided by LLDB (basename or absolute)
  unsigned line;
  unsigned col; // 0 = any column
  unsigned id;
};
} // namespace

static std::vector<IBIDSourceBP> g_source_breakpoints;
static unsigned g_next_bp_id = 0;

//===----------------------------------------------------------------------===//
// Step-over state
//===----------------------------------------------------------------------===//

static bool g_step_active = false;
static unsigned g_step_start_depth = 0;
static unsigned g_step_start_line = 0;
static std::string g_step_start_filename;

unsigned __ibid_add_breakpoint(const char *filename, size_t filename_len,
                                unsigned line, unsigned col) {
  unsigned id = g_next_bp_id++;
  g_source_breakpoints.push_back(
      {std::string(filename, filename_len), line, col, id});
  return id;
}


// Match the full path stored in the tracer against what the user requested.
// If the request has no '/' it is treated as a basename-only match.
static bool ibid_filename_matches(const std::string &full_path,
                                   const std::string &requested) {
  if (requested.find('/') == std::string::npos) {
    auto pos = full_path.rfind('/');
    auto basename =
        pos != std::string::npos ? full_path.substr(pos + 1) : full_path;
    return basename == requested;
  }
  if (full_path == requested)
    return true;
  // Suffix match for relative-with-dirs or absolute sub-paths.
  if (full_path.size() > requested.size() &&
      full_path.compare(full_path.size() - requested.size(), requested.size(),
                        requested) == 0)
    return full_path[full_path.size() - requested.size() - 1] == '/';
  return false;
}

//===----------------------------------------------------------------------===//
// Helpers
//===----------------------------------------------------------------------===//

static std::string withoutQuotes(const std::string &s) {
  // Find the first non-quote character. Use that to slice the string and return
  // the substring that doesn't have quotes.
  auto non_quote = s.find_first_not_of("\"\'");
  return s.substr(non_quote, s.size() - (2 * non_quote));
}

static std::optional<std::string> py_string_to_string(PyObject *obj) {
  Py_ssize_t size = 0;
  const char *raw = PyUnicode_AsUTF8AndSize(obj, &size);
  if (!raw) {
    return std::nullopt;
  }

  return std::string(raw, size);
}

/// Provides a list that you can use to register destructors. When the object is
/// destroyed, all destructors are called in reverse order.
namespace {
struct DestructorList {
  std::vector<std::function<void()>> list;

  void append(std::function<void()> &&f) { list.push_back(std::move(f)); }

  ~DestructorList() {
    for (auto iter = list.rbegin(), end = list.rend(); iter != end; ++iter)
      (*iter)();
  }
};
} // namespace

/// Provides a log file for the bridge so we can inspect what inside the bridge.
static std::unique_ptr<std::ofstream> logfile = nullptr;

//===----------------------------------------------------------------------===//
// Internal Representation
//===----------------------------------------------------------------------===//

/// Provides an owning representation of an __ibid_program_point so that we
/// don't have to deal with memory management.
namespace {
struct ProgramPoint {
  PyFrameObject *frame;
  std::string function; // Each frame has a unique function.
  std::string filename;
  int line, col;

  /// Populate the value cache from `frame`. This will pre-populate all local
  /// variables into the value cache dict so we can access them later.
  void populateLocals() {
    // If the value cache is fully populated, we're done. This works because the
    // value cache is recreated along with every frame each time we pause.
    if (valueCachePopulated)
      return;

    DestructorList list;

    PyObject *locals = PyFrame_GetLocals(frame);
    if (!locals) {
      *logfile << "no locals\n";
      return;
    }

    // Create a dict we can use.
    PyObject *localsDict = PyDict_New();
    if (!localsDict) {
      *logfile << "no locals dict?\n";
      return;
    }
    list.append([&] { Py_DECREF(localsDict); });

    if (PyDict_Update(localsDict, locals)) {
      *logfile << "update failed\n";
      return;
    }

    *logfile << "found " << PyDict_Size(localsDict) << " locals\n";

    // Iterate the locals dict and populate the value cache. This ignores
    // __builtins__, modules, and functions. TBD if this is desirable behavior
    // or not, though!
    PyObject *key, *value;
    Py_ssize_t pos = 0;
    Py_BEGIN_CRITICAL_SECTION(localsDict);
    while (PyDict_Next(localsDict, &pos, &key, &value)) {
      // Ignore __builtins__ - that's not necessary.
      if (!PyUnicode_CompareWithASCIIString(key, "__builtins__")) {
        *logfile << "ignoring __builtins__\n";
        continue;
      }

      PyObject *keyRepr = PyObject_Repr(key);
      if (!keyRepr) {
        *logfile << "no keyrepr\n";
        continue;
      }

      auto keyOr = py_string_to_string(keyRepr);
      if (!keyOr) {
        *logfile << "no keyOr\n";
        continue;
      }

      // If the object is a module or function, we can ignore that too.
      if (PyModule_Check(value) || PyFunction_Check(value)) {
        *logfile << "ignoring module/function " << *keyOr << "\n";
        continue;
      }

      PyObject *valueRepr = PyObject_Repr(value);
      if (!valueRepr) {
        *logfile << "no valuerepr\n";
        continue;
      }

      auto valueOr = py_string_to_string(valueRepr);
      if (!valueOr) {
        *logfile << "no valueor\n";
        continue;
      }

      // Update the value in the cache.
      *logfile << "updating cache with " << withoutQuotes(*keyOr) << " = "
               << *valueOr << "\n";
      valueCache[withoutQuotes(*keyOr)] = std::move(*valueOr);
    }
    Py_END_CRITICAL_SECTION();
    *logfile << "finished with populate_frame_locals\n";
    valueCachePopulated = true;
  }

  // Cache of name/expr -> value. Mainly for memory management so as to be able
  // to return char * from the APIs easily.
  std::unordered_map<std::string, std::string> valueCache = {};
  bool valueCachePopulated = false;

  /// Render the local variable names.
  void renderNames() {
    if (!valueCachePopulated) {
      *logfile << "empty valueCache\n";
      return;
    }

    renderedNames = "[";
    // Keep the iterator outside the loop so we can handle it correctly.
    auto iter = valueCache.begin();
    // Stop one before the end so we don't have an extra comma.
    for (auto end = valueCache.end(); std::next(iter) != end; ++iter) {
      // Grab the values.
      auto &[k, _] = *iter;
      // Skip expression results. Those get returned directly to the user.
      if (k.find("expr:") != std::string::npos)
        continue;
      // Render this key into the string. Remove the quotes around it that
      // python adds for some reason.
      renderedNames += "\"" + withoutQuotes(k) + "\",";
    }
    renderedNames += "\"" + withoutQuotes(iter->first) + "\"]";
  }

  // Rendered variable names.
  std::string renderedNames = {};
};
} // namespace

/// Overall state of the program, meant to be used as the global container.
namespace {
struct ProgramState {
  PyObject_HEAD std::vector<ProgramPoint> current_frames;
  std::vector<__ibid_program_point> framelist;

  /// Update the frame list and the global state based on current_frames.
  void update() {
    __ibid_num_frames = current_frames.size();
    *logfile << "updated __ibid_num_frames with " << __ibid_num_frames << "\n";
    // Update the vector of ibid frames.
    framelist.resize(__ibid_num_frames, {});
    for (int i = 0, e = __ibid_num_frames; i < e; ++i) {
      auto &frame = current_frames[i];
      framelist[i] = {{frame.function},
                      {frame.filename},
                      (unsigned)frame.line,
                      (unsigned)frame.col};
    }
    // And set the pointer.
    __ibid_current_backtrace = framelist.data();
    *logfile << "updated __ibid_current_backtrace\n";
  }
};
} // namespace

/// Global pointer to store the memory used in the ibid states.
static ProgramState *g_state = nullptr;

void __ibid_arm_step_over_current() {
  if (!g_state || g_state->current_frames.empty())
    return;
  auto &top = g_state->current_frames[0];
  g_step_start_filename = top.filename;
  g_step_start_line = (unsigned)top.line;
  g_step_start_depth = (unsigned)g_state->current_frames.size();
  g_step_active = true;
}

//===----------------------------------------------------------------------===//
// IBID Implementations
//===----------------------------------------------------------------------===//

/// Returns the names of all the frame locals. This will render all the local
/// names.
__ibid_string __ibid_get_frame_local_names(unsigned idx) {
  if (!g_state)
    return {};

  if (idx >= g_state->current_frames.size())
    return {};

  auto &frame = g_state->current_frames[idx];

  // Populate the locals.
  frame.populateLocals();

  frame.renderNames();
  return {frame.renderedNames};
}

/// Evaluate `expr` in the frame. This should also be used for locals - it will
/// return those values no problem.
__ibid_string __ibid_evaluate_expression_in_frame(unsigned idx,
                                                  const char *expr) {
  if (!g_state)
    return {};

  if (idx >= g_state->current_frames.size())
    return {};

  auto &frame = g_state->current_frames[idx];

  // Populate the locals.
  frame.populateLocals();

  // Check the value cache first. If we have it, return it.
  if (auto found = frame.valueCache.find(expr);
      found != frame.valueCache.end() && !found->second.empty()) {
    return {found->second};
  }

  // Same for expressions.
  std::string exprKey = "expr:`" + std::string{expr} + "`";
  if (auto found = frame.valueCache.find(exprKey);
      found != frame.valueCache.end() && !found->second.empty()) {
    return {found->second};
  }

  // OK - was not found, so compile and execute the code.

  DestructorList list;

  // Get the locals and the globals, but it's OK if both are empty.
  PyObject *locals = PyFrame_GetLocals(frame.frame);
  PyObject *globals = PyFrame_GetGlobals(frame.frame);

  *logfile << "running expression `" << expr << "`\n";
  // Py_eval_input is the start symbol that I want - it will return the result
  // of the expression rather than treating it as the REPL would and printing it
  // out. See
  // https://docs.python.org/3/c-api/veryhigh.html#available-start-symbols for
  // more information.
  PyObject *result = PyRun_String(expr, Py_eval_input, globals, locals);
  if (!result) {
    // If we got an error, print it.
    if (PyErr_Occurred()) {
      PyErr_Print();
      return {};
    }
    *logfile << "expression failed\n";
    return {};
  }
  list.append([&]() { Py_DECREF(result); });

  // Return the string representation of the object.
  PyObject *repr = PyObject_Repr(result);
  if (!repr)
    return {};
  list.append([&]() { Py_DECREF(repr); });

  auto string_or = py_string_to_string(repr);
  if (!string_or)
    return {};

  // Set the variable and its value in the cache.
  frame.valueCache[exprKey] = *string_or;
  return {frame.valueCache[exprKey]};
}

//===----------------------------------------------------------------------===//
// Python Extension Implementation
//===----------------------------------------------------------------------===//

/// Initializer for the ProgramState object in Python.
static int ProgramState_init(ProgramState *self, PyObject *args_unused,
                             PyObject *kwds_unused) {
  // Take a reference to this object and store it in the global.
  g_state = self;
  logfile = std::make_unique<std::ofstream>();
  // Set the file to unbuffered.
  logfile->rdbuf()->pubsetbuf(nullptr, 0);
  logfile->open("lldb_bridge.log", std::ios::app);
  // Error, couldn't open the log file.
  if (!logfile->is_open())
    return -1;
  return 0;
}

/// Deallocator for the ProgramState object in Python.
static void ProgramState_dealloc(ProgramState *self) {
  // Call the destructor and set the global to nullptr.
  self->~ProgramState();
  g_state = nullptr;
  logfile->flush();
  logfile->close();
  logfile.reset(nullptr);
}

/// Call operator for the ProgramState object in Python.
static PyObject *ProgramState_call(ProgramState *self, PyObject *args,
                                   PyObject *kwds) {
  PyFrameObject *frame;
  PyObject *what_str;
  PyObject *arg;
  if (!PyArg_ParseTuple(args, "O!O!O:ProgramState_call", &PyFrame_Type, &frame,
                        &PyUnicode_Type, &what_str, &arg)) {
    return nullptr;
  }

  // Clear out the current frame list.
  self->current_frames.clear();

  // Iterate the current frames in inner -> outer order. Once the frame becomes
  // nullptr, we have to end.
  while (frame && PyFrame_Check(frame)) {
    int last_instr = PyFrame_GetLasti(frame);
    auto *code = PyFrame_GetCode(frame);

    int start_line, start_col, end_line, end_col;

    if (PyCode_Addr2Location(code, last_instr, &start_line, &start_col,
                             &end_line, &end_col) == 0) {
      // PyFrame_GetLasti returns -1 on "call" events (no instruction has
      // executed yet), which PyCode_Addr2Location cannot resolve. Skip this
      // frame so we still return self and keep the local trace active.
      frame = PyFrame_GetBack(frame);
      continue;
    }

    // Extract the filename.
    auto filename_or = py_string_to_string(code->co_filename);
    if (!filename_or)
      return nullptr;

    // Extract the qualified function name.
    auto function_or = py_string_to_string(code->co_qualname);
    if (!function_or)
      return nullptr;

    // TODO: Figure out some way to not re-traverse frames we've already seen.
    // Incref the frame - we're going to hold it.
    Py_INCREF(frame);
    self->current_frames.emplace_back(
        ProgramPoint{frame, *function_or, *filename_or, start_line, start_col});

    // Go to the next frame.
    frame = PyFrame_GetBack(frame);
  }

  // Update the ibid frames.
  self->update();

  // Only match source-line breakpoints on "line" events. In Python 3.14+,
  // "call" events report lasti=0 which PyCode_Addr2Location resolves to the
  // first line of the function body — the same line as the "line" event that
  // follows immediately. Firing on "call" would cause every breakpoint to hit
  // twice per function invocation.
  auto what_or = py_string_to_string(what_str);
  bool is_line_event = what_or && *what_or == "line";

  // If any source-line breakpoints are registered, check the topmost frame
  // against them. Call __ibid_breakpoint_hit (the fast anchor) only on a
  // real match so LLDB doesn't stop for every trace event.
  if (is_line_event && !g_source_breakpoints.empty() &&
      !self->current_frames.empty()) {
    auto &top = self->current_frames[0];
    for (auto &bp : g_source_breakpoints) {
      if (ibid_filename_matches(top.filename, bp.requested_filename) &&
          (unsigned)top.line == bp.line &&
          (bp.col == 0 || (unsigned)top.col == bp.col)) {
        __ibid_hit_id = bp.id;
        __ibid_breakpoint_hit();
        break;
      }
    }
  }

  // Check if an active step-over has completed. Fire __ibid_step_hit when
  // execution returns to the starting depth (or shallower) at a different line.
  if (is_line_event && g_step_active && !self->current_frames.empty()) {
    unsigned depth = (unsigned)self->current_frames.size();
    auto &top = self->current_frames[0];
    bool stepped_out = depth < g_step_start_depth;
    bool same_depth_new_line =
        depth == g_step_start_depth &&
        ((unsigned)top.line != g_step_start_line ||
         !ibid_filename_matches(top.filename, g_step_start_filename));
    if (stepped_out || same_depth_new_line) {
      g_step_active = false;
      __ibid_step_hit();
    }
  }

  // Call the anchor.
  __ibid_debugger_anchor();

  // Return ourselves.
  Py_INCREF(self);
  return (PyObject *)self;
}

/// __repr__ operator for the programstate object. This will allow us to view it
/// as a variable too!
static PyObject *ProgramState_repr(ProgramState *self) {
  if (self->current_frames.empty())
    return PyUnicode_FromString("ProgramState[]");
  std::string formatstr = "ProgramState[";
  for (int i = 0, e = self->current_frames.size(); i < e - 1; ++i) {
    auto &f = self->current_frames[i];
    formatstr += "{" + f.function + " at " + f.filename + ":" +
                 std::to_string(f.line) + ":" + std::to_string(f.col) + "}, ";
  }
  auto &f = self->current_frames.back();
  formatstr += "{" + f.function + " at " + f.filename + ":" +
               std::to_string(f.line) + ":" + std::to_string(f.col) + "}]";
  return PyUnicode_FromString(formatstr.c_str());
}

static PyTypeObject ProgramStateType = {
    PyVarObject_HEAD_INIT(NULL, 0) "lldb_bridge.ProgramState",
    sizeof(ProgramState),                     /*tp_basicsize*/
    0,                                        /*tp_itemsize*/
    (destructor)ProgramState_dealloc,         /*tp_dealloc*/
    0,                                        /*tp_vectorcall_offset*/
    0,                                        /*tp_getattr*/
    0,                                        /*tp_setattr*/
    0,                                        /*tp_compare*/
    (reprfunc)ProgramState_repr,              /*tp_repr*/
    0,                                        /*tp_as_number*/
    0,                                        /*tp_as_sequence*/
    0,                                        /*tp_as_mapping*/
    0,                                        /*tp_hash */
    (ternaryfunc)ProgramState_call,           /*tp_call*/
    0,                                        /*tp_str*/
    0,                                        /*tp_getattro*/
    0,                                        /*tp_setattro*/
    0,                                        /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /*tp_flags*/
    "Bridge objects",                         /* tp_doc */
    0,                                        /* tp_traverse */
    0,                                        /* tp_clear */
    0,                                        /* tp_richcompare */
    0,                                        /* tp_weaklistoffset */
    0,                                        /* tp_iter */
    0,                                        /* tp_iternext */
    0,                                        /* tp_methods */
    0,                                        /* tp_members */
    0,                                        /* tp_getset */
    0,                                        /* tp_base */
    0,                                        /* tp_dict */
    0,                                        /* tp_descr_get */
    0,                                        /* tp_descr_set */
    0,                                        /* tp_dictoffset */
    (initproc)ProgramState_init,              /* tp_init */
    0,                                        /* tp_alloc */
    0,                                        /* tp_new */
};

static int module_inited = 0;

static int bridge_exec(PyObject *mod) {
  if (module_inited != 0) {
    return 0;
  }

  // Initialize the program state type.
  ProgramStateType.tp_new = PyType_GenericNew;
  if (PyType_Ready(&ProgramStateType) < 0) {
    return -1;
  }

  // Add the type to the current module.
  Py_INCREF(&ProgramStateType);
  if (PyModule_AddObject(mod, "ProgramState", (PyObject *)&ProgramStateType) <
      0) {
    Py_DECREF(&ProgramStateType);
    return -1;
  }

  module_inited = 1;
  return 0;
}

static PyModuleDef_Slot bridge_slots[] = {
    {Py_mod_exec, (void *)&bridge_exec},
#if PY_VERSION_HEX >= 0x030c00f0 // Python 3.12+
    {Py_mod_multiple_interpreters, Py_MOD_MULTIPLE_INTERPRETERS_NOT_SUPPORTED},
#endif
#if PY_VERSION_HEX >= 0x030d00f0 // Python 3.13+
    // signal that this module supports running without an active GIL
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
#endif
    {0, NULL}};

#define MODULE_DOC                                                             \
  PyDoc_STR("LLDB Interpreted Binary Interactive Debugging (IBID) bridge.")

static PyModuleDef moduledef = {
    .m_base = PyModuleDef_HEAD_INIT,
    .m_name = "lldb_bridge",
    .m_doc = MODULE_DOC,
    .m_size = 0,
    .m_slots = bridge_slots,
};

PyMODINIT_FUNC PyInit_lldb_bridge(void) { return PyModuleDef_Init(&moduledef); }
