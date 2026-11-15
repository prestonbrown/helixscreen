# LVGL 9.6 subject storage: move every subject onto `lv_subject_create()`

Status: plan, approved for execution. Deleted in the change that lands the final phase;
what lasts moves into `docs/devel/THREADING.md`, `docs/devel/LVGL9_XML_GUIDE.md` and
`docs/devel/architecture/02-subjects-dataflow.md`.

## 1. What LVGL 9.6 changed

Source: `lib/lvgl/include/lvgl/core/lv_observer.h`, `lib/lvgl/src/core/lv_observer.c`,
`lib/lvgl/docs/src/changelog/migration-v9-6.mdx` § "Subject create/delete".

| Call | Status | Behaviour that matters here |
|---|---|---|
| `lv_subject_t * lv_subject_create(lv_subject_type_t type)` | new | `lv_ll_ins_tail()` on LVGL's global `subject_ll`: one `lv_malloc(sizeof(lv_subject_t) + 2 pointers)`. Starts at a neutral value (0, NULL, black, "", empty group). Needs `lv_init()` first: the list lives in `LV_GLOBAL_DEFAULT()`. No main-thread assert. |
| `void lv_subject_delete(lv_subject_t * subject)` | new | NULL-safe. Same `deinit()` as `lv_subject_deinit()` (frees every observer, group membership, ext data), then unlinks and frees the node. |
| `lv_subject_init_int/_float/_string/_pointer/_color/_group` | `LV_DEPRECATED` | Zero the struct in place, seed value and previous value. |
| `lv_subject_deinit` | `LV_DEPRECATED` | Frees observers, keeps the storage. Our patch `patches/lvgl_subject_deinit_refuses_observers.patch` adds the `deinitialized` bit: a deinitialized subject refuses new observers until the next init zeroes it. |
| `lv_subject_set_string_buffer_static(s, buf, prev_buf, size)` | new | The only way to give a created string subject storage. Caller owns the buffers; they must outlive the subject. **Writes `buf[0] = '\0'` before anything else**, so an initial value that aliases `buf` is lost unless copied first. |
| `lv_subject_set_group_list_static` | new | Group list for a created group subject. |
| `lv_subject_global_deinit()` (from `lv_deinit()`) | | Deinitializes every subject still on `subject_ll` (groups first) and frees the nodes. After `lv_deinit()` every created subject pointer dangles. |

Initial values move to the ordinary setters. Consequences, all checked against our code:

- `lv_subject_set_int()` clamps to min/max and `lv_subject_init_int()` did not. No first-party
  code calls `lv_subject_set_min_value_*`, so nothing clamps.
- After the first set, the previous value is the neutral one. No first-party code calls
  `lv_subject_get_previous_*` (grep of `src/`, `include/`, `lib/helix-xml/src`).
- `lv_subject_set_pointer()` notifies unconditionally. Harmless at init: no observers yet.

Removal: the 9.6 notes mark the API deprecated and repeat the entry in
`migration-v10.mdx`; no removal version is stated. The build keeps deprecations a warning
(`Makefile`, `-Wno-error=deprecated-declarations` under `WERROR=1`).

`lv_subject_t::user_data` is "free for the user". Nothing in `lib/lvgl/src`,
`lib/helix-xml/src`, `src/` or `include/` reads or writes it on a subject. This plan uses it.

## 2. How HelixScreen holds subjects today

Measured on `main` at 8cf5d72393 (pointer-arithmetic census at c6e59a786d).

| Shape | Count | Where |
|---|---|---|
| `lv_subject_t` by-value declarations (members, statics, locals) | 1,872 | 172 headers, ~120 `.cpp` files |
| C arrays of `lv_subject_t` | 66 | |
| `std::array` / `std::vector` / `unique_ptr` / maps of `lv_subject_t` | 46 | e.g. `include/ams_state.h#BackendSlotSubjects`, `include/printer_fan_state.h#fan_speed_subjects_` |
| `make_unique<lv_subject_t>` / `new lv_subject_t` | 9 | the dynamic per-item subjects (fans, extruders, power devices, extruder usage, pre-print rows, progress arc, indexed pool) |
| Managed macro sites (`UI_MANAGED_SUBJECT_*`, `INIT_SUBJECT_*`, `UI_SUBJECT_INIT_AND_REGISTER_*`) | ~1,640 in `src/`+`include/`, ~60 in `tests/` | expand to the deprecated calls in three headers |
| Raw deprecated calls in `src/`+`include/` outside the macro headers | 468 (401 init, ~67 deinit) in ~115 files | top: `src/printer/ams_state_subjects.cpp` 68, `src/ui/panel_widgets/tool_switcher_widget.cpp` 29, `src/ui/panel_widgets/print_status_widget.cpp` 24 |
| of which the first argument is not `&expr` | 19 | `unique_ptr::get()`, local pointers, helper parameters |
| Raw deprecated calls in `tests/` | ~380 | 173 init_int, 159 deinit, 32 init_string, 12 copy_string |
| `lib/helix-xml/src` | 10 | `lv_xml_component.c` (XML-declared subjects, `lv_zalloc` + init), `lv_xml.c` (transient expression operands on the stack) |
| `&subject_member` uses | ~6,300 `src/`, ~1,300 `include/`, ~2,000 `tests/` | every LVGL call, getter and registry |

