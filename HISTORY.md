# vim mouse: development history and lessons

Read this before changing the code. It records how the program got to its
current shape, which mistakes were already made and fixed, which behaviour
was changed on purpose, and where to make common changes. It covers every
commit on `master` from `0964d95` (2026-03-02) to `443b135` (2026-10-31): 93
commits, 79 of them non-merge. It also covers the 7 commits of the unrelated
`bakup` branch (2026-02-10 to 2026-02-23), where the program started. Hashes in
backticks are short commit ids; `git show <hash>` gives the full story, and
most commit bodies explain the measured symptom and the root cause.

User-facing behaviour is documented in `README.md`. The module contract is
`common.h`. This file is about *why* the code looks the way it does.

## 1. How to use this file

- Before a change, read section 5 (rules) and section 6 (decisions not to
  revert) for the area you touch. Most bugs in this history came back in a
  new form after a nearby change, not from new code.
- Use section 8 to find every place a change has to go. A key or setting
  touches up to seven places.
- In the same commit as your change, update this file when you fix a bug with
  a lesson in it, change a key binding or default, or finish a piece of work
  left open in section 7. Add a commit-log row (section 4) for each commit.

## 2. Architecture map

Windows-only C (gnu11) on the Unicode Win32 API, built with MinGW. One
process, one instance (mutex `Local\vim_mouse_single_instance`).

| File | Lines | Responsibility | Names worth knowing |
|---|---|---|---|
| `common.h` | 393 | The contract between modules: `WM_APP_*` messages, `Config` (every setting with its default in a comment), every cross-module function. Read it first. | `Config g_cfg`, `Hotkey`, `KMOD_*`, `ClickList`, `FOCUS_*` |
| `main.c` | 566 | Entry point, single instance (waits for the old instance after an update), DPI awareness, main window and message routing, settings-file watcher thread, logging, startup and shutdown order. | `main`, `main_wnd_proc`, `app_log`, `app_reload_config`, `app_config_written`, `watch_thread_proc`, `dpi_for_monitor` |
| `config.c` | 2131 | `vim.ini` location (exe folder or `%APPDATA%\vim mouse`), defaults, parsing, warnings, key-name parsing and formatting, default-file template, text-shortcut table (locked) and the `[shortcuts]` rewrite, import and export. | `config_set_defaults`, `config_load`, `config_path`, `process_general` / `process_mouse` / `process_hints`, `apply_*`, `build_default_ini`, `read_text_file`, `shortcuts_save`, `parse_key`, `parse_hotkey` |
| `input.c` | 3294 | The single keyboard dispatcher: `WH_KEYBOARD_LL` hook, hotkeys, mouse mode, Alt and bare keys, text-shortcut capture, keyboard lock, Alt withholding for menus, movement and scroll thread, scroll-to-end glides, mode indicator, session and power handling, hook-loss detection. | `kb_proc`, `handle_key` / `handle_down` / `handle_up`, `binding_action`, `alt_key_action`, `apply_action_down`, `bare_key_ok`, `popup_open`, `alt_should_withhold`, `scroll_press`, `scroll_end_press`, `scroll_end_shift_up`, `movement_thread`, `raw_input_check`, `replace_hook`, `reset_key_state`, `input_thread_proc` |
| `hint.c` | 2834 | The click-hint overlay on the UI thread: element mode (prefix-free labels), grid mode with subgrid, the status bar, queued keys during a scan, prescan adoption, the wait for a browser to catch up, live refresh merging, monitor cycling. | `hint_open`, `hint_prepare`, `hint_on_elements`, `hint_on_key`, `hint_hotkey_again`, `start_scan`, `begin_scan`, `settle_wait_ms`, `adopt_prescan`, `finalize_scan`, `merge_scan`, `alloc_labels`, `replay_queue`, `paint_overlay` |
| `uia.c` | 4414 | UI Automation through hand-declared COM vtables. Four MTA workers: the element scan (one thread plus 8 helpers), the scroll-target cache, the `Shift+g` scroll to the bottom and the focus classifier for bare keys. All engines are created under one lock. | `uia_worker`, `uia_helper`, `scan_window_uia`, `filter_containers`, `scan_reject`, `detail_flush`, `click_grid_build`, `slow_check`, `lazy_tree_window`, `uia_chromium_window`, `engine_lock`, `uia_engine_init`, `scroll_worker`, `uia_scroll_target`, `scroll_end_worker`, `scroll_end_at`, `uia_scroll_end`, `focus_worker`, `focus_classify`, `uia_focus_wait` |
| `tray.c` | 1385 | Tray icon and menu, balloons, the help window (balanced columns), start-with-Windows Run value and its repair. | `tray_init`, `show_menu`, `tray_on_command`, `build_help_rows`, `help_layout`, `startup_enabled`, `startup_repair`, `run_value_path` |
| `snippet.c` | 1031 | Text-shortcut editor window (tray, "Text shortcuts..."), import and export, and inserting a shortcut's text by clipboard paste (saved and restored) or by typing. | `snippet_editor_open`, `snippet_editor_refresh`, `first_key_taken`, `snippet_insert`, `save_clipboard`, `restore_clipboard`, `paste_text` |
| `update.c` | 483 | Self-update (tray, "Update to latest version"): WinINet download of the latest release's `vim.exe` on a worker, byte comparison with the running exe, rename-then-restart with `--updated`, startup cleanup of `<exe>.old` / `<exe>.new`. | `update_start`, `update_worker`, `download`, `check_download`, `update_on_done`, `install_and_restart`, `update_wait_for_old`, `update_cleanup` |
| `version.h` | 12 | The one place to bump the version (numbers and both strings). | `VIM_VERSION_*` |
| `vim.rc`, `vim.manifest`, `vim.ico` | | Resources: icon, manifest (Common Controls v6, per-monitor DPI, supported OS), version info. `vim.ico` is generated by `tools/make_icon.c` and committed. | |
| `tools/uia_probe.c` | 292 | Console tool: lists what the scan finds on the monitor under the cursor (`-v` for every element). `make probe`. | |
| `tools/hint_check.c`, `tools/hint_check.mjs` | 1941, 804 | Compares Chrome's DOM clickables (over CDP) with the real scan and reports misses with the drop reason. `make check`, then `node tools/hint_check.mjs [url ...]`. | `g_scan_debug` hook in `uia.c` |
| `tools/make_icon.c` | 512 | Draws `vim.ico` (large sizes stored as PNG). `make icon`. | |

### Threads and how modules talk

| Thread | Owner | Does |
|---|---|---|
| Main / UI | `main.c` | Message loop; the overlay (`hint.c`), tray and help (`tray.c`), editor (`snippet.c`). Everything that touches windows we own. Every `WM_APP_*` message in `common.h` lands here. |
| Input (highest priority) | `input.c` `input_thread_proc` | Installs the low-level hook (`kb_proc`), WinEvent focus hooks, raw keyboard input (hook-loss check) and the power/session window; owns the mode indicator window. |
| Movement | `input.c` `movement_thread` | Pointer movement, scroll streams, glides. Ticks on a high-resolution waitable timer (1 ms). Calls `uia_scroll_target` and `uia_scroll_end`. |
| UIA scan + 8 helpers | `uia.c` `uia_worker`, `uia_helper` | Scans windows in parallel, merges in completion order, posts cumulative `ClickList`s to the main window (`WM_APP_ELEMENTS`). |
| UIA scroll worker | `uia.c` `scroll_worker` | Per-window cache (8 windows, LRU) of scrollable areas for Alt and bare scrolling, kept warm after a press. |
| UIA scroll-end worker | `uia.c` `scroll_end_worker` | `Shift+g`: `ScrollPattern.SetScrollPercent` to 100 on the scrollable area at a point. Started on first use. |
| UIA focus worker | `uia.c` `focus_worker` | Decides whether the focus is in a text field, after focus and foreground events and polls; the hook reads the cached answer. |
| Settings watcher | `main.c` `watch_thread_proc` | Notices `vim.ini` changes and posts `WM_APP_RELOAD_CONFIG`. |
| Update download (on demand) | `update.c` `update_worker` | Downloads `<exe>.new`, compares it with the running exe, posts `WM_APP_UPDATE_DONE`. The swap and restart run on the main thread. |

