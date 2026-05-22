<p align="center">
  <img src="./logo.svg" alt="CORE UI" height="64">
</p>

<p align="center">
  <b>English</b> · <a href="./README.md">中文</a>
  &nbsp;·&nbsp;
  <a href="https://ghboke.github.io/core-ui/"><b>📖 Online Docs</b></a>
</p>

**Core UI** is a modern Windows desktop UI framework, rebuilt from the ground up to match Microsoft's **Fluent 2** visual language while keeping **native-level performance** and a **tiny distribution footprint**. Rendering runs on **Direct2D / Direct3D 11** hardware acceleration, and every widget — from buttons and text fields to Flyout, Dialog, and TitleBar — is exposed through a single **pure C API (250+ functions)**, so Rust, Go, Python, C#, Delphi, and even Lua can bind it directly without writing a C++ shim.

UIs are best described in **`.uix` single-file components** — Vue 3 SFC style (`<window>` + `<script>` + `<style>` + `<template>`) with reactive bindings, computed / methods, `v-if` / `v-for` / `v-model` / `@click`, a CSS subset, and CSS-variable theming. Scripts are evaluated in-process by an embedded **QuickJS-NG** runtime — no DOM, no Webview. The project is also purpose-built for **AI-driven development**: a single self-contained cheatsheet ([`docs/uix-ai-guide.md`](./docs/uix-ai-guide.md)) lets an LLM read it once and emit a complete, runnable app, turning "describe what you want" into "see it running" in a single loop.

> **A 3.0 MB single DLL that ships Office / VS Code-grade UI on Windows.**
> No Chromium. No .NET. No 40 MB of Qt DLLs and moc/uic preprocessors. One C header, one `.uix` single-file component — done.