The owners and the machinery around them:

- **`SubjectManager`** (`include/subject_managed_panel.h`) records `lv_subject_t*` plus the XML
  name; `deinit_all()` withdraws the name from the XML scope and `SubjectDebugRegistry`, then
  calls `lv_subject_deinit()`. It already refuses to touch anything once
  `lv_is_initialized()` is false, and never logs (atexit chain). `WidgetSubjects` derives from it.
- **`StaticSubjectRegistry`** (`include/core/static_subject_registry.h`) runs every owner's
  `deinit_subjects()`: `Scope::PrinterSwitch` on a printer switch (LVGL stays up and the
  owners **re-init the same storage**), `Scope::ProcessExit` before `lv_deinit()`.
- **`ObserverGuard`** (`include/core/observer_guard.h`, `src/core/observer_guard.cpp#ObserverGuard::reset`)
  skips `lv_observer_delete()` when the owner's `SubjectLifetime` token reads dead, or when an
  invalidation epoch says a registry teardown may have freed the observer. Teardown-exempt
  subjects are kept in a set keyed by `lv_subject_t*`.
- **XML publication** (`helix::xml::register_subject_in_current_scope`, `lv_xml_register_subject`)
  and **`SubjectDebugRegistry`** are both keyed by the `lv_subject_t*` address.
- **Theme globals** (`src/ui/theme/theme_manager.cpp#publish_int_subject`) re-seed in place
  with a `ready` flag per subject.

**The load-bearing fact: a subject's address is stable for its owner's lifetime, across
any number of deinit/re-init cycles.** Cached getters, the XML registry, the debug
registry, the teardown-exempt set and every guard's `subject_` rely on it at each printer
switch. A design that frees on reset and allocates on re-init moves every subject at every
switch and turns each cached pointer into a use-after-free. This plan never does that.

## 3. Target design

### 3.1 One owner type: `helix::Subject`

`include/core/subject.h`, implemented in `src/core/subject.cpp`. Everything but the
plain loads is out of line, so a site costs one call (§ 6 has the measured sizes). The
header as phase 1 lands it, compiled by every unit that includes the macro headers:

```cpp
namespace helix {

class Subject {
  public:
    /// Allocates the node when LVGL is up on the main thread, so `&member` taken in a
    /// constructor already has its final address. The node refuses observers until the
    /// first init_*(). Before lv_init() the handle stays null until init_*().
    Subject() noexcept;
    ~Subject();
    Subject(const Subject&) = delete;
    Subject& operator=(const Subject&) = delete;
    Subject(Subject&& other) noexcept;
    Subject& operator=(Subject&& other) noexcept;

    void init_int(int32_t value);
    void init_float(float value);
    void init_pointer(void* value);
    void init_color(lv_color_t value);
    /// `buf` (and `prev_buf` when non-null) must outlive this Subject; `value` may alias `buf`.
    void init_string(char* buf, char* prev_buf, size_t size, const char* value);
    template <size_t N> void init_string(char (&buf)[N], const char* value);

    /// Removes every observer and refuses new ones until the next init_*(); keeps the
    /// allocation. The replacement for lv_subject_deinit().
    void reset();

    [[nodiscard]] bool is_live() const noexcept;       // inline load
    [[nodiscard]] lv_subject_t* get() const noexcept;  // inline load
    /// `&member` keeps yielding the lv_subject_t* every LVGL call takes.
    lv_subject_t* operator&() const noexcept;          // inline load

    /// The Subject that owns `subject`, or nullptr for one no Subject owns.
    [[nodiscard]] static Subject* owner_of(const lv_subject_t* subject) noexcept;

  private:
    lv_subject_t* prepare(lv_subject_type_t type);
    lv_subject_t* handle_ = nullptr;
    bool live_ = false;
};

/// lv_subject_deinit() for code holding only the pointer (SubjectManager, registries).
void subject_reset(lv_subject_t* subject);

/// The managed macros' entry points, overloaded on the storage so one macro serves an
/// unconverted lv_subject_t and a helix::Subject during the migration. The lv_subject_t
/// overloads are the last deprecated calls and are deleted in phase 5.
void subject_init_int(Subject& subject, int32_t value);
void subject_init_pointer(Subject& subject, void* value);
void subject_init_color(Subject& subject, lv_color_t value);
void subject_init_string(Subject& subject, char* buf, char* prev_buf, size_t size, const char* value);
void subject_init_int(lv_subject_t& subject, int32_t value);
void subject_init_pointer(lv_subject_t& subject, void* value);
void subject_init_color(lv_subject_t& subject, lv_color_t value);
void subject_init_string(lv_subject_t& subject, char* buf, char* prev_buf, size_t size, const char* value);

} // namespace helix
```