Flow of a hint open: Alt goes down, so the hook posts
`WM_APP_HINT_PREPARE(1)` and the overlay starts a prescan (`e9f57b1`). The F
key completes the hotkey, so the hook turns key capture on itself and posts
`WM_APP_HINT_OPEN`. The overlay adopts the running scan, and `uia.c` posts
partial and final `ClickList`s. Typed keys arrive as `WM_APP_HINT_KEY`; the
overlay clicks through `mouse_click_at`.

Invariants stated in `common.h` and enforced by history:
- `g_cfg` is written only by the main thread (load and reload); other threads
  only read it.
- UI Automation never runs on the UI thread. Querying our own windows from
  there can deadlock.
- The hook is the only keyboard dispatcher. The old `RegisterHotKey` approach
  (`0964d95`) is gone; every hotkey is matched inside `kb_proc`.
- Shutdown order in `main_wnd_proc` `WM_DESTROY`: watcher, editor, hints,
  **`input_stop` before `uia_stop`** (`4dda943`), then tray.

## 3. Timeline by phase

### Prehistory: `m.c` on branch `bakup` (2026-02-10 to 2026-02-23)
`37fc028` `b07d5c9` `c3be97f` `8bf3184` `23758b6` `59b091c` `cbdaf8c`

- A separate history with no common ancestor with `master` (fetch it with
  `git fetch origin bakup`). `m.c` grew from 178 lines: CapsLock-on mouse
  movement, then drag (`b07d5c9`) and acceleration (`8bf3184`).
- Three fixes there are the first appearance of problems this history keeps
  meeting. `c3be97f` "fix capslock key misdirection": key-up now *always*
  clears the internal key flags so they cannot get stuck, and drag starts
  only when CapsLock is on. `23758b6` "fix slight movement after release":
  when an axis goes idle, clear its velocity and fractional accumulator, or
  smoothing makes the pointer coast. `cbdaf8c` "not working intermittently":
  a message-only window receives `WM_POWERBROADCAST`, and on resume the hook
  is uninstalled and reinstalled and motion state is reset.
- `0964d95` restarted the history as `vim.c`. Its `.gitignore`, `Makefile`
  and `README.md` are the `bakup` files (the README still names `m.c` and
  `mouse_control_windows.exe`).

### Phase 0: the original single-file program (2026-03-02 to 2026-04-10)
`0964d95` `c03c5e8` `0212997` `8f5d19c`

- One console program, `vim.c` (~1460 lines), with a Makefile and a short
  README. CapsLock *state* meant mouse mode, `hjkl` moved the pointer
  (500 to 1500 px/s over 0.65 s), a fixed 160 x 80 grid overlay opened on
  `Shift+F` through `RegisterHotKey`, and `Ctrl+Alt+Q` quit.
- `c03c5e8` added a watchdog thread that reinstalled a missing hook and
  restarted a dead movement thread. `0212997` added a suspended state on
  session lock and reset motion after long time gaps (sleep or resume).
- `8f5d19c` moved the hint hotkey from `Shift+F` to `Alt+F` "for vim
  compatibility". `Alt+F` has been the hint hotkey since.

### Phase 1: usability overhaul and module split (2026-10-23)
`a267a1e` `0f692d4` `3217713` `fe00e2b` `0ebf562`, merges `a89f753` `8126570`

- `a267a1e` wrote the module contract (`common.h`) and a redesign spec
  (`SPEC.md`) first. Parallel agents then built `hint.c` (`0f692d4`),
  `main.c` and `tray.c` (`3217713`), `input.c` (`fe00e2b`) and `config.c` in
  separate worktrees, merged into branch `usability-overhaul`.
- `0ebf562` integrated the modules with review fixes, deleted `vim.c` and
  `SPEC.md`, and ignored `vim.ini` and `vim.log`. Notable fixes there: the
  hint hotkey is inactive while paused, pausing closes the hints, Esc drops
  key capture inside the hook as a failsafe, and the indicator repaints only
  when it moves or changes.
- Outcome: the contract-first split worked. `common.h` is still the place
  where module boundaries are written down.

### Phase 2: clickable-element hints through UI Automation (2026-10-23 to 2026-10-27)
`43063d0` `64a5805` `c5e5472` `f6cb7d4` `0bb6577` `52e952a` `92bf572` `af50fc5` `72e0cbb`

- `43063d0` extended the contract and `64a5805` added `uia.c`: an MTA worker
  with hand-declared COM vtables (MinGW.org ships no UIAutomation headers).
  It skips covered and cloaked windows, drops occluded elements, scrollbar
  parts, containers and duplicates, and pokes each new foreground window so
  Chromium, Electron and Firefox build their accessibility tree before the
  first scan. `hint.c` gained element mode with Vimium-style prefix-free
  labels and keys queued during the scan. The grid became the fallback.
- `c5e5472`: menus close as soon as they see Alt go down, so while a popup is
  open (or the hints capture keys) the left Alt is withheld and replayed when
  needed.
- `f6cb7d4` made it fast: cheap tests first, parallel window scans, a
  persistent back buffer (opening went from 15 to 50 ms down to ~5 ms), and
  a skip list for slow apps. Movement and scrolling got faster too.
- Accuracy work, driven by real pages: `52e952a` (Google's duplicated inner
  targets), then the measurement tool `92bf572` (`hint_check`), then
  `af50fc5` and `72e0cbb` (actionable cards, clipping to the page Document,
  which list items count as wrappers).

### Phase 3: release pipeline and packaging (2026-10-24 to 2026-10-29)
`60df0e5` (PR merged as `08abd50`) `673be4c` `394b361` `8d7fcde` `4114c5f`

- `60df0e5`: `release.yml` publishes `vim.exe` to a rolling "latest" release
  on every push to `master` and on `v*` tags; `build.bat` builds by
  double-click. It also removed a duplicate `g_cfg` definition that GCC 10+
  refuses to link (`-fno-common`).
- `673be4c`: MSYS2 dropped `mingw-w64-i686-make`, so the release now
  cross-compiles the 32-bit exe on Ubuntu with `gcc-mingw-w64-i686`.
- `394b361`: manifest, version info (`version.h`), embedded icon, and
  `build.yml`, which builds 32-bit and 64-bit with `-Werror` on every branch
  except `master`. `8d7fcde` and `4114c5f` store the large icon sizes as PNG
  (313 KB down to 45 KB).

### Phase 4: Alt scrolling, text shortcuts, keyboard lock (2026-10-28 to 2026-10-29)
`09b7fdb` `f90c515` `5b3d7e4` `faf5234` `54c5551`

- `09b7fdb`: `Alt+hjkl` scrolls anywhere and `Alt+u/d` scrolls fast, reusing
  the mouse-mode keys and the scroll thread. Text shortcuts work by holding
  Alt, typing an abbreviation and releasing Alt; they live in `[shortcuts]`
  and get an editor (`snippet.c`).
- `f90c515`: `Ctrl+Alt+L` keyboard lock; the hook tracks the swallowed
  modifiers itself.
- `5b3d7e4`: Alt scrolling targets the nearest scrollable area through UI
  Automation, and text shortcuts are pasted through the clipboard instead of
  typed. `faf5234`: import and export in the editor. `54c5551`: a MinGW.org
  header gap.

### Phase 5: exact, smooth and fast (2026-10-29)
`56c6e10` `25e734f` `fcdc742` `e9f57b1` `267ab08` `1e8677d` `3615694` `b54285f` `aad1eaa` `7d5de41`

- Measured and fixed: pointer speed was 0.57x to 1.79x of the setting
  (`56c6e10`); one slow window held back every other window's labels
  (`25e734f`); every Alt scroll searched the whole window first (`fcdc742`);
  the scan now starts when Alt goes down (`e9f57b1`, ~4 ms to labels instead
  of 50 to 110 ms).