![version](https://img.shields.io/badge/version-1.6.0.112-blue)
![license](https://img.shields.io/badge/license-MIT-green)
![platform](https://img.shields.io/badge/platform-Windows%2010%2B-lightgrey)
![size](https://img.shields.io/badge/dll-3.0MB-brightgreen)
![api](https://img.shields.io/badge/C%20API-250%2B-orange)
![script](https://img.shields.io/badge/script-QuickJS--NG-purple)

## 🎯 Why Core UI

| Dimension | Electron | WPF / WinUI 3 | Qt | **Core UI** |
|-----------|----------|---------------|-----|-------------|
| **Distribution size** | 100+ MB | needs .NET runtime | 40+ MB Qt DLLs | **3.0 MB single DLL** |
| **Startup time** | 1–3 s | 0.5–1 s | 0.5–1 s | **< 200 ms** |
| **Memory footprint** | 150+ MB | 80+ MB | 60+ MB | **< 30 MB** |
| **Language bindings** | JS only | .NET only | C++ only | **C ABI, any language** |
| **Design language** | DIY | Fluent (limited) | Platform-native | **Fluent 2, native-grade** |
| **Declarative / reactive UI** | JSX + virtual DOM | XAML + Binding | QML | **`.uix` Vue 3 SFC (QuickJS-NG)** |
| **Learning curve** | full-stack JS | XAML + C# | C++ + meta object | **Vue templates + C, instant** |

**In one line:** You want Electron's DX + native-level performance + Fluent 2 looks + Vue's reactivity model — Core UI is currently the only option that checks all those boxes.

## 🤖 Fast UI Development for the AI Era

**Core UI is designed from the ground up to be driven by LLMs and AI agents.**

Everybody is having AI write code now. Try asking Claude / GPT / Cursor to build a Qt app — it'll point you at `moc`, include a pile of unnecessary headers, and misremember signal/slot syntax. Try WPF — it will blend XAML, C# code-behind, and `x:Bind` expressions. The reason is simple: **the cognitive surface of these frameworks was never designed with a token budget in mind.**

Core UI is architected so agents get it right:

| Design decision | Win for AI |
|-----------------|-----------|
| **`.uix` = Vue 3 SFC** | LLM training data is dominated by Vue / React / HTML far more than any desktop UI; templates + `data/computed/methods` come out right the first time |
| **Pure C ABI, every handle is `uint64_t`** | No C++ templates, no inheritance, no virtuals — LLMs almost never hallucinate a type error |
| **CSS-style styling + Flexbox** | Frontend layout intuition transfers directly — no new grid / anchor / dock system to learn |
| **Every API follows `ui_<noun>_<verb>`** | Highly predictable: "load a `.uix`" → `ui_page_load_file`, guessed right on the first try |
| **A single self-contained cheatsheet** | Agents only need to fetch `docs/uix-ai-guide.md`; no repo-wide crawl required |
| **250+ exports in the DLL symbol table** | Agents can `objdump -p core-ui.dll` to ground-truth the current API surface |
| **All text uses `$t('key')`** | AI-generated UI is i18n-ready and data-driven by default, not hard-coded |
| **`ui_debug_*` event injection** | Agents can click / type / screenshot without a real mouse or keyboard — closing the emit → build → click → verify loop |

Typical workflow:

```
User: "Build me a Windows desktop download-manager UI with a Fluent look"
  ↓
Agent fetches docs/uix-ai-guide.md   (one file — all the knowledge)
  ↓
Agent emits   app.uix + main.cpp     (single-file SFC + 10 lines of C)
  ↓
build-clang-cl.ps1 -Target ...       (compiler is the ground truth, no LLM self-check)
  ↓
ui_debug_*  click + screenshot       (agent runs its own regression — the loop closes)
```

### 📖 AI Documentation Entry Points

| Entry | Audience | Description |
|-------|----------|-------------|
| **[`llms.txt`](./llms.txt)** | AI agent crawlers | [llmstxt.org](https://llmstxt.org) standard index — the first file an agent should fetch |
| **[`docs/uix-ai-guide.md`](./docs/uix-ai-guide.md)** | LLMs writing code | **Self-contained cheatsheet**: `.uix` file structure + template + script + CSS subset + widget list + examples |
| **[`docs/uix-guide.md`](./docs/uix-guide.md)** | Deep reference | Vue 3 SFC complete guide, cookbook, limitations |
| **[`docs/debug-simulation.md`](./docs/debug-simulation.md)** | Automation | `ui_debug_*` event injection + Named Pipe IPC, AI self-verification loop |
| **[`UI_CORE_API.md`](./UI_CORE_API.md)** | Full API | 250+ exported functions, grouped by module |

**Cursor / Claude Code / Cline / Continue users:** add `docs/uix-ai-guide.md` to your project rules (Cursor's `.cursorrules`, Claude Code's `CLAUDE.md`) for full coverage in a single context.

Copy-paste prompt template:

```
This project uses the Core UI framework. Read docs/uix-ai-guide.md before generating code.
Constraints:
  1. Describe UI in .uix single-file components; C++ only writes the
     ui_init / ui_page_load_file / ui_run glue.
  2. <script> must be a Vue 3 SFC: export default { data(){...}, computed:{}, methods:{} }.
  3. Use CSS variables for colors: var(--bg) / var(--fg) / var(--accent) ...; never hard-code hex.
  4. Reference text via {{ $t('key') }} into .lang files; never hard-code user-facing strings.
  5. Template events: @click / @change / @input / @focus / @blur / @mousedown / @wheel ...
```

## 🆕 What's New in 1.6.0 (since the v1.5.0 public release)

1.6.0 consolidates 65 internal builds of capability extensions and bug fixes since
the v1.5.0 public release (build 46). Full per-build history lives in
[CHANGELOG.md](./CHANGELOG.md).

### New capability — Widget-level event callbacks (the full set)

Previously only prebuilt widgets like `button` / `input` could receive `onclick`.
1.6.0 opens up event callbacks to **any widget** — including custom-drawn
`<custom>` widgets — so user-drawn controls behave like first-class citizens:

```c
ui_widget_on_mouse_move (w, on_move,  ctx);   // widget-local coordinates
ui_widget_on_mouse_leave(w, on_leave, ctx);
ui_widget_on_mouse_wheel(w, on_wheel, ctx);   // generic, not just ScrollView
ui_widget_on_focus      (w, on_focus, ctx);   // pair with ui_custom_set_focusable
ui_widget_on_blur       (w, on_blur,  ctx);
ui_widget_set_cursor    (w, "hand");          // hand / wait / size-* / IDC_*
```

### New capability — Reactive menu system (rewrite)

`<menu>` is now fully reactive, Vue 3 style — wires through `data()` state:

```vue
<menu trigger="#tree-item" event="rclick">
  <menuitem v-for="cmd in commands" :key="cmd.id"
            :disabled="!cmd.enabled" @click="run(cmd)">
    <label>{{ cmd.label }}</label>
  </menuitem>
  <separator/>
  <menuitem v-if="isAdmin" @click="del"><label>Delete</label></menuitem>
</menu>
```

- rclick dispatch is now **deepest-match**: nested `<menu trigger>` blocks no
  longer let an ancestor steal the child widget's menu — matching DOM event
  bubbling semantics
- submenu autoclose uses the visible rect; arrow is now an outline glyph; widths
  align with the parent menu

### New capability — Window lifecycle / zero-flash first frame

- `ui_page_prepare_window` — hidden window + RT pre-creation, zero black flash on first frame
- `UiWindowConfig.start_maximized` — persisted "maximized close → maximized open" with no 1-frame normal flash
- `UiWindowConfig::owner` — child window attached to main window (no separate taskbar entry, follows parent minimize)
- `ui_debug_server` supports multiple servers concurrently, one pipe per window (great for main + settings windows side-by-side debugging)

### Misc additions

- `:id="..."` dynamic binding (finally usable for `v-for` unique ids)
- `ui_toast_ex(anim = UI_TOAST_ANIM_FADE)` — pure fade in/out style; toasts in the center position no longer "pop in" instantly
- `ui_window_dpi_scale` — generic helper

### Bug fixes from real-world feedback

**ScrollView three-fix combo:**
- `<ScrollView>` wrapping multiple v-if/v-for panels misbehaved → always wrap children in a VBox and patch v-if/v-for `parentWidget`
- CSS `padding` on ScrollView used to be a no-op → `DoLayout` now subtracts padL/padT/padR/padB
- Frameless window's bottom edge couldn't be dragged to resize → NC hit-test now matches left/right/top (always HTBOTTOM)

**Other notable fixes:**
- `PushRoundedClip` initialized with `INITIALIZE_FOR_CLEARTYPE` to prevent ClearType text disappearing inside layers
- SVG presentation attrs (`stroke` / `fill` / `stroke-width`) now inherit from parent to children
- LabelWidget honors CSS `line-height` (default 1.3 × font-size) and `white-space: nowrap`
- Toggle ON-state thumb in dark mode is now white instead of an invisible black
- WM_KEYDOWN onKey self-destruct path no longer falls through into UAF

### 💥 BREAKING (must read when upgrading from 1.5.0)

| Change | Old | New |
|---|---|---|
| menuitem became a widget template | `<menuitem>X</menuitem>` | `<menuitem><label>X</label></menuitem>` |
| Window geometry API: x/y now screen px | DIP / screen mixed | x/y/w/h all screen px; pair with `ui_window_dpi_scale` |
| `ui_toast_ex` added `anim` parameter | `ui_toast_ex(win,t,d,pos,icon)` | `ui_toast_ex(win,t,d,pos,icon, UI_TOAST_ANIM_SLIDE)` |
| `ui_toast_at` removed | `ui_toast_at(win,t,d,pos)` | `ui_toast_ex(win,t,d,pos,0,UI_TOAST_ANIM_SLIDE)` |
| `WireMenus` static helper removed | `WireMenus(...)` auto-wire | use `<menu>` declarative + Vue 3 binding |

## ✨ Core Features

### 🚀 Ridiculously small, absurdly fast

- **3.0 MB full DLL**, or a **~2 MB statically-linked exe** — it fits on a USB stick
- **Direct2D + Direct3D 11** full hardware acceleration, Per-Monitor DPI V2 out of the box
- **Cold start < 200 ms** — click and the window is already there

### 🎨 Looks that sell

- Strictly aligned with **Microsoft Fluent 2 design tokens**: colors, radii, shadows, motion — no shortcuts
- Dark / light theme switches with **one line** of C; CSS variables re-cascade automatically
- **Custom borderless window** ships with a `<TitleBar>` control, native drag / snap / animation
- 25+ controls match WinUI 3's granularity: `button` / `input` / `toggle` / `combobox` / `progressbar` / `menu` / `Dialog` / `Toast` ...

### 🧩 `.uix` single-file components — write desktop UI like Vue

```vue
<window title="Hello" width="400" height="300" centered="true" theme="light"/>

<script>
export default {
  data()    { return { count: 0 }; },
  computed: { doubled() { return this.count * 2; } },
  methods:  { inc() { this.count++; } }
}
</script>

<style>
  .root  { padding: 24px; gap: 12px; background: var(--bg); }
  .h1    { font-size: 22px; color: var(--fg); font-weight: 600; }
  button { background: var(--accent); color: #fff;
           padding: 6px 14px; border-radius: 4px; cursor: pointer; }
</style>

<template>
  <div class="root">
    <label class="h1">Hello, Core UI!</label>
    <label>count = {{ count }} · doubled = {{ doubled }}</label>
    <button @click="inc">+1</button>
  </div>
</template>
```

- **Vue 3 Options API**: `data()` / `computed` / `methods`, evaluated by QuickJS-NG (ES2020+)
- **Reactivity**: Proxy + WatchEffect; templates auto-track dependencies via `{{ expr }}` / `:attr` / `v-if` / `v-for` / `v-model` / `@click` and re-render incrementally
- **CSS subset**: class / element / descendant selectors, pseudo-classes (`:hover`, `:disabled`), Flexbox, `var(--*)` CSS-variable theming
- **`<window>` tag**: declarative window config (title / size / frameless / theme) — no C needed
- **Built-in i18n**: `{{ $t('welcome') }}` resolves from `.lang` files; switch at runtime via `ui_page_set_locale`
- **Nested `v-for > v-if > v-for`** is safe; keyed diff reuses widgets across reorder
- **Declarative right-click menus**: `<menu trigger="#id" event="rclick">` + `<menuitem>` / `<separator>`

### 🌐 Pure C API — every language is welcome

```c
#include <ui_core.h>

ui_init_with_theme(UI_THEME_LIGHT);

UiPage page = ui_page_load_file(L"app.uix");
ui_page_load_language_file(page, "zh", L"lang/zh.lang");
ui_page_set_locale(page, "zh");

UiWindow win = ui_page_open_window(page, NULL);

/* Two-way exchange of reactive state */
ui_page_set_int (page, "count", 42);
ui_page_set_text(page, "name",  L"Alice");
ui_page_set_json(page, "items", "[{\"id\":1,\"label\":\"a\"}]");
char* j = ui_page_get_json(page, "items");
ui_page_free(j);

ui_run();
ui_page_destroy(page);
```

- **250+ exported functions**, all handles are plain `uint64_t` — zero C++ types leak through
- Rust / Go / Python / C# / Delphi / Pascal / Lua can all bind directly
- The Page API (`ui_page_*`) is purpose-built for `.uix`; the lower-level widget factories (`ui_vbox`, `ui_button`, ...) remain for fully procedural construction

### ⚡ Widget-level event callbacks — custom widgets handle the full set (new in 1.6.0)

Any widget (including custom-drawn `<custom>` widgets) can now receive event
callbacks the same way built-in controls do — no more "only button/input can
receive onclick":

```c
ui_widget_on_mouse_move (w, on_move,  ctx);   // widget-local coordinates
ui_widget_on_mouse_leave(w, on_leave, ctx);
ui_widget_on_mouse_wheel(w, on_wheel, ctx);   // generic, not just ScrollView
ui_widget_on_focus      (w, on_focus, ctx);   // pair with ui_custom_set_focusable
ui_widget_on_blur       (w, on_blur,  ctx);
ui_widget_set_cursor    (w, "hand");          // hand / wait / size-* / IDC_*
```

### 🔍 Automation / debugging: controls are programmable

Shipped in 1.1.0: a full **`ui_debug_*` event-injection API** — drive any control
without real mouse or keyboard. Designed for end-to-end tests, AI agents operating
UIs, and scripted regressions:

```c
ui_debug_click(win, btn);                    // full mouse down/up, fires onClick
ui_debug_combo_select(win, combo, 2);        // select item 2 + fire onChanged
ui_debug_right_click_at(win, 300, 200);      // pop up the registered context menu
int path[] = {2, 0};
ui_debug_menu_click_path(win, path, 2);      // click the leaf in a submenu
ui_debug_type_text(win, L"hello");           // per-character keyboard input
```

- **60+ `ui_debug_*` functions**: click / hover / drag / wheel / key / focus,
  per-widget high-level ops, submenu path click, HWND `PostMessage` channel, etc.
- Built-in **Named Pipe IPC** (`\\.\pipe\ui_core_debug`) with 45+ commands —
  drive from PowerShell / Python in one line; start with `ui_debug_server_start(win, NULL)`
- **Golden-image regression**: `golden_runner.exe` renders `demo/golden/*.uix` to PNG and diffs against `*.expected.png`
- **Thread-safety helper** `ui_window_invoke_sync` marshals calls from worker threads onto the UI thread
- Full reference in **[`docs/debug-simulation.md`](./docs/debug-simulation.md)**

## 📊 Real Numbers

| Metric | Value |
|--------|-------|
| `core-ui.dll` size (full feature) | **3.0 MB** |
| `ui-demo-uix.exe` size (static link) | **~2 MB** |
| Empty window memory | **< 30 MB** |
| Cold start to first frame | **< 200 ms** |
| 60 fps animation CPU usage | **< 3%** |
| Exported C functions | **250+** |
| Built-in controls | **25+** |
| Script runtime | **QuickJS-NG v0.14.0** (ES2020+) |
| Image formats (ImageView) | PNG / JPG / BMP / GIF / WebP / SVG |

## 🚀 Getting Started

### Requirements

- Windows 10 (1709+)
- CMake 3.20+
- **MSVC 2019+ or clang-cl** (C++17) — MinGW is not supported

### Build

Builds must be invoked from PowerShell using the bundled script (it sets up
vcvars64 and routes around a Windows SDK rc.exe hang in ninja subprocesses by
swapping in llvm-rc):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File scripts/build-clang-cl.ps1 -Target core-ui
```

Common targets:

| Target | Artifact |
|---|---|
| `core-ui` | `core-ui.dll` + `core-ui.lib` import library (default) |
| `core-ui-static` | `core-ui-static.lib` self-contained static archive (bundles QuickJS) |
| `ui-demo-uix` | `ui-demo-uix.exe` single-file demo (resources baked in) |
| `golden_runner` | `golden_runner.exe` golden-image regression runner |

Pass `-Clean` to rebuild the build directory from scratch; omit `-Target` to build everything.

Single-exe distribution (no DLL dependency):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File scripts/build-clang-cl.ps1 -Target core-ui -Static
```

### Hello World

**hello.uix**

```vue
<window title="Hello" width="400" height="300" centered="true" theme="light"/>

<script>
export default {
  data()    { return { count: 0 }; },
  methods:  { inc() { this.count++; } }
}
</script>

<style>
  .root  { padding: 24px; gap: 12px; background: var(--bg); }
  button { background: var(--accent); color: #fff;
           padding: 6px 14px; border-radius: 4px; }
</style>

<template>
  <div class="root">
    <label>count = {{ count }}</label>
    <button @click="inc">+1</button>
  </div>
</template>
```

**main.cpp**

```cpp
#include <ui_core.h>

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    ui_init_with_theme(UI_THEME_LIGHT);

    UiPage page = ui_page_load_file(L"hello.uix");
    if (!page) return 1;

    UiWindow win = ui_page_open_window(page, NULL);
    if (!win) { ui_page_destroy(page); return 2; }

    int ret = ui_run();
    ui_page_destroy(page);
    return ret;
}
```

That's it. **No `.vcxproj`, no `moc` preprocessor, no XAML compiler, no IDL.**

### Single-exe packaging (resources baked into the executable)

```cmake
include(cmake/UiCoreHelpers.cmake)

add_executable(my-app WIN32 main.cpp)
target_link_libraries(my-app PRIVATE core-ui)

ui_core_embed_text(my-app FILE app.uix      OUT app.embed.h     VAR k_app)
ui_core_embed_text(my-app FILE lang/zh.lang OUT lang_zh.embed.h VAR k_lang_zh)
```

```cpp
#include "app.embed.h"
#include "lang_zh.embed.h"

UiPage page = ui_page_load_string(k_app);
ui_page_load_language_string(page, "zh", k_lang_zh);
```

`demo/ui_demo_uix.cpp` is the smallest complete example of this pattern (57 lines of glue + a single `.uix` file with 12 demo pages).

## 🧩 Built-in Tags / Controls

Tags inside `.uix` templates map directly to native widgets:

| Category | Tags |
|---|---|
| **Containers** | `div` (Flexbox: `flex-direction` / `flex` / `gap` / `padding`) |
| **Text** | `label` (multi-line, auto-wrap) |
| **Buttons** | `button`, `IconButton` |
| **Input** | `input` (type=`text` / `password` / `checkbox` / `radio` / `range` / `number`), `textarea` |
| **Selection** | `toggle`, `combobox` |
| **Status** | `progressbar`, `badge` (CSS class) |
| **Popups** | `menu` / `menuitem` / `separator`, `Flyout`, `Dialog`, `Toast` |
| **Image** | `img`, `svg` (inline); the underlying `ImageView` supports zoom / pan / crop |
| **Window** | `TitleBar` (only when `frameless="true"`) |

For procedural construction, the C API also offers factories: `ui_vbox` / `ui_hbox` / `ui_label` / `ui_button` / `ui_text_input` / `ui_combobox` / `ui_slider` / `ui_progress_bar` / `ui_image_view` / `ui_dialog` / `ui_tab_control` / `ui_scroll_view`, etc.

## 🎨 Theming

Built-in Fluent 2 dark / light themes, one-line runtime switch:

```c
ui_theme_set_mode(UI_THEME_DARK);
ui_theme_set_mode(UI_THEME_LIGHT);
```

`.uix` `<style>` blocks reference theme colors via CSS variables — the library re-cascades on theme change so all controls follow:

```css
var(--bg) / var(--fg) / var(--fg-2) / var(--fg-3)
var(--accent) / var(--card-bg) / var(--sidebar-bg)
var(--sidebar-hover) / var(--border-subtle)
```

## 🔢 Versioning

Version format: `MAJOR.MINOR.PATCH.BUILD` (currently `1.6.0.112`)

- Compile-time macros: `UI_CORE_VERSION_MAJOR/MINOR/PATCH/BUILD` + `UI_CORE_VERSION_STRING`
- Runtime API:

```c
int major, minor, patch;
ui_core_version(&major, &minor, &patch);   // 1, 5, 0
int build = ui_core_version_build();        // 42
const char* v = ui_core_version_string();   // "1.6.0.112"
```

- Windows DLL properties (right-click `core-ui.dll` → Properties → Details) show `FileVersion 1.6.0.112`
- Full release history in [CHANGELOG.md](./CHANGELOG.md)

## 📁 Project Layout

```
core-ui/
├── include/
│   ├── ui_core.h         # Public C API (250+ functions)
│   └── plugin_api.h
├── src/ui/
│   ├── renderer.*        # Direct2D renderer
│   ├── widget.*          # Base widget + layout
│   ├── controls.*        # Built-in controls
│   ├── ui_window.*       # Window + event dispatch
│   ├── ui_api.cpp        # Core C API implementation
│   ├── ui_debug_server.* # Named Pipe debug IPC
│   ├── animation.*       # Animation system
│   ├── context_menu.*    # Right-click menu
│   ├── image_*           # Image decoding / GDI / SVG / GIF
│   ├── uix/              # .uix SFC parser + QuickJS script runtime
│   ├── markup/           # Legacy .ui XML markup (compatibility)
│   ├── reactive/         # Proxy + WatchEffect reactive bindings
│   ├── css/              # CSS parsing / selectors / cascade
│   ├── flex/             # Flexbox layout
│   ├── page/             # Page API (widget_factory / compiler / state)
│   └── expression/       # JSON interop
├── demo/
│   ├── ui_demo.uix       # 12-page SFC demo
│   ├── ui_demo_uix.cpp   # 60 lines of glue + locale poll
│   ├── lang/             # zh / en language packs
│   ├── golden/           # Golden-image regression cases
│   └── golden_runner.cpp # Render-and-diff runner
├── docs/                 # Documentation
├── scripts/              # build-clang-cl.ps1 etc.
├── cmake/                # UiCoreHelpers.cmake (embed_text)
├── test/                 # Unit tests
├── CMakeLists.txt
├── VERSION
└── CHANGELOG.md
```

## 📚 Documentation

| Doc | Content |
|-----|---------|
| [Getting Started](docs/getting-started.md) | Integration guide (CMake + MSVC / clang-cl) |
| [.uix AI Cheatsheet](docs/uix-ai-guide.md) | **One-page prompt to feed an LLM** to write `.uix` single-file components |
| [.uix Guide](docs/uix-guide.md) | Full Vue 3 SFC reference (script / CSS / widgets / cookbook / limitations) |
| [Debug & Automation](docs/debug-simulation.md) | Named Pipe event injection, AI self-verification loop |
| [C API Reference](docs/c-api.md) | Parameter-level detail for every function |
| [Controls](docs/controls.md) | Per-control spec |
| [Layout](docs/layout.md) | Flexbox / percentage / absolute positioning |
| [Design System](docs/design-system.md) | Fluent 2 design tokens |
| [i18n](docs/i18n.md) | `.lang` files + `$t()` |
| [.ui Markup Cheatsheet](docs/ai-guide.md) | Legacy `.ui` XML markup cheatsheet (compatibility path) |
| [.ui Markup Reference](docs/markup.md) | `.ui` syntax |
| [API Index](UI_CORE_API.md) | Full exported function list |

> Note: docs are currently authored in Chinese. English translations are planned.

## 🤝 Where It Fits

- ✅ **Windows utility apps** (downloaders, image viewers, config managers, data tools)
- ✅ **Native projects that want Fluent looks** without getting locked into .NET / WinUI
- ✅ **Rust / Go / Python projects** that need a Fluent UI but can't find a solid binding
- ✅ **Embedding into existing C++ projects** as the UI layer — no third-party runtime
- ✅ **Size-sensitive offline distribution** where shipping Electron is not an option
- ✅ **AI-driven UI generation**: target `.uix` as the output format and close the emit → build → click → screenshot loop

## 📝 License

[MIT License](./LICENSE) © core-ui contributors

— If this project saved you from shipping another Electron app, **please drop a Star ⭐**.