Contract, each line pinned by a case in § 5.1:

| Event | Behaviour |
|---|---|
| Construction | Creates a node only when `lv_is_initialized()` and on the main thread: `lv_subject_create(LV_SUBJECT_TYPE_INT)` as a placeholder, `user_data = this`, `deinitialized` set, not live. `&member` taken from then on (every panel, widget and modal constructor) is the address every later `init_*()` keeps; observers are refused until init, as after `reset()`. Otherwise `handle_` stays null and the first `init_*()` creates the node. |
| `&` of a `Subject` constructed before `lv_init()`, taken before its first `init_*()` | `nullptr` (namespace-scope statics, function-local statics first reached before `lv_init()`). LVGL's patched null guards turn a get/set/observe on it into a no-op plus the `observer_null_subject` anomaly, which is how that case surfaces. |
| `init_*()` | Creates the node if there is none, otherwise reuses it **whatever its previous type**: the type, value, previous value and range are written in place exactly as `lv_subject_init_<type>()` leaves them, so nothing is notified or clamped. Clears `deinitialized`, `user_data = this`, `live_ = true`. Refuses (anomaly `subject_init_before_lvgl`, no allocation) when `!lv_is_initialized()`. Off the main thread it reports `subject_init_off_main_thread`. |
| `init_*()` while live | Removes the observers first and reports `subject_reinit_while_live`. Today this zeroes `subs_ll` and orphans them. |
| Retype in place | Legal only while `subs_ll` is empty. A node that is not live refuses observers, so a non-empty list there is a breach: `init_*()` reports `subject_retype_with_observers` and removes them before rewriting the type. |
| `init_string()` | Points `value`/`prev_value` at the caller's buffers and copies the initial value with `memmove`, so a value that aliases or points into `buf` survives. `lv_subject_set_string_buffer_static()` is not used: it clears `buf[0]` before the copy, and `UI_MANAGED_SUBJECT_STRING_N` and `UI_SUBJECT_INIT_AND_REGISTER_STRING` both pass the buffer as the value. |
| `reset()` | Deletes every observer the way `deinit()` does (next pointer first, `lv_observer_delete()`, then `lv_ll_clear()`), sets `deinitialized`, `live_ = false`. Keeps `handle_`. No-op when not live or `!lv_is_initialized()`. |
| `~Subject()` | `lv_subject_delete(handle_)` when `handle_` and `lv_is_initialized()`; otherwise drops the handle (LVGL already freed it in `lv_deinit()`). Never logs. |
| Move | Transfers `handle_` and `live_`, re-points `user_data` at the destination; the source is empty. `vector<Subject>` relocation keeps every subject's address, which `vector<lv_subject_t>` did not. |
| Never initialized | A node made by the constructor and never initialized, or moved into a `Subject` that never initializes it, is freed by the destructor like any other; a moved-from `Subject` holds none. |
| Group subjects | Not supported by `Subject`. No `src/` code uses one; the two test uses call `lv_subject_create(LV_SUBJECT_TYPE_GROUP)` directly. |

Why the constructor and not `operator&` allocates: a lazy create inside `operator&`
(`h_ ? h_ : create()`) puts a branch and a call at each of ~9,600 `&` sites, measured at
12.4 B of Xtensa code per site over a plain load, against about 8 B per declaration for
an out-of-line constructor (§ 6). The cost of the constructor route: a `Subject` that is
never initialized still holds a node (~52 B of PSRAM), and an object built before
`lv_init()` gets its node only at `init_*()`.

**Ownership summary.** LVGL allocates; the `Subject` frees, exactly once, when it dies with
LVGL up. `reset()` never frees. Between `lv_deinit()` and process exit, `lv_deinit()` has
freed every node and each `Subject` destructor skips. This holds because no first-party
code calls `lv_init()` again after `lv_deinit()`: the three `lv_deinit()` calls in
`src/application/display_manager.cpp#DisplayManager::init` are failure exits and
`#DisplayManager::shutdown` is process exit, and no test calls `lv_deinit()`. Phase 1
adds a test that pins the skip; a future in-process re-init would need a generation
counter, which this plan deliberately does not add.