- Scrolling became a per-frame velocity stream (`3615694`). Scroll-to-end was
  added as `Alt+u u` / `Alt+d d` (`b54285f`) and then made to glide
  (`aad1eaa`). The container filter stopped comparing every pair (`7d5de41`).
- Side effects fixed within the phase: `267ab08` (prescan timing lost
  labels) and `1e8677d` (second hotkey press rescans).

### Phase 6: hardening and review fixes (2026-10-29)
`e04d6a0` `4dda943` `4b04fd5` `80970eb` `4dcbc6f` `6cf94df` `21aa24e` `2eebd25` `4955523` `f0da5fd` `348a968` `729e682` `e987c6a`

- Work done largely by parallel agents in worktrees, merged into
  `element-hints` (merges `8abb770` `cf1fcbf` `e583902` `c4617c9` `8fb609e`
  `8abcd74`). `4dcbc6f` had to gitignore `.claude/worktrees/`.
- Environment robustness: read-only exe folder, settings encodings, Run
  value repair, hook loss, RDP disconnect, quit order, clipboard limits.
- Real-app robustness: Explorer (`348a968`) and Teams (`729e682`, `e987c6a`).

### Phase 7: live hints, bare keys, `g g` (2026-10-30)
`8b0a194` `b9a8d6f` `b474c89` `e51fecb` `27e5efe` `da32599` `7855485` `5ea3a61` `6622ce0` `ba62724` `37b210c` `1c0a83b` `ee4f7fb` `33a979f` `515eb02` `443b135`, merges `0979ae1` `40657d6` `30f7f62` `735de23`

- `8b0a194`: labels stay current while the overlay is open (`scan_refresh`).
- `b474c89`: **bare keys**. Outside text fields `f h j k l u d , .` work
  without Alt, using a new UIA focus worker. `Alt+h` / `Alt+l` became back
  and forward.
- `e51fecb`: the grid never opens by itself any more.
- The new focus worker broke hint startup about one start in three
  (`27e5efe`), and bare keys were unreliable (`da32599`, `7855485`,
  `5ea3a61`); all three were fixed the same day.
- `6622ce0`: scroll-to-end moved to `g g` / `Shift+g`, and `ba62724` made
  `Shift+g` work in browsers by waiting for Shift to come up. `be52799`
  made it act on press through UI Automation, with that wait as the
  fallback.
- `37b210c` adds diagnostics for the one unresolved bare-key case (section 7).
- `ee4f7fb`: the tray menu updates the program to the latest release
  (`update.c`; the rename-then-restart rule is in section 5).
- `33a979f`: labels already on screen stay put while the hints refresh
  (`keep_labels`), so a label cannot change between reading and typing it.
- `515eb02`: that made Chromium's late reports visible. Labels sat where the
  page had been before a `j`/`k` scroll, and a panel opened by a hint click
  got none. After our own scroll or click in a Chromium window the first
  scan now waits (`scan_settle`), and the first labels leave free codes for
  elements that appear later.
- The account menu still had no labels, and the user said timing was not
  the problem. `443b135`: only an element's own content (the run after it
  in tree order) can make it a container, so panel rows over a dense page
  are no longer dropped as containers of the links behind them. `log = 2`
  now writes what each hint open made of every element of the active
  window, for the next report.

## 4. Complete commit log (non-merge, oldest first)

### Branches on `origin` (2026-10-30)

| Branch | Points at | Notes |
|---|---|---|
| `master` | `37b210c` | Release branch; the `latest` tag is here too |
| `element-hints` | `37b210c` | Where phases 2 to 7 were developed and merged |
| `usability-overhaul` | `0ebf562` | Phase 1 integration branch |
| `claude/relaxed-gates-osh5rb` | `08abd50` | PR #1 (release workflow) |
| `claude/serene-feynman-3zxeub` | `faf5234` | Agent session branch, merged |
| `claude/compassionate-keller-t659jq` | `e987c6a` | Agent session branch, merged |
| `bakup` | `cbdaf8c` | Unrelated prehistory (`m.c`) |

### `bakup` (prehistory)

| Hash | Date | What changed |
|---|---|---|
| `37fc028` | 02-10 | "int": `m.c`, Makefile, README, `.gitignore` |
| `b07d5c9` | 02-10 | Drag with `a` |
| `c3be97f` | 02-11 | Key-up always clears key flags; drag only with CapsLock on |
| `8bf3184` | 02-11 | Acceleration |
| `23758b6` | 02-12 | Clear velocity and accumulator when an axis goes idle (no coasting) |
| `59b091c` | 02-12 | Base speed tweak |
| `cbdaf8c` | 02-23 | Reinstall the hook and reset state on resume (`WM_POWERBROADCAST`); logging |

### `master`

Phase numbers refer to section 3. Merge commits: `a89f753`, `8126570`
(phase 1, worktree agents into `usability-overhaul`); `08abd50` (PR #1,
`claude/relaxed-gates-osh5rb` into `master`); `2a826f0` (`origin/master` into
`element-hints`); `8abb770`, `cf1fcbf`, `e583902`, `c4617c9`, `8fb609e`,
`8abcd74` (phase 6); `0979ae1`, `40657d6`, `30f7f62`, `735de23` (phase 7); the
last ten are all `worktree-agent-*` into `element-hints`.