**Why `operator&`.** The ~9,600 `&member` sites keep compiling and keep meaning "the
`lv_subject_t*`". Declarations change type; call sites do not. The alternative, rewriting
every `&x_` to `x_.get()`, touches ~1,500 files while a dozen peer sessions edit the same
tree. The cost: `&s` on a `Subject` no longer names the `Subject`, so code that needs the
object uses `std::addressof` (the macros do). Standard containers already do.

**The silent hazard it creates: pointer arithmetic.** For `helix::Subject arr[N]`,
`&arr[0] + i`, `p - &arr[0]` (an index recovered from a subject pointer), `++p` and a
relational compare of two such pointers all still compile, because both sides are
`lv_subject_t*`, and they produce garbage: the handles are separate heap nodes. `arr`
decaying to `Subject*` and `.data()` fail to compile, so those are safe. `==`/`!=`
between two handles stays correct. Census on `main` at c6e59a786d: **0 sites** in `src/`,
`include/` and `tests/` (searched: `&X[...]` and `.data()` next to `+ - ++ --` or a
relational operator for each of the 105 names declared as an `lv_subject_t` array or
container, plus every `- &x[` and `- x.data()`; every hit was `&&` or string arithmetic).
The codemod still prints any such site it meets, and a batch agent fixes one with an
index from the container, never pointer math.

### 3.2 What changes around it

| Piece | Change |
|---|---|
| `SubjectManager::deinit_all` | `lv_subject_deinit(subject)` becomes `helix::subject_reset(subject)`. Signatures unchanged: it still stores `lv_subject_t*` and finds the owner through `user_data`. During the migration `subject_reset()` on a pointer no `Subject` owns calls the deprecated deinit (an unconverted member); phase 5 turns that branch into the anomaly `subject_reset_unowned`. |
| Macros in `include/subject_managed_panel.h`, `include/state/subject_macros.h`, `include/ui_subject_registry.h` | `lv_subject_init_*(&(s), ...)` becomes `helix::subject_init_*((s), ...)`. Overload resolution picks the storage. Nothing else in the macros changes. |
| `ObserverGuard`, `SubjectLifetime`, `StaticSubjectRegistry`, XML scope, `SubjectDebugRegistry`, `VolatileSubjects`, `DynamicSubjectMap`, `IndexedSubjectPool` | No interface change. All keyed by an address that no longer moves. The dynamic owners keep their token discipline: flip the token, then destroy the `Subject` (which frees). |
| `ui_xml` | Nothing. |
| `lib/helix-xml` | Its own C migration (phase 4): XML-declared subjects become `lv_subject_create()`/`lv_subject_delete()` with `lv_malloc`ed string buffers; `lv_xml_subject_record_release_storage` reads the buffer pointers, deletes the subject, then frees the buffers. Transient expression operands (`xml_compose_expr_resolver`) become created subjects freed at the end of the evaluation. |

## 4. Phases

Each phase lands on its own with the app working: unconverted storage keeps going through
the `lv_subject_t` overloads until phase 5 removes them.

| Phase | Scope | Agent | Worktree | Runs in parallel with | Files it shares |
|---|---|---|---|---|---|
| 1 Core | `Subject`, `subject_reset`, macro rerouting, `SubjectManager`, the gate, the codemod | 1 | `feature/subject-core` | nothing (everything depends on it) | the three macro headers |
| 2a Printer state | `include/printer_*`, `include/ams_state.h`, `src/printer/`, `src/app_globals.cpp`, `include/state/` | 1 | `feature/subject-printer` | 2b-2e, 3, 4 | none |
| 2b Home widgets | `src/ui/panel_widgets/`, `src/ui/widgets/`, `src/ui/home/` and their headers | 1 | `feature/subject-widgets` | 2a, 2c-2e, 3, 4 | none |
| 2c Print | `src/ui/print/`, `src/print/` and headers | 1 | `feature/subject-print` | 2a, 2b, 2d, 2e, 3, 4 | none |
| 2d Rest of `src/ui/` | `modals/ components/ controls/ filament/ wizard/ printers/ settings/ calibration/ theme/ notify/ debug/ tour/ plugin/` | 1 | `feature/subject-ui` | 2a-2c, 2e, 3, 4 | none |
| 2e Everything else | `src/system/ led/ util/ plugin/ api/ sensors/ xml_registration.cpp`, `include/performance_state.h` and the rest | 1 | `feature/subject-misc` | 2a-2d, 3, 4 | none |
| 3 Tests | `tests/` | 1 | `feature/subject-tests` | 2a-2e, 4 | none |
| 4 helix-xml | `lib/helix-xml` repo, then the pin move | 1 | in the submodule, then `feature/subject-xml-pin` | 2a-3 | the submodule pointer only |
| 5 Finish | delete the `lv_subject_t` overloads and the transitional branch, gate to a wall, docs, delete the codemod and this plan | 1 | `feature/subject-finish` | nothing (after all of 2, 3, 4 land) | macro headers, `src/core/subject.cpp` |

A header belongs to the batch that owns the `.cpp` implementing it; the phase 1 codemod
prints the owner for any header two batches both touch, and the agent who owns it converts it.

Per CLAUDE.md § Verification Cadence: phases 2a-2e, 3 and 4 are one batch. Each agent
converts, runs `scripts/syntax_check.py` on every file it touched, and commits, with no
builds or tag runs. The owning session merges them, runs the tags once on the merged
tree, then one review and one `make land-gate`.

### Phase 1: core (MAJOR, one agent, lands alone)

1. `include/core/subject.h` and `src/core/subject.cpp` exactly as § 3.1. The deprecated
   calls in this file are the `lv_subject_t&` overloads and the transitional branch of
   `subject_reset()`, inside one `-Wdeprecated-declarations` pragma block; the gate exempts
   the file.
2. Route the three macro headers through `helix::subject_init_*` (§ 3.2). Overload choice
   rides on the lvalue type, so all ~1,640 macro sites stop warning without an edit.
3. `SubjectManager::deinit_all` calls `helix::subject_reset`.
4. The gate (§ 7) with its baseline at the counts this commit leaves.
5. `scripts/subject_codemod.py`, used by phases 2 and 3. Mechanical rewrites only, on the
   paths given:
   - `lv_subject_t NAME;` / `lv_subject_t NAME[N];` / `std::array<lv_subject_t, N>` /
     `std::vector<lv_subject_t>` / `std::unique_ptr<lv_subject_t>` /
     `make_unique<lv_subject_t>()` to the `helix::Subject` form (adding `#include "core/subject.h"`);
   - `lv_subject_init_T(&EXPR, ARGS)` to `EXPR.init_T(ARGS)`, and `lv_subject_init_T(PTR.get(), ARGS)`
     to `PTR->init_T(ARGS)`;
   - `lv_subject_deinit(&EXPR)` to `EXPR.reset()`; any other `lv_subject_deinit(X)` to
     `helix::subject_reset(X)`;
   - helper parameters `lv_subject_t& p` that are init'd or reset in the body to `helix::Subject& p`;
   - prints, and does not touch, every other site (the 19 non-`&` inits, `lv_subject_t*`
     tables built before init, `sizeof(lv_subject_t)`, and every `&X[...]` or `.data()`
     of a converted array used with `+ - ++ --` or a relational operator, and every
     subtraction of two subject pointers), for the batch agent to do by hand.
   Its tests live in `tests/python/test_subject_codemod.py`.
6. Phase 1 converts no owner, so no node is allocated outside its own tests and the app's
   behaviour is unchanged.

### Phase 2a-2e: storage batches (one agent each, parallel worktrees)

For the paths in the row: run the codemod, hand-convert what it lists, delete the `bool
*_ready` / `subjects_initialized_` flags only where `is_live()` now says the same thing
(leave every other flag alone), and lower the gate baseline for the files converted.

Hazards each agent checks in its own files and lists in its commit body:

- **Address taken before `lv_init()`.** A `Subject` constructed after `lv_init()` has its
  final address from construction (§ 3.1). One constructed earlier (namespace scope, or a
  function-local static first reached before `lv_init()`) is `nullptr` until its first
  `init_*()`. Grep the batch for those and for a table, member initializer or lambda that
  stores their address before init; build it after init instead.
- **Pointer arithmetic on converted arrays** (§ 3.1). Compiles, yields garbage. Use the
  container index; the codemod lists every candidate.
- **Copies.** `Subject` is not copyable. A class that copied subject-holding structs fails
  to compile; that copy was already a bug (two owners of one `subs_ll`). Move or restructure.
- **`memset`/`memcpy` over a struct holding subjects.** Replace with value-init.
- **2a only:** `PrinterState` is a critical path (CLAUDE.md § Critical Paths). The dynamic
  per-item subjects in `src/printer/printer_fan_state.cpp`, `src/printer/printer_temperature_state.cpp`,
  `src/printer/power_device_state.cpp` and `src/printer/printer_print_state.cpp` become
  `std::unique_ptr<helix::Subject>` (or `helix::Subject` by value where the container is
  node-based); keep the order "flip the `SubjectLifetime`, then destroy".
- **2d only:** `src/ui/theme/theme_manager.cpp#publish_int_subject` keeps its in-place
  re-seed: `is_live()` replaces `ready`, and a re-publish calls `lv_subject_set_int()`, not `init_int()`.