| # | Hash | Date | Ph | What changed | Why / notes |
|---|---|---|---|---|---|
| 1 | `0964d95` | 03-02 | 0 | Initial `vim.c`, Makefile, README ("pq") | Single-file mouse control with a grid overlay |
| 2 | `c03c5e8` | 03-03 | 0 | Watchdog thread: reinstall the hook, restart the movement thread | Hook and thread silently dying |
| 3 | `0212997` | 03-04 | 0 | Suspend on session lock; reset motion after gaps over 2 s | Drift and stuck keys after lock or resume |
| 4 | `8f5d19c` | 04-10 | 0 | Hint hotkey `Shift+F` to `Alt+F` | Vim compatibility |
| 5 | `a267a1e` | 10-23 | 1 | `common.h` contract and `SPEC.md` | Contract first, so agents could build modules in parallel |
| 6 | `0f692d4` | 10-23 | 1 | `hint.c` grid overlay module | |
| 7 | `3217713` | 10-23 | 1 | `main.c`, `tray.c`, help window, README | |
| 8 | `fe00e2b` | 10-23 | 1 | `input.c`: hook, mouse mode, movement, indicator | |
| 9 | `0ebf562` | 10-23 | 1 | Integrate modules plus `config.c`; delete `vim.c` and `SPEC.md` | Review fixes: no hotkey while paused, Esc failsafe in the hook |
| 10 | `43063d0` | 10-23 | 2 | Contract for element hints | |
| 11 | `64a5805` | 10-23 | 2 | `uia.c` element scan, element mode, `uia_probe` | Label only clickable elements; settings `hint_mode`, `scan_scope`, `scan_timeout` |
| 12 | `c5e5472` | 10-23 | 2 | Withhold the left Alt while a menu is open | Menus close on Alt down, before the F arrives |
| 13 | `f6cb7d4` | 10-24 | 2 | Faster scan and painting; slow-app skip list; faster movement and scroll | Overlay opened in 15 to 50 ms; `scan_timeout` 1000 to 400 ms |
| 14 | `0bb6577` | 10-24 | 2 | Leave mouse mode on CapsLock press, not release | |
| 15 | `52e952a` | 10-24 | 2 | Count duplicate inner targets once in the container filter | Google result titles were dropped |
| 16 | `60df0e5` | 10-24 | 3 | `release.yml`, `build.bat`; drop duplicate `g_cfg` | GCC 10+ `-fno-common` link error |
| 17 | `92bf572` | 10-27 | 2 | `tools/hint_check` (CDP vs. scan) and the `g_scan_debug` hook | Measure misses instead of guessing |
| 18 | `af50fc5` | 10-27 | 2 | Keep actionable cards; clip to the largest Document | Clicks landed on the browser toolbar |
| 19 | `72e0cbb` | 10-27 | 2 | Only pattern-less list, table and tree items are wrappers; clip frames | Edits and Buttons have no pattern yet are clickable |
| 20 | `09b7fdb` | 10-28 | 4 | `Alt+hjkl` / `Alt+u/d` scrolling; text shortcuts; `snippet.c` | |
| 21 | `f90c515` | 10-28 | 4 | `Ctrl+Alt+L` keyboard lock, tray item, grey icon | |
| 22 | `673be4c` | 10-28 | 3 | Release cross-compiles on Ubuntu | MSYS2 lost `mingw-w64-i686-make` |
| 23 | `5b3d7e4` | 10-28 | 4 | Alt scroll to the nearest scrollable area; paste shortcuts by clipboard | `KEYEVENTF_UNICODE` lost the first characters in browsers and with the Korean IME |
| 24 | `faf5234` | 10-28 | 4 | Import and export in the shortcut editor | |
| 25 | `54c5551` | 10-29 | 4 | `#define WM_MOUSEHWHEEL` when missing | MinGW.org GCC 6.3 headers lack it |
| 26 | `56c6e10` | 10-29 | 5 | High-resolution waitable timer; absolute `SendInput` from a sub-pixel position | `Sleep(1)` ticked every ~6 ms, and relative moves went through "Enhance pointer precision" |
| 27 | `25e734f` | 10-29 | 5 | 8 helper threads; merge in completion order; ref-counted job | One slow window (Explorer, ~250 ms) held back the rest |
| 28 | `fcdc742` | 10-29 | 5 | Per-window scroll-area cache (8, LRU; 1 s fresh, 10 s stale) | 34 ms or more per Alt-scroll press |
| 29 | `e9f57b1` | 10-29 | 5 | Prescan when the hotkey's modifiers go down (`prescan`) | Labels ~4 ms after the hotkey instead of 50 to 110 ms |
| 30 | `267ab08` | 10-29 | 5 | Wait for the whole scan up to `scan_timeout` | The 100 ms after the first result lost slower windows |
| 31 | `1e8677d` | 10-29 | 5 | A second hotkey press rescans instead of opening the grid | |
| 32 | `3615694` | 10-29 | 5 | Scroll as a velocity stream once per display frame (`smooth_scroll`) | Replaces a whole notch, a 150 ms pause, then 40-unit chunks |
| 33 | `b54285f` | 10-29 | 5 | `Alt+u u` / `Alt+d d` jumps to the top or bottom (`scroll_end_tap_ms`) | Replaced later by `6622ce0` |
| 34 | `aad1eaa` | 10-29 | 5 | Glide to the end instead of a 64-event burst | The burst jumped in one frame with Windows animations off |
| 35 | `e04d6a0` | 10-29 | 6 | `vim.ini` and `vim.log` in `%APPDATA%\vim mouse` when the exe folder is read-only | Program Files installs |
| 36 | `394b361` | 10-29 | 3 | Manifest, `version.h`, icon, `build.yml` (`-Werror`, 32 and 64 bit) | |
| 37 | `4dda943` | 10-29 | 6 | `input_stop` before `uia_stop` on quit | Movement thread could be inside `uia_scroll_target` |
| 38 | `4b04fd5` | 10-29 | 6 | Raw input detects a removed hook; suspend on RDP disconnect or user switch; tray hints while paused | Windows removes slow hooks silently |
| 39 | `80970eb` | 10-29 | 6 | Replay queued keys through `hint_on_key`; no tray hints while locked; subgrid bound | Keys after a Tab now wait for the new monitor's scan; a ninth subgrid row wrote past `g_sub` |
| 40 | `8d7fcde` | 10-29 | 3 | Large icon sizes as PNG | 313 KB to 43 KB |
| 41 | `7d5de41` | 10-29 | 6 | Container search through a click-point grid | Every pair of candidates was compared |
| 42 | `4114c5f` | 10-29 | 3 | Draw the 256 px icon into its PNG | |
| 43 | `4dcbc6f` | 10-29 | 6 | Ignore `.claude/worktrees/` | Keep agent worktrees out of git |
| 44 | `6cf94df` | 10-29 | 6 | Repair a Run value that points at a missing exe | A moved exe left a dead autostart entry |
| 45 | `21aa24e` | 10-29 | 6 | Help window in balanced columns that fit the work area | The help window has to fit the screen |
| 46 | `2eebd25` | 10-29 | 6 | An unquoted Run value with spaces is one path when that file exists | `6cf94df` cut `C:\Program Files\...` at the first space |
| 47 | `4955523` | 10-29 | 6 | Read UTF-16 and ANSI `vim.ini`; keep comments before a section | Non-ASCII became U+FFFD and the editor saved it |
| 48 | `f0da5fd` | 10-29 | 6 | Type instead of paste when the clipboard is too large; refresh the editor on reload | The clipboard was wiped; rows mapped to the wrong shortcut |
| 49 | `348a968` | 10-29 | 6 | Slow skip per window (with pid), count all elements, exempt Explorer; wait for the foreground window up to 3 s | One slow Explorer window turned hints off for all of explorer.exe |
| 50 | `729e682` | 10-29 | 6 | Dropped scans do not judge slowness; WebView2 and CEF exempt; hotkey again forgets skips; redirect waits for refresh | Teams lost hints after Alt scrolling |
| 51 | `e987c6a` | 10-29 | 6 | Redirect only to areas in line with the pointer or within 64 px | Teams' chat list scrolled instead of the conversation |
| 52 | `8b0a194` | 10-30 | 7 | `scan_refresh` live label merging; an empty first scan retries up to 3 s | New or moved elements had no labels |
| 53 | `b9a8d6f` | 10-30 | 7 | Show a retried scan's active window at once; README wording | |
| 54 | `b474c89` | 10-30 | 7 | Bare keys outside text fields (focus worker); `Alt+h/l` back and forward; `Alt+,/.` sideways | `bare_keys`, `alt_nav` |
| 55 | `e51fecb` | 10-30 | 7 | A scan that finds nothing closes the hints; the grid only with `hint_mode = grid` | The grid opens only when asked for |
| 56 | `27e5efe` | 10-30 | 7 | Create every UIA engine under one lock; the scanner retries twice; log scan results | Concurrent engine creation failed about one start in three |
| 57 | `da32599` | 10-30 | 7 | The hook waits up to 100 ms for a pending focus answer; ignore other processes' focus events; keep the last good answer | Bare keys typed letters |
| 58 | `7855485` | 10-30 | 7 | Keep the scroll list warm (700 ms for 8 s); retry a failed scroll engine; reinstall the hook after 1 s | Bare scrolling waited up to 500 ms |
| 59 | `5ea3a61` | 10-30 | 7 | Tooltips and click-through windows are not menus; ComboBox or List without a Text pattern is not a text field | Chrome's link bubble disabled bare keys |
| 60 | `6622ce0` | 10-30 | 7 | `g g` / `Shift+g` scroll to the top / bottom (`key_scroll_end`); `u u` / `d d` just scroll fast twice | Same glide or jump as before, on vim's keys |
| 61 | `ba62724` | 10-30 | 7 | Send the bottom jump after Shift is released | Browsers read the held Shift and scrolled sideways |
| 62 | `37b210c` | 10-30 | 7 | Log why bare keys are off (`log = 1`) | Explorer still types after selecting a file (open) |
| 63 | `f01dca8` | 10-30 | 7 | `HISTORY.md` and `CLAUDE.md` | This guide |
| 64 | `1c2f611` | 10-30 | 7 | `CLAUDE.md`: push finished work to `master` | Every push to `master` republishes `latest` |
| 65 | `1c0a83b` | 10-30 | 7 | Ask Chromium's `OBJID_CARET` before UI Automation (`caret_visible`) | Bare keys scrolled in Zendesk's autocomplete fields: Chromium names the `aria-activedescendant` option as the focus |
| 66 | `35315a2` | 10-30 | 7 | Commit-log row for `1c0a83b` | |
| 67 | `ee4f7fb` | 10-30 | 7 | Tray "Update to latest version": download the latest release, rename the running exe to `.old`, restart with `--updated` (`update.c`) | Updating meant quitting, downloading and replacing `vim.exe` by hand |
| 68 | `7cf18c4` | 10-30 | 7 | Commit-log row for `ee4f7fb` | |
| 69 | `33a979f` | 10-30 | 7 | `keep_labels` (default 1): a refresh never moves, retires or lengthens a shown label; elements overlapping a shown one (IoU >= 0.5) get no second label; new ones get as many free labels as fit | Labels moved, changed letters or vanished between reading and typing them, so the user misclicked |
| 70 | `75ecb48` | 10-30 | 7 | Commit-log row for `33a979f` | |
| 71 | `f539e1f` | 10-31 | 7 | Alt keys act from `alt_resolve` with Alt still withheld (`alt_action_down`) | While a Chrome bubble (downloads, extensions, notifications) was open, the first Alt+j went to Chrome as Alt+J; only the second press scrolled |
| 72 | `e261176` | 10-31 | 7 | Commit-log row for `f539e1f` | |
| 73 | `46f5976` | 10-31 | 7 | Bare keys ignore Chrome's panels (`uia_chromium_panel`: a `Chrome_WidgetWin_*` owned popup without `WS_EX_NOACTIVATE` or `WS_EX_TOPMOST`); a panel's bare root focus counts as no text field | With the downloads or extensions panel open, bare `j`/`k` typed into Chrome; before `f539e1f` the replayed Alt had closed the panel |
| 74 | `27d9313` | 10-31 | 7 | Commit-log row for `46f5976` | |
| 75 | `be52799` | 10-31 | 7 | `Shift+g` scrolls to the bottom on press through UI Automation (`uia_scroll_end`: `ScrollPattern.SetScrollPercent`, own worker, 300 ms); the wheel after Shift release is only the fallback (`s_end_uia`) | The bottom jump waited for Shift to come up |
| 76 | `fac3443` | 10-31 | 7 | Commit-log row for `be52799` | |
| 77 | `515eb02` | 10-31 | 7 | After our own scroll, click, end jump or back/forward (`input_since_page_action`), the first scan of a Chromium window (`uia_chromium_window`) waits until `scan_settle` (default 650 ms) has passed; the prescan is skipped or dropped meanwhile. With `keep_labels`, `build_labels` plans codes for a quarter more elements (at least an alphabet's worth) and leaves those free | Labels sat where the page had been before a `j`/`k` scroll; the panel a hint click opened (Google account menu, Zendesk overlays) got no labels. Blink sends moved positions at most every 500 ms and new elements after 150 ms, and a full label code left no room for late elements |
| 78 | `965acd0` | 10-31 | 7 | Commit-log row for `515eb02` | |
| 79 | `443b135` | 10-31 | 7 | `filter_containers` counts only an element's own content: the run of candidates right after it in tree order whose click points are inside it (`content_end`). `log = 2`: every element of the active window with type, rect, name and why it was left out, plus every visible window (`detail_condition`, `detail_cache`, `DetailText`, written after the post); no prescan with `log = 2`; `uia_scan` takes a detail flag | Google's account menu and Zendesk's overlays still had no labels: their rows held the click points of the page links behind them and were dropped as containers. Not yet confirmed on Windows, hence the log |

## 5. Lessons learned: rules not to break

### Keyboard hook and input

- **Keep `kb_proc` fast. A slow callback silently loses the hook.** Windows
  removes a low-level hook whose callback exceeds `LowLevelHooksTimeout`
  without telling the process. Since `4b04fd5`, raw keyboard input notices a
  raw event with no matching hook call three times in a row and runs
  `replace_hook`; `7855485` cut the reinstall delay to 1 s. The hook's only
  deliberate wait is `uia_focus_wait(BARE_WAIT_MS)` (100 ms, `da32599`), and
  only while an ask is already pending and recent. Never add blocking I/O,
  UIA calls or `SendMessage` to the hook path. Post to the main window or a
  worker instead.
- **Never inject modifier key events.** `ba62724`: an injected Shift up/down
  pair races the physical keys and can leave Shift stuck. To act "after
  Shift", remember the action in the hook and run it from the key-up path
  (`scroll_end_shift_up`). Apps read modifier state with `GetKeyState` when a
  wheel event arrives, so Shift held during a vertical wheel turns into
  horizontal scrolling in browsers. To act while Shift is still held, do not
  use the wheel: `be52799` scrolls `Shift+g` to the bottom through UI
  Automation (`ScrollPattern.SetScrollPercent`), which ignores modifiers,
  and keeps the after-Shift wheel only as the fallback. Its hand-off between
  the hook and the movement thread (`s_end_uia`: ask, asking, done, failed,
  wheel) is lock-free; the hook only moves the state with
  `InterlockedCompareExchange`.
- **Alt reaches menus before your hotkey does.** `c5e5472`: menus and flyouts
  close on Alt down. While a popup is open or the hints capture keys, the
  left Alt is withheld (`alt_should_withhold`, `alt_resolve`) and replayed in
  front of any other key, or as a tap. Any new Alt combination must go
  through that path, not around it. The Alt keys (scrolling, back and
  forward, `g g`) act from `alt_resolve` with Alt still hidden
  (`alt_action_down`, `f539e1f`): replaying Alt in front of the first key
  sent Chrome Alt+J while its download, extension or notification bubble
  was open, so only the second press scrolled.
- **"A popup is open" must exclude tooltips and click-through windows.**
  `5ea3a61`: Chrome's link status bubble and Win32 tooltips are owned
  popups, so `popup_open` thought a menu was open and turned bare keys off.
  `46f5976`: Chrome's panels (downloads, extensions, permission prompts) are
  owned popups too, but they take the focus themselves. Chromium gives its
  menus `WS_EX_NOACTIVATE` and `WS_EX_TOPMOST` and its dropdowns
  `WS_EX_NOACTIVATE`; a panel has neither (`widget_hwnd_utils.cc`,
  `menu_host.cc`). `uia_chromium_panel` tests that, and only bare keys skip
  panels (`popup_open(TRUE, ...)`); Alt is still withheld over them so they
  stay open.
- **Release held state on every suspend.** Sleep and resume (`cbdaf8c` on
  `bakup`), session lock (`0212997`), and RDP disconnect or user switch
  (`4b04fd5`) all suspend input and reset motion. `reset_key_state()` must
  clear every pending tap or jump you add (it clears the pending `Shift+g`
  since `ba62724`).
- **Key-up always clears key state.** `c3be97f` (`bakup`): flags set on key
  down were cleared only under a condition, so they got stuck. Clear on every
  key up, whatever mode you are in.
- **Stop motion the moment a key is released.** `23758b6` (`bakup`): velocity
  smoothing and the fractional accumulator made the pointer coast after
  release. The same rule holds for scroll streams: "a release after that
  stops at once" (`3615694`).
- **Act on CapsLock press, not release** (`0bb6577`).
- **Queued keys must go through the same handler as live keys.** `80970eb`:
  keys typed during a scan are replayed through `hint_on_key`, so the ones
  after a Tab wait for the new monitor's scan.
- **Anything that opens the hints must have a way to close them.** `80970eb`:
  the tray cannot open hints while the keyboard is locked, because no key
  could close them. `4b04fd5`: hints opened from the tray while paused get
  their keys, Esc included.

### Bare keys (keys without Alt outside text fields)

- **Unknown means "type the letter".** `b474c89`: anything not known to be
  outside a text field (still asking, a caret, an open menu, a game, a remote
  desktop) types as usual. Keep this bias: a swallowed letter in a text field
  is worse than a missed scroll.
- **Do not throw away a good answer.** `da32599`: every focus change made the
  answer unknown; background apps and IME windows invalidated the foreground
  answer; a failed poll replaced a good one. The fix: ignore focus events
  from other processes, keep the last good poll answer, retry unknown answers
  quickly, and let the hook wait briefly for a pending ask.
- **In browsers, the UIA focus is not always where typing goes.** Chromium
  reports an input's `aria-activedescendant` target (the highlighted option
  of an autocomplete list) as the focused element, so Zendesk's CC, tags and
  assignee fields looked like a ListItem, bare keys stayed on and `j`/`k`
  scrolled while the user typed. Chromium makes no Win32 caret, but its
  window answers `WM_GETOBJECT(OBJID_CARET)` with a caret that is visible
  exactly while a text input has the focus (its `TEXT_INPUT_TYPE` is not
  none). `focus_classify` asks for that caret (`caret_visible`) before UI
  Automation (`1c0a83b`). Ask the focused HWND only; Chromium's transparent child
  `Chrome_RenderWidgetHostHWND` has a caret that never moves.