### Phase 3: tests

Codemod `tests/`. A stack-local `lv_subject_t s; lv_subject_init_int(&s, 0); ...
lv_subject_deinit(&s);` becomes `helix::Subject s; s.init_int(0);` with the reset dropped
where the destructor now does it. Tests that pin LVGL's own deinit behaviour
(`tests/unit/test_subject_deinit_refuses_observers.cpp`) are rewritten onto
`Subject::reset()`, which is the path the app now takes; the two group-subject cases use
`lv_subject_create(LV_SUBJECT_TYPE_GROUP)` / `lv_subject_delete()`.
`lv_subject_copy_string` becomes `lv_subject_set_string` (rename only). Test-budget
neutral: this rewrites lines, it adds none.

### Phase 4: helix-xml

In `lib/helix-xml` (its own repo and test suite, `tests/cases/`): § 3.2's two changes,
then `tests/cases/test_subject_provenance.c` and the other cases that init stack subjects
moved onto `lv_subject_create()`. Push helix-xml `main` first, then the pin move in
helixscreen (memory: rebasing helix-xml commits orphans pins).

### Phase 5: finish

1. Delete the `lv_subject_t&` overloads; `subject_reset()` on an unowned pointer becomes
   the `subject_reset_unowned` anomaly. `src/core/subject.cpp` then calls no deprecated
   function: grep for `lv_subject_init_` and `lv_subject_deinit` over `src/ include/
   tests/ lib/helix-xml/src firmware/` returns nothing.
2. The gate becomes a wall: baseline file deleted, any count fails.
3. Docs: replace `lv_subject_deinit`/`lv_subject_init_*` guidance in
   `docs/devel/THREADING.md`, `docs/devel/LVGL9_XML_GUIDE.md`,
   `docs/devel/architecture/02-subjects-dataflow.md`, `01-declarative-ui.md`,
   `11-startup-shutdown.md`, `docs/devel/MULTI_PRINTER.md`, `docs/devel/LED_CONTROL.md`,
   `docs/devel/SENSOR_MANAGEMENT.md`, `docs/devel/DEVELOPER_QUICK_REFERENCE.md` and
   `.claude/rules/threading.md` with `helix::Subject`, `reset()` and the stable-address rule.
4. Delete `scripts/subject_codemod.py`, its tests and this plan.
5. Series-end checks (§ 5.3).

## 5. Tests

### 5.1 Phase 1: `tests/unit/test_subject_owner.cpp`, tags `[subject][raii]`

Each case went red under the hand break in its last column (`src/core/subject.cpp` edited,
one case run, file restored). The three cases that need LVGL down re-run the test binary
on one hidden `[.subject_child]` case each (skipped unless `HELIX_SUBJECT_CHILD_CASE` is set): every case in this binary starts with LVGL up,
and a forked child cannot call `lv_deinit()` (it joins draw threads the child lacks).

| Case | Pins | Hand break that turned it red |
|---|---|---|
| `Subject keeps its address across reset and re-init` | handle and node count unchanged by `reset()`; re-init (also to another type) keeps the address | `reset()` deletes the node and clears the handle |
| `Subject::reset removes every observer and refuses new ones until re-init` | `subs_ll` empty, `lv_subject_add_observer()` refused after reset, accepted after re-init | `reset()` does not set `deinitialized` |
| `Subject::reset unbinds a widget-bound observer from its widget` | the widget's event count returns to its pre-bind value | observers cleared from the list without `lv_observer_delete()` (also crashes on widget delete) |
| `SubjectManager deinit_all resets a registered Subject through its owner pointer` | `subject_reset()` finds the owner through `user_data` | `subject_reset()` ignores the owner |
| `Moving a Subject keeps its handle and re-points the owner` | `owner_of()` follows a move and `vector` relocation | move constructor leaves `user_data` on the source |
| `Subject destructor frees its LVGL node` | LVGL's subject list is back to its starting length, for an initialized node and for a never-initialized one moved into another `Subject` | destructor skips `lv_subject_delete()`; separately, destructor frees only live nodes |
| `Subject init_string keeps a value that aliases its buffer` | value equal to `buf`, and pointing inside it | `buf[0]` cleared before the copy |
| `Subject init_string with a previous buffer tracks the previous string` | `prev_buf` wiring (`src/plugin/lua_bind_ui.cpp` uses it) | `prev_value` left null |
| `Subject re-init while live removes the old observers` | no observer survives a re-init | the live branch of `prepare()` skips the removal |
| `Subject constructed after lv_init has a stable handle that init_* keeps` | constructor allocation; observers refused until init | constructor allocates nothing |
| `Subject constructed before lv_init yields null until init_*` | no node before `lv_init()`, still none after it until `init_*()`, then a live one | constructor ignores `lv_is_initialized()` |
| `Subject init before lv_init allocates nothing` | refusal path | `prepare()` ignores `lv_is_initialized()` |
| `Subject destructor after lv_deinit leaves the freed node alone` | the skip after `lv_deinit()` | destructor ignores `lv_is_initialized()` (double free) |
| `Managed macros accept both lv_subject_t and helix::Subject` | overload routing, owner pointer, `deinit_all()` on both storages | the `Subject&` overload calls the deprecated init on `&subject` |