- **Classify by UIA patterns, not control type alone.** `5ea3a61`: a closed
  `<select>` (ComboBox) and a listbox (List) have a writable Value pattern but
  no Text pattern, so letters there only pick an item. They count as
  `FOCUS_OTHER` unless the Text pattern is present (editable comboboxes keep
  it).
- **A bare key has no Alt press to prefetch on.** `7855485`: the scroll worker
  keeps the list under the pointer warm (every 700 ms for 8 s after a press),
  and a slightly old list that would redirect waits only 150 ms for its
  refresh.
- **Text shortcuts always need Alt**, and a shortcut whose first letter is an
  Alt-key letter cannot start. `snippet.c` `first_key_taken` warns; update it
  when you add an Alt key (`6622ce0` added `g`).

### UI Automation

- **Create UIA engines one at a time.** `27e5efe`: `b474c89` started the focus
  worker's engine just before the scanner's, and creating two engines at the
  same moment made the scanner's fail about one start in three. Hints then
  stayed off for the whole session. Every engine now goes through
  `uia_engine_init` (under `engine_lock`), and the scanner retries its start.
  **Any new UIA worker must use `uia_engine_init`** and retry a failed init
  (the scroll and focus workers retry every 5 s, `da32599`, `7855485`).
  `uia_engine_init` takes `engine_lock` itself and the lock is not
  reentrant: never wrap it in another `engine_lock` (only the scroll worker's
  own `scroll_engine_init` is called under an explicit lock). The first draft
  of `be52799` did, which would have spun forever and blocked every other
  engine.