Phase 1 runs `make t F='[subject],[observer],[raii],[subject_lifetime],[core][subjects],[macro]'`.

### 5.2 Phases 2-4

No new C++ cases: the conversion changes storage, not behaviour, and the existing suites
pin the behaviour. The merged batch runs, once:

| Tags | Covers |
|---|---|
| `[subject]`, `[observer]`, `[raii]`, `[subject_lifetime]`, `[crash_hardening]`, `[subjects][shutdown]` | owner, guard and shutdown machinery |
| `[printer_state]`, `[ams]`, `[core][subjects]` | 2a, including printer switch re-init |
| `[panel_widget]`, `[home]` | 2b |
| `[print]`, `[print_status]` | 2c |
| `[modal]`, `[settings]`, `[filament]`, `[wizard]`, `[theme]` | 2d |
| `[xml]`, `[xml][pool]` | 4 (plus helix-xml's own `tests/cases` suite in its repo) |

then the batch's one `make land-gate`, which runs the whole C++ suite in CI's layout plus
`tests/ui` (the app started and driven, so pre-init address use in panels shows up there).
`tests/python/test_subject_codemod.py` and the gate's bats run inside it.

Pre-init `&member` is null only for a `Subject` constructed before `lv_init()`, which means
a namespace-scope or early function-local static. Each phase-2 agent greps its batch for
`static helix::Subject` and namespace-scope `helix::Subject` and checks that no address of
one is stored before its `init_*()`; `observer_null_subject` in the telemetry anomaly
stream is the runtime signal.

### 5.3 Series end (after phase 5), once

- `scripts/test-host-run.sh asan` and `scripts/test-host-run.sh tsan` on zeus.
- The app under ASan through a printer switch and a clean exit on the test host: start
  `--test`, switch printers twice with `ctl`, quit. This is the only run that crosses
  `lv_deinit()` with live owners, which no unit test reaches.
- `make mutate-diff MUTATE_ARGS="--base <origin/main before phase 1>"` in the background,
  capped at 45 minutes; survivors become follow-up tests.
- ESP32: the CI esp32-build size check (§ 6).

## 6. ESP32

Firmware facts: `firmware/helixscreen-esp32/components/helixcore/lv_conf.h` has
`LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB`, `LV_USE_EXT_DATA 0`, `LV_USE_FLOAT 1`;
`sdkconfig.defaults` has `CONFIG_SPIRAM_USE_MALLOC=y` with
`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0`, so `lv_malloc` goes to PSRAM. `helixapp`'s `.bss`
is already linked into PSRAM (`components/helixapp/linker.lf`), and 187 subject
declarations carry `HELIX_PSRAM_BSS`. `app_srcs.txt` compiles ~1,600 subject init sites.

Per subject on the 32-bit target (computed from the struct, phase 1 confirms with a
`static_assert` in `firmware/.../helix_lvgl_abi.h`):

| | Today | After |
|---|---|---|
| In the owner | `lv_subject_t`, 36 B (PSRAM `.bss` or heap) | `Subject`, 8 B |
| Heap | 0 | 36 B + 8 B list links + ~4-8 B allocator header, ~52 B, PSRAM |
| Total | 36 B | ~60 B |

About +24 B per live subject, ~40 KB of PSRAM for 1,600, against 8 MB. Internal DRAM does
not move: only `subject_ll`'s head lives in `lv_global`. Fragmentation: the allocation is
made once per owner and survives every reset and printer switch, so steady state has no
churn; the per-item subjects that do churn (fans, extruders) already allocate today.

Flash: every class holding subjects gains a destructor call per member, and macro sites
call `helix::subject_init_*` instead of `lv_subject_init_*` (one call either way).
`Subject`'s members stay out of line except the plain loads (`get()`, `operator&`,
`is_live()`): measured with the IDF's `xtensa-esp32s3-elf-g++ -Os -mlongcalls`, a
`h_ ? h_ : slow()` operator& costs 12.4 B per `&` site over a plain load, and an
out-of-line constructor or destructor about 8 B per member.

The flash delta is measured locally, not estimated: phase 1 adds no owner, so its delta is
the core alone; each phase-2 batch measures its own. Run on the batch's base commit and on
its tip, from the worktree root, and compare the image size `check_esp32_size.py` prints:

```bash
make apply-patches
python3 scripts/esp32_printer_images.py && python3 scripts/esp32_stage_assets.py && python3 scripts/esp32_pack_assets.py
PRIMARY=$(dirname "$(git rev-parse --path-format=absolute --git-common-dir)")
scripts/pool-docker.sh docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp \
    -v "$PWD:$PWD" -v "$PRIMARY:$PRIMARY:ro" -w "$PWD/firmware/helixscreen-esp32" espressif/idf:v5.5.5 \
    bash -c "git config --global --add safe.directory '*' && . /opt/esp/idf/export.sh >/dev/null && idf.py build"
python3 scripts/check_esp32_size.py firmware/helixscreen-esp32/build/helixscreen_esp32.bin \
    firmware/helixscreen-esp32/size_budget.json firmware/helixscreen-esp32/build/helixscreen_esp32.map
```

The batch's commit body records base size, tip size and the difference. CI's esp32-build
runs the same size check on every push and stays the gate.

No separate allocation strategy for the firmware: the CLIB path already lands in PSRAM,
and a pool would add code to save memory that is not short.

## 7. The gate: `scripts/check_lvgl_subject_api.py`

Modelled on `scripts/check_hand_rolled_swatch.py` (per-file ratchet through
`scripts/ratchet.py`), wired as `scripts/qc/lvgl_subject_api.sh` in `QC_ALL` (cheap, so
it runs at commit with `--staged-only`), meta-tested in
`tests/shell/test_lvgl_subject_api_gate.bats`, listed in `tests/shell/tooling-triggers.tsv`.

- **Counts, per file**, in `src/ include/ tests/ firmware/` (`.c .cc .cpp .h .hpp .inc`),
  code only (comments and strings blanked with `scripts/source_strip.py#blank_cxx`):
  calls to `lv_subject_init_{int,float,string,pointer,color,group}`, `lv_subject_deinit`,
  `lv_subject_copy_string`; and by-value `lv_subject_t` storage (declarations of a
  variable, member or array of `lv_subject_t`, `std::array/vector/deque/unique_ptr` of it,
  `make_unique<lv_subject_t>`, `new lv_subject_t`). `lv_subject_t*` and `lv_subject_t&`
  are not counted.
- **Exempt**: `src/core/subject.cpp`, and a line carrying `SUBJECT_API_OK: <reason>` or
  under a comment line carrying it.
- **Baseline** `scripts/lvgl_subject_api_baseline.txt`, `path count` per line; counts may
  fall, never rise; a file absent starts at zero. `--write-baseline` rewrites it, `--list`
  prints every site.
- **Bats cases**: a new `lv_subject_init_int(&x, 0)` in a fixture file fails; a new
  `lv_subject_t x_;` member fails; `lv_subject_t* p` and `const lv_subject_t& r` pass; a
  call inside a comment or string passes; `SUBJECT_API_OK` passes; a count under baseline
  passes leniently and fails under `HELIX_RATCHET_STRICT=1`; `src/core/subject.cpp` passes.
- Phase 5 deletes the baseline, so any count fails.

`scripts/lvgl96_rename.py` keeps excluding the subject API (its docstring says so); the
two gates do not overlap.

## 8. Threads

Subjects are main-thread only (`.claude/rules/threading.md` rule 1), and nothing here
changes who touches them. `lv_subject_create()` has no thread assert, so `Subject::init_*`
asserts `helix::ui::is_main_thread()` once the main thread is known
(`include/core/update_queue.h#is_main_thread`). Validity of a captured `lv_subject_t*` is
unchanged: before, the storage lived as long as the owner; now the allocation does,
because `reset()` never frees. The one new window is after `lv_deinit()`, where a pointer
into a freed node replaces a pointer into dead but mapped storage; § 5.3's ASan app run is
the check for code that reads a subject in a static destructor.

## 9. Decisions taken

- One untyped `helix::Subject`, mirroring `lv_subject_t`, so the codemod is a type swap.
  Typed wrappers (`IntSubject`, ...) would catch type mismatches at compile time but need
  the type of every one of ~1,900 declarations; not worth it inside this migration.
- `operator&` returns the handle (§ 3.1).
- Allocation once per owner, freed only by the owner's death (§ 2, the load-bearing fact).
- The back-pointer lives in `lv_subject_t::user_data`; no side table, no LVGL patch.
- No LVGL generation counter: no first-party re-init after `lv_deinit()` exists.

## 10. Open questions

None. `operator&` was weighed against the `.get()` sweep and accepted (§ 3.1).