- **Never run UIA on the UI thread** (`common.h`): querying our own windows
  from there can deadlock.
- **A cancelled or partial scan must not change persistent state.** The
  prescan starts on every Alt press and is dropped as soon as `j`/`k`
  follows (`e9f57b1`). `729e682`: such a dropped scan, run while Teams was
  busy scrolling, came out slow and nearly empty and put Teams on the
  10-minute skip list. Only a scan that ran to its end may judge a window
  slow.
- **Remember per window, not per process.** `348a968`: `explorer.exe` runs
  every folder window, the taskbar and the desktop; one slow, mostly covered
  folder window turned hints off for all of them. The slow memory keys on
  `(hwnd, pid)` (pid against handle reuse) and counts every element the
  window answered with, not just the visible targets.
- **Lazy-tree apps are slow and empty on the first ask by design.**
  `lazy_tree_window` exempts `Chrome_WidgetWin_*`, Firefox, Explorer folder
  windows (`CabinetWClass`, `ExploreWClass`) and any window hosting a
  Chromium child (WebView2 and CEF, such as the new Teams, `729e682`). Add new
  such apps there rather than tuning timeouts.
- **Do not let a fast window decide when the overlay is done.** The
  history: `f6cb7d4` showed labels 100 ms after the foreground window's
  results; `25e734f` changed the merge order so "first" was no longer the
  foreground window; `267ab08` waits for the whole scan up to `scan_timeout`;
  `348a968` waits for the foreground window (`ClickList.fg_done`) up to the
  3 s hard limit (`SCAN_HARD_LIMIT_MS`). Keep both conditions.
- **Do not hold back fast windows behind slow ones.** `25e734f`: merge in
  completion order, and reference-count the job so a cancelled scan returns
  at once while late helpers clean up after themselves.
- **Container filtering is where labels get lost.** `52e952a`: count
  duplicate inner targets once, and mark containers before compacting so
  every check sees the full list. `af50fc5`: keep actionable cards with
  enough free space and move their click point off inner targets.
  `72e0cbb`: Edit and Button have none of the four patterns but are
  clickable, so only pattern-less list, table and tree items are wrappers.
  `7d5de41`: use the click-point grid (`click_grid_build`), never an O(n²)
  pass. `443b135`: only an element's own content counts. A panel drawn over
  a page holds the click points of the links behind its rows, which made
  each row a container. FindAll lists elements in tree order, so an
  element's descendants follow it without a break. `content_end` is that
  run of candidates whose click points are inside it, and only it counts
  as inner. Click points still avoid every smaller target on the element.
  Validate any filter change with `tools/hint_check` on real pages.
- **UI Automation cannot say what is on top inside a page.** Chromium's
  synchronous `ElementFromPoint` is an approximate bounds search that it
  calls "wrong in cases of z-index" (`CachingAsyncHitTest`), and it hides
  the rest of the page only for an `aria-modal` dialog. So the page
  elements under a non-modal panel keep their labels, drawn over the
  panel.
- **Clip page elements to the page.** `af50fc5`, `72e0cbb`: clip to the
  largest Document, and clip each frame's Document to the one it overlaps, or
  click points land on the browser toolbar.
- **Labels never change meaning while the overlay is open.** `8b0a194`:
  refreshed elements keep their labels, removed ones are retired for the
  rest of the open, and new ones are allocated prefix-free in shortlex order.
  A label the user is reading must not move either: with `keep_labels`
  (default 1) a merge freezes a shown label's text, rect and click point,
  treats an incoming element that overlaps a shown one by at least half of
  their union as the same element, and stops allocating instead of
  lengthening a shown label (`make_room` may only open a retired one). The
  remaining new elements stay unlabelled until the hotkey reloads. The old
  follow-and-retire merge is `keep_labels = 0`. The cost is deliberate: a
  label whose element moved far or went away still clicks where it is
  drawn, which is where the user saw it. Matching by rect alone cannot tell
  "moved" from "replaced" (UIA identity is not carried in `ClickTarget`), so
  following elements gave a label a new letter or a new place between the
  user reading it and typing it.

- **Chromium's tree lags the screen.** Blink sends moved elements'
  positions to the browser process at most every 500 ms unless the focused
  element moved, and new elements 150 ms after they appear
  (`ax_object_cache_impl.cc`: `GetLocationSerializationDelay`,
  `GetDeferredEventsDelay`); a document still loading sends no location
  changes at all. UI Automation answers from that copy, and `IsOffscreen` is
  computed from it too. So a scan right after a scroll labels the old
  positions, and a scan right after a click can miss the panel it opened or
  see it at its first, unplaced position. `515eb02`: the first scan waits
  `scan_settle` after our own scroll, click, end jump or back/forward, in
  Chromium windows only (native apps answer synchronously). The wait is
  measured from our last `SendInput`/`PostMessage`, so physical mouse wheel
  scrolls and clicks are not covered (there is no mouse hook). Any new
  action that changes a page (a new key that scrolls, clicks or navigates)
  must call `page_acted` in `input.c`.
- **A full label code cannot take late elements.** Vimium-style labelling
  uses the whole code space, and `keep_labels` may not lengthen a shown
  label, so before `515eb02` an element that appeared during the open got
  one of the few codes left over (0 to 21 with the default alphabet) or
  none. `build_labels` now plans for a quarter more elements, at least an
  alphabet's worth.

### Scrolling and movement

- **Measure timing; do not trust `Sleep(1)`.** `56c6e10`: with
  `timeBeginPeriod(1)` it ticked every ~6 ms (spikes to 16 ms) in this
  windowless process. Use the high-resolution waitable timer and opt out of
  Windows 11 timer throttling. Without that timer (Windows 7, 8, early 10)
  hold the 1 ms period only while something moves (`4b04fd5`).
- **Move the pointer with absolute `SendInput`.** Relative moves go through
  "Enhance pointer precision", so speed varied from 0.57x to 1.79x
  (`56c6e10`). Keep a sub-pixel position that resyncs with the real cursor.
- **Send wheel deltas per display frame** (`3615694`). A tap still delivers
  exactly one notch. `smooth_scroll = 0` is for apps that treat each wheel
  message as a notch.
- **Do not burst a long scroll.** `aad1eaa`: with Windows animations off,
  Chrome applied a 64-event burst in one frame. Glide with growing speed, and
  split large frame amounts into several 16-bit wheel deltas.
- **Redirecting the wheel can scroll the wrong panel.** `729e682`: a cached
  list up to 10 s old sent the wheel to the nearest area. `e987c6a`: Teams
  does not report its conversation as scrollable, so a redirect only goes to
  an area in line with the pointer or at most 64 px beside it. If in doubt,
  leave the wheel at the pointer.

### Settings, files, Windows integration

- **The exe folder may be read-only.** `e04d6a0`: `vim.ini` and `vim.log`
  move to `%APPDATA%\vim mouse`. `main.c` calls `config_path()` once before
  any worker thread can ask for it.
- **Users edit `vim.ini` in any editor.** `4955523`: decode a UTF-16 BOM and
  fall back to the ANSI code page (with a warning) for invalid UTF-8.
  Otherwise non-ASCII text becomes U+FFFD, and the shortcut editor saves the
  damage back.
- **Rewrite only what you own.** `shortcuts_save` rewrites only the
  `[shortcuts]` section (`09b7fdb`) and keeps comments right before the next
  section header (`4955523`). Our own writes reload quietly
  (`app_config_written`).
- **A list in a window must be rebuilt when its data reloads.** `f0da5fd`:
  after a reload, editor rows mapped to other shortcuts and Delete removed the
  wrong one.
- **Never destroy the user's clipboard.** `5b3d7e4` pastes through the
  clipboard, restores it afterwards and keeps the text out of clipboard
  history. `f0da5fd`: when the clipboard is too large to save or memory runs
  out, type instead of paste.
- **Run registry values come in many shapes.** `6cf94df`: repair only a value
  whose exe no longer exists, since another existing exe may be a second
  copy that owns the value. `2eebd25`: an unquoted value with spaces is one
  path when that file exists. Compare paths case-insensitively.
- **A running exe can be renamed but not overwritten or deleted.** The
  self-update (`update.c`) downloads to `<exe>.new`, renames the running exe
  to `<exe>.old`, moves the download into its name (so the Run value and
  `vim.ini` stay valid) and starts it with `--updated`. If a step fails, the
  renames are undone, so the next start always finds a working exe. The new
  process starts while the old one still owns the instance mutex: with
  `--updated` it waits for the mutex (`WAIT_ABANDONED` counts, since the old
  one exits holding it) instead of saying "already running", and only then
  deletes `<exe>.old`, retrying for 2 s while the old image is unmapped.
  Never do the download on the UI thread, and validate the file (`MZ`, `PE`,
  Content-Length) before it can replace a working exe.
- **Shut down users before providers.** `4dda943`: `input_stop` must come
  before `uia_stop`. Tearing down the scroll cache first could close the
  event the movement thread waits on and delete its critical section.

### Build and toolchain

- **Two toolchains must both compile the code.** Local builds use 32-bit
  MinGW.org GCC 6.3 (`build.bat`, README); CI uses mingw-w64 on Ubuntu, 32
  and 64 bit, with `-Werror` (`394b361`). Guard every newer SDK constant
  with `#ifndef` as the top of each `.c` file does (`54c5551` was
  `WM_MOUSEHWHEEL`). UIA interfaces are hand-declared in `uia.c`.
- **No tentative definitions across files.** `60df0e5`: GCC 10+ defaults to
  `-fno-common`; `g_cfg` is defined once, in `main.c`.
- **CI images change under you.** `673be4c`: MSYS2 dropped the 32-bit make,
  so the release moved to Ubuntu cross-compiling.
- **`vim.ico` is committed.** CI uses it as is; regenerate with `make icon`
  after changing `tools/make_icon.c` (`394b361`, `8d7fcde`, `4114c5f`).

### Process (agents and vibe coding)

- **Contract first.** Phase 1 worked because `common.h` was written before
  parallel agents built the modules (`a267a1e`). Change `common.h` first when
  a change crosses modules.
- **Parallel agent work needs review and an integration pass.** Phase 1
  ended with `0ebf562` "review fixes". In phases 5 to 7, regressions were
  found and fixed hours after the feature that caused them: `27e5efe` fixed
  `b474c89`, and `267ab08` and `729e682` fixed side effects of `e9f57b1`. After
  adding a worker, a prescan or a cache, test cold start several times and
  test cancelled paths.
- **Ignore agent worktrees** (`.claude/worktrees/`, `4dcbc6f`).
- **Write the symptom and root cause in the commit body.** The best commits
  here (`56c6e10`, `348a968`, `729e682`, `27e5efe`) state what was measured,
  why it happened and what changed. Keep doing that; this file depends on it.
- **Add logging for problems you cannot reproduce.** `27e5efe` logs scan
  results and scanner start failures; `37b210c` logs bare-key reason
  changes; `443b135` adds `log = 2`, which lists every element of the
  active window at each hint open with why it got no label. Log reasons,
  never typed keys. A log written during a scan must be buffered:
  `app_log` opens `vim.log` for every line.

## 6. Behaviour changed on purpose: do not revert

| Current behaviour | Set by | Replaced |
|---|---|---|
| `Alt+F` opens hints | `8f5d19c` | `Shift+F` |
| Hotkeys are matched in the low-level hook | `fe00e2b`, `0ebf562` | `RegisterHotKey` in `vim.c` |
| CapsLock tap toggles mouse mode, hold is momentary; `Shift+CapsLock` is the real CapsLock | phase 1, `0bb6577` | CapsLock *state* meant mouse mode |
| Hints label UIA elements; the grid opens only with `hint_mode = grid` | `64a5805`, `e51fecb` | Grid only (phase 0 and 1); grid as automatic fallback (`64a5805` to `1e8677d`) |
| A scan that finds nothing retries up to 3 s, then closes the hints | `8b0a194`, `e51fecb` | Opening the grid |
| Hotkey again rescans (and forgets slow-window skips) | `1e8677d`, `729e682` | Opening the grid |
| Labels refresh while open and never change meaning; shown labels stay put (`keep_labels = 0` restores the moving merge) | `8b0a194`, `33a979f` | Static labels; labels that followed their elements, vanished and grew letters (`8b0a194` to `33a979f`) |
| `Alt+h` / `Alt+l` go back / forward; `Alt+,` / `Alt+.` scroll sideways (`alt_nav = 0` restores) | `b474c89` | `Alt+h/l` scrolled sideways (`09b7fdb`) |
| `g g` / `Shift+g` scroll to the top / bottom; `Alt+g g` / `Alt+Shift+g` in text fields | `6622ce0` | `Alt+u u` / `Alt+d d` (`b54285f`) |
| The bottom jump fires on press through UI Automation; where that cannot scroll, the wheel goes after Shift is released | `be52799`, `ba62724` | Wheel at once (sideways in browsers, before `ba62724`); wheel only after Shift release (`ba62724` to `be52799`) |
| Scroll-to-end glides | `aad1eaa` | 64-event burst (still used with `smooth_scroll = 0`) |
| After our own scroll or click in a Chromium window, the first scan waits `scan_settle` (650 ms; `scan_settle = 0` restores) | `515eb02` | Scan at once, labelling where the page had been |
| With `keep_labels`, the first labels leave free codes (a quarter more, at least an alphabet's worth); up to three one-letter labels become two letters | `515eb02` | Exactly as many codes as elements |
| Bare keys outside text fields (`bare_keys = 0` restores) | `b474c89` | Alt always needed |
| Text shortcuts are pasted through the clipboard (`shortcut_method = type` restores) | `5b3d7e4` | Typed with `KEYEVENTF_UNICODE` |
| Settings next to the exe, else `%APPDATA%\vim mouse` | `e04d6a0` | Exe folder only |
| Defaults: movement 600 to 2400 px/s over 0.45 s; scroll 10 to 40 notches/s; `scan_timeout` 400 ms | `f6cb7d4` | 500 to 1500 over 0.65 s; 5 to 20 notches/s; 1000 ms |

## 7. Open threads

- **Bare keys type in Explorer after a file is selected** (`37b210c`). Every
  probe says the focus is not a text field, with no caret and no popup, yet
  the user sees letters typed. With `log = 1`, `bare_key_ok` writes a "bare
  keys: on/off" line to `vim.log` whenever its reason changes: the popup that
  counts as open (`popup_open` can now name it) or the focus answer with the
  focused window's class and any caret (`bare_log` in `input.c`). Next step:
  collect that log from the failing setup. Suspects worth checking are an
  invisible owned popup, a caret owned by a different window, and the
  `FOCUS_UNKNOWN` path timing out.
- **Zendesk autocomplete fields** (`1c0a83b`, the browser caret check in
  `focus_classify`, section 5) was built and reasoned from Chromium's source,
  not tried on Windows. If bare keys still scroll in a Zendesk field, set
  `log = 1`: `vim.log` says "focus: no text field" there, and the next step
  is to see which element UI Automation names and whether the caret answers.
- **Late browser trees** (`515eb02`) was reasoned from Chromium's source
  (`GetLocationSerializationDelay`, `GetDeferredEventsDelay`), not tried on
  Windows. Check on a Google results page: click the account avatar through
  a hint, press `f` at once, and expect the status text briefly, then labels
  on the panel. Also scroll with `j` and press `f` straight away. Still
  open: physical mouse scrolls and clicks (no settle wait); page elements
  behind a non-modal overlay are still in the tree and get labels drawn
  over it (Chromium hides the rest of the page only for `aria-modal`
  dialogs, `IsBlockedByAriaModalDialog`).
- **Google's account menu and Zendesk overlays without labels.** After
  `515eb02` the user reported the account menu (from a Google results page)
  still unlabelled, and not a timing problem. `443b135` stops dropping
  panel rows as containers of the links behind them. That cause was found
  by reading the code and is confirmed only in a Linux harness. If labels
  are still missing, the `log = 2` lines for the menu say which case it is.
  Not listed at all: Chromium does not expose it. `offscreen (UI
  Automation)` or `zero area`: Chromium's geometry for it is wrong (a
  transform it never re-sends is one suspect). `hidden by a window`: a
  window on top, named in the window lines. Something else: a filter here.
  Also ask whether "profile" meant Chrome's own profile button: its menu
  is a separate owned popup, which the window lines show.
- **Version** is still `1.0.0` in `version.h`; no `v*` tag exists yet.

## 8. How to change things efficiently

### Add or change a key binding

1. `common.h`: add the `UINT key_*` field to `Config`, with its default in the
   comment.
2. `config.c`: default in `config_set_defaults` (`parse_key(L"g", ...)`),
   parsing in `process_mouse` (`apply_key`), and the template line in
   `build_default_ini` (`add_key_line`).
3. `input.c`: an `ACT_*` value, the match in `binding_action` (mouse mode)
   and/or `alt_key_action` (Alt and bare keys), the press in
   `apply_action_down` and the release in `handle_up`. Clear any pending
   state in `reset_key_state`.
4. `snippet.c` `first_key_taken`: if it is a new Alt-key letter.
5. `tray.c` `build_help_rows`: the help window.
6. `README.md`: the Keys tables, Settings, and Troubleshooting if it takes a
   letter away from apps.

`6622ce0` is the reference example: it touched all of these.

### Add a setting

`common.h` field and comment, then `config.c`: `config_set_defaults`, the
right `process_*` function with an `apply_*` helper (range-checked, with a
warning on bad values), and `build_default_ini`. Then the `README.md`
Settings section. If a running thread caches the value, react in
`input_config_changed` (input) or on `WM_APP_RELOAD_CONFIG`. Reload is live:
the watcher thread notices the file change.

### Change hint scanning or filtering

`uia.c`: `scan_window_uia` (candidate tests), `filter_containers` and
`choose_outer_click_point` (containers and click points), `slow_check` and
`lazy_tree_window` (skip rules), `uia_scan_monitor` (window list and
merging). Verify with `make probe` (`tools/uia_probe.exe -v`) and, for web
pages, `make check` then `node tools/hint_check.mjs <url>`, which reports
misses with the drop reason. For a window or panel without labels, `log = 2` lists every element of
the active window with the reason. Overlay timing lives in `hint.c` (`start_scan`,
`begin_scan`, `settle_wait_ms`, `adopt_prescan`, `finalize_scan`,
`scan_limit_ms`, `SCAN_HARD_LIMIT_MS`).

### Change scrolling or movement

`input.c` `movement_thread` (streams, glides, timer), `scroll_press` /
`scroll_release`, `scroll_end_press`, `post_wheel`, `send_wheel_jump`.
Targeting and its cache: `uia.c` `uia_scroll_target`, `scroll_area_answer`,
`scroll_side_gap`, the `SCROLL_*_MS` constants. Re-check the Teams cases in
section 5 after any targeting change.

### Add a tray menu item

`tray.c`: an `IDM_*` id, `append_item` in `show_menu`, handling in
`tray_on_command`. State shown in the menu or icon comes from `tray_update`
(`WM_APP_STATE_CHANGED`).

### Self-update

`update.c`: the download URL (`UPDATE_URL`, the rolling `latest` release that
`release.yml` publishes), the worker (`download`, `check_download`), and the
swap and restart on the main thread (`install_and_restart`). `main.c` calls
`update_wait_for_old` when the instance mutex already exists and
`update_cleanup` after loading the settings. Renaming the release asset or the
repository breaks updates of every copy already installed, so keep
`vim.exe` at `releases/latest/download/vim.exe`. Test on Windows: an update
from an older build, "up to date" right after one, a read-only folder (a copy
under `C:\Program Files`), and no network.

### Bump the version or change the icon

Version: `version.h` only (numbers and both strings). Icon: edit
`tools/make_icon.c`, run `make icon` on Windows, and commit `vim.ico`.

### Build and check

- Local (Windows): `mingw32-make`, or double-click `build.bat` (finds MinGW in
  `C:\MinGW` or `C:\msys64\mingw32`). Tools: `mingw32-make probe check icon`.
- Linux cross-compile, the same as CI (needs `gcc-mingw-w64-i686`):
  `make CC=i686-w64-mingw32-gcc WINDRES=i686-w64-mingw32-windres CFLAGS="-Wall -Wextra -O2 -std=gnu11 -Werror"`,
  then the same with `probe check`. Also try `x86_64-w64-mingw32-*`, since CI
  builds both.
- CI: `build.yml` (every branch except `master`, and PRs: 32 and 64 bit,
  `-Werror`, app and tools); `release.yml` (`master` and `v*`: 32-bit build,
  zip, rolling `latest` release).
- There are no automated tests. Behaviour is verified on Windows; the logs
  (`log = 1`) and the two tools are the debugging aids.

### Conventions seen in the history

- Commit subjects are plain imperative sentences about user-visible
  behaviour ("Keep hints working in Explorer windows"), not code terms. The
  body gives the symptom, the measured cause, the change, and any new
  setting with its default and how to turn it off.
- Every new behaviour gets a setting to turn it off (`prescan`,
  `smooth_scroll`, `bare_keys`, `alt_nav`, `alt_scroll_auto`, `scan_refresh`,
  `keep_labels`, `scan_settle = 0`, `scroll_end_tap_ms = 0`,
  `shortcut_method = type`).
- README, help window, default `vim.ini` and the `common.h` comments change
  in the same commit as the behaviour.
- Comments explain *why* (often with the app that forced the rule); match
  that density. Newer Windows constants are guarded with `#ifndef` at the top
  of the file that uses them.
