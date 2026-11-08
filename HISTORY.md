# vim mouse: development history and lessons

Read this before changing the code. It records how the program got to its
current shape, which mistakes were already made and fixed, which behaviour
was changed on purpose, and where to make common changes. It covers every
commit on `master` from `0964d95` (2026-03-02) to `8bca056` (2026-11-03): 126
commits, 112 of them non-merge. It also covers the 7 commits of the unrelated
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
| `common.h` | 480 | The contract between modules: `WM_APP_*` messages, `Config` (every setting with its default in a comment), every cross-module function. Read it first. | `Config g_cfg`, `Hotkey`, `KMOD_*`, `ClickList`, `FOCUS_*` |
| `labelbox.h` | 95 | Where a label box goes: at its element's top-left corner, inside the monitor, and clear of the boxes already placed (`labels_apart`). Pure geometry shared by `hint.c` and `tools/hint_check.c`, so the checker reports the boxes the overlay draws. | `labelbox_natural`, `labelbox_apart`, `LABEL_PLACE_TRIES` |
| `main.c` | 576 | Entry point, single instance (waits for the old instance after an update), DPI awareness, main window and message routing, settings-file watcher thread, logging, startup and shutdown order. | `main`, `main_wnd_proc`, `app_log`, `app_reload_config`, `app_config_written`, `watch_thread_proc`, `dpi_for_monitor` |
| `config.c` | 2272 | `vim.ini` location (exe folder or `%APPDATA%\vim mouse`), defaults, parsing, warnings, key-name parsing and formatting, default-file template, text-shortcut table (locked) and the `[shortcuts]` rewrite, import and export. | `config_set_defaults`, `config_load`, `config_path`, `process_general` / `process_mouse` / `process_hints`, `apply_*`, `build_default_ini`, `read_text_file`, `shortcuts_save`, `parse_key`, `parse_hotkey`, `parse_clock` |
| `input.c` | 3960 | The single keyboard dispatcher: `WH_KEYBOARD_LL` hook, hotkeys, mouse mode, Alt and bare keys, text-shortcut capture, keyboard lock, Alt withholding for menus, movement and scroll thread, scroll-to-end glides, mode indicator, session and power handling, hook-loss detection, keeping the CapsLock state off, telling the hints about page actions, hiding the pointer outside mouse mode (`WH_MOUSE_LL` hook, pointer thread), putting it back after a hint click, the `0 0` jump to the middle of the screen, `Esc` leaving a text field (`esc_unfocus`: armed at the press, sent to the focus worker at the release), a held `j k u d` leaving it (`hold_unfocus`: armed at the press, `OWN_HOLD` holds its repeats back, the scroll starts from the worker's answer). | `kb_proc`, `handle_key` / `handle_down` / `handle_up`, `handle_hotkeys`, `binding_action`, `alt_key_action`, `apply_action_down`, `bare_key_ok`, `popup_open`, `alt_should_withhold`, `scroll_press`, `scroll_end_press`, `scroll_end_shift_up`, `movement_thread`, `raw_input_check`, `replace_hook`, `reset_key_state`, `caps_check`, `page_acted_at`, `ptr_update`, `mouse_proc`, `own_move`, `center_key_down`, `alt_center_down`, `center_pointer`, `tap_defer`, `tap_answer`, `unfocus_key_down`, `unfocus_cancel`, `hold_key_down`, `hold_repeat`, `hold_key_up`, `hold_answer`, `hold_cancel`, `input_thread_proc` |
| `hint.c` | 4186 | The click-hint overlay on the UI thread: element mode (prefix-free labels), grid mode with subgrid, the status bar, queued keys during a scan, prescan adoption, the wait for a browser to catch up (started by the action itself, hints open or not; the scan starts on its first flush where the browser does not animate), live refresh merging, the watch on a browser page still loading (status "Page loading", content reports, one relabel), monitor cycling, the grid key, clicks held back until Shift and Ctrl are up. | `hint_open`, `hint_prepare`, `hint_page_acted`, `hint_flush_done`, `hint_on_elements`, `hint_on_key`, `hint_hotkey_again`, `start_scan`, `begin_scan`, `settle_poke`, `settle_arm`, `settle_begin`, `settle_rebase`, `settle_step`, `settle_answered`, `settle_done`, `settle_early_start`, `settle_blocks`, `scroll_event_proc`, `load_note`, `load_watch`, `load_event_proc`, `load_content_in`, `relabel_all`, `scan_ready`, `adopt_prescan`, `finalize_scan`, `merge_scan`, `alloc_labels`, `replay_queue`, `paint_overlay`, `act_at`, `click_when_free`, `toggle_view`, `place_labels` |
| `uia.c` | 6768 | UI Automation through hand-declared COM vtables. Five MTA workers: the element scan (one thread plus 8 helpers, which search a classic window's parts at the same time; browser windows get a second condition that adds keyboard-focusable elements), the page flush (a hit test on a thread of its own, so a scan never delays it), the scroll-target cache, the `g g` / `Shift+g` scroll to the top / bottom and the focus classifier for bare keys (which also moves the focus from a text field to the page on `Esc`). All engines are created under one lock. | `uia_worker`, `uia_helper`, `scan_root_item`, `scan_split_item`, `scan_titlebar_item`, `scan_window_finish`, `scan_filter`, `scan_window_uia`, `add_unlisted_windows`, `scan_web_window`, `focusable_left_out`, `label_stand_in`, `scan_layout`, `rescue_offscreen`, `filter_containers`, `cover_overlays`, `scan_reject`, `detail_flush`, `click_grid_build`, `slow_check`, `lazy_tree_window`, `uia_chromium_window`, `uia_browser_window`, `uia_page_point`, `page_busy`, `msaa_state`, `uia_flush`, `flush_worker`, `flush_at`, `engine_lock`, `uia_engine_init`, `scroll_worker`, `uia_scroll_aim`, `scroll_end_worker`, `scroll_end_at`, `uia_scroll_end`, `focus_worker`, `focus_classify`, `uia_focus_wait`, `uia_focus_leave_down`, `uia_focus_leave_hold`, `uia_focus_leave_result`, `leave_step`, `leave_erase`, `leave_look`, `leave_act`, `leave_page_document`, `browser_process` |
| `tray.c` | 1456 | Tray icon and menu (with the "Schedule shutdown" submenu), balloons, the help window (balanced columns), start-with-Windows Run value and its repair. | `tray_init`, `show_menu`, `append_shutdown_menu`, `tray_on_command`, `build_help_rows`, `help_layout`, `startup_enabled`, `startup_repair`, `run_value_path` |
| `snippet.c` | 1031 | Text-shortcut editor window (tray, "Text shortcuts..."), import and export, and inserting a shortcut's text by clipboard paste (saved and restored) or by typing. | `snippet_editor_open`, `snippet_editor_refresh`, `first_key_taken`, `snippet_insert`, `save_clipboard`, `restore_clipboard`, `paste_text` |
| `update.c` | 483 | Self-update (tray, "Update to latest version"): WinINet download of the latest release's `vim.exe` on a worker, byte comparison with the running exe, rename-then-restart with `--updated`, startup cleanup of `<exe>.old` / `<exe>.new`. | `update_start`, `update_worker`, `download`, `check_download`, `update_on_done`, `install_and_restart`, `update_wait_for_old`, `update_cleanup` |
| `shutdown.c` | 1137 | Scheduled shutdown (tray "Schedule shutdown", `shutdown_at`): a one-off from the tray and the next `shutdown_at` day, whichever is first, on wall-clock UTC; one thread timer; the countdown window (Cancel, 10 more minutes, Shut down now); the "At a time or after..." window; `ExitWindowsEx`. Skips a shutdown the computer slept through. | `sd_check`, `next_due`, `clock_next`, `daily_consume`, `schedule_once`, `power_off`, `missed`, `countdown_open`, `parse_when`, `shutdown_next` |
| `touchpad.c` | 235 | `touchpad_off`: the touchpad is off while vim mouse runs, not paused and the keyboard not locked, and back on when paused, locked, quit or the setting is turned off. Reads the state with `SPI_GETTOUCHPADPARAMETERS`, switches it with an injected Ctrl+Win+F24 once no modifier is held, checks the result on a thread timer (phase 13). | `touchpad_update`, `touchpad_stop`, `set_goal`, `step`, `tp_state`, `press_toggle` |
| `zoom.c` | 485 | `Alt+=` / `Alt+-` (and `=` / `-` in mouse mode): a two-finger touch pinch around the pointer while the key is held (phase 19). Touch injection declared by hand (`VmTouchInfo`, size checked at compile time) and loaded with `GetProcAddress`; one thread of its own; puts the pointer back after each grip. | `zoom_press`, `zoom_release`, `zoom_cancel`, `zoom_thread`, `pinch`, `grip_place`, `grip_span`, `touch_send`, `pointer_back` |
| `version.h` | 12 | The one place to bump the version (numbers and both strings). | `VIM_VERSION_*` |
| `vim.rc`, `vim.manifest`, `vim.ico` | | Resources: icon, manifest (Common Controls v6, per-monitor DPI, supported OS), version info. `vim.ico` is generated by `tools/make_icon.c` and committed. | |
| `vim.uiaccess.manifest`, `tools/uiaccess_setup.ps1` | | The UIAccess build (`make uiaccess`): the manifest with `uiAccess="true"`, and the script that signs the exe with a certificate made on the machine, trusts it, installs the exe under Program Files and restarts vim mouse (phase 11). | `app_has_uiaccess` |
| `tools/uia_probe.c` | 320 | Console tool: lists what the scan finds on the monitor under the cursor (`-v` for every target, `-d` for every element of the active window with what became of it, as `log = 2`; `-n` runs, `-w` seconds before each, `-o` file). Read-only. `make probe`. | |
| `tools/hint_check.c`, `tools/hint_check.mjs` | 1941, 804 | Compares Chrome's DOM clickables (over CDP) with the real scan and reports misses with the drop reason. `make check`, then `node tools/hint_check.mjs [url ...]`. | `g_scan_debug` hook in `uia.c` |
| `tools/ax_sim/` | 364 | Linux check of the element filters: Chromium's real tree for a mock page (headful, AT-SPI) run through `uia.c`'s own `filter_containers`, `cover_overlays`, `rescue_pick` and `target_add`, plus a copy of the focusable-only test. `tools/ax_sim/run.sh <page> [steps]`, `OLD=<rev>` diffs against an older `uia.c`, `INPUT=<file>` replays a saved input (one written from a Windows scan: `pages/google_menu_win.txt`). | `run.sh`, `mk.py`, `conv.py`, `pages/` |
| `tools/load_probe.c`, `tools/load_probe.mjs`, `tools/vim_post.c` | 117, 118, 109 | How a Chromium page reports its loading: slow local pages (streamed, a slow image, a slow first byte) in a temporary Chrome driven over CDP, with `uia.c`'s `page_busy` and the events `hint.c`'s page watch takes printed against the navigation. `--open` also opens and closes the running vim mouse's hints by message (`vim_post`, which can bring a window to the front and give it back) for a live test with `log = 1`. `make loadprobe`, then `node tools/load_probe.mjs [stream,slowimg,ttfb] [--open]`. | |
| `tools/shutdown_test.c` | 459 | Console check of `shutdown.c` with a fake clock and a fake `ExitWindowsEx` (never shuts down): typed times, `vim.ini` values, daylight saving, sleep, late wake, the one-off against `shutdown_at`. `make shutdown-test`; runs on Windows or under Wine. | |
| `tools/make_icon.c` | 512 | Draws `vim.ico` (large sizes stored as PNG). `make icon`. | |

### Threads and how modules talk

| Thread | Owner | Does |
|---|---|---|
| Main / UI | `main.c` | Message loop; the overlay (`hint.c`), tray and help (`tray.c`), editor (`snippet.c`), the scheduled shutdown's timer and windows (`shutdown.c`), the touchpad switch (`touchpad.c`). Everything that touches windows we own. Every `WM_APP_*` message in `common.h` lands here, and so do Chromium's scroll reports (`EVENT_SYSTEM_SCROLLINGSTART`/`END`, an out-of-context WinEvent hook in `hint.c`). |
| Input (highest priority) | `input.c` `input_thread_proc` | Installs the low-level hook (`kb_proc`), WinEvent focus hooks, raw keyboard input (hook-loss check) and the power/session window; owns the mode indicator window. |
| Pinch zoom | `zoom.c` `zoom_thread` | Started and stopped with the input thread. Waits for a zoom key's press (`zoom_press`), then injects the two touch contacts every 8 ms until the release (`ZOOM_FRAME_MS`, the 1 ms timer only while a pinch runs). |
| Pointer | `input.c` `ptr_thread_proc` | `hide_pointer`: swaps the system cursors for blank ones and back (`SetSystemCursor`, `SPI_SETCURSORS`) when the input thread signals. |
| Movement | `input.c` `movement_thread` | Pointer movement, scroll streams, glides. Ticks on a high-resolution waitable timer (1 ms). Polls `uia_scroll_aim` (never waits) and calls `uia_scroll_end`; moves the mode indicator with each pointer move (`indicator_moved`). |
| UIA scan + 8 helpers | `uia.c` `uia_worker`, `uia_helper` | Scans windows in parallel (a classic window's parts too: one queue of work items per scan), merges in completion order, posts cumulative `ClickList`s to the main window (`WM_APP_ELEMENTS`). |
| UIA scroll worker | `uia.c` `scroll_worker` | Per-window cache (8 windows, LRU) of scrollable areas for Alt and bare scrolling, kept warm after a press. |
| UIA scroll-end worker | `uia.c` `scroll_end_worker` | `Shift+g`: `ScrollPattern.SetScrollPercent` to 100 on the scrollable area at a point. Started on first use. |
| UIA focus worker | `uia.c` `focus_worker` | Decides whether the focus is in a text field, after focus and foreground events and polls; the hook reads the cached answer. On `Esc` (`esc_unfocus`) looks at the field at the press and moves the focus to the page after the release; for a held `j k u d` (`hold_unfocus`) looks once the key has been held long enough, takes the typed letter back and moves the focus. |
| Settings watcher | `main.c` `watch_thread_proc` | Notices `vim.ini` changes and posts `WM_APP_RELOAD_CONFIG`. |
| Update download (on demand) | `update.c` `update_worker` | Downloads `<exe>.new`, compares it with the running exe, posts `WM_APP_UPDATE_DONE`. The swap and restart run on the main thread. |

Flow of a hint open: Alt goes down, so the hook posts
`WM_APP_HINT_PREPARE(1)` and the overlay starts a prescan (`e9f57b1`). The F
key completes the hotkey, so the hook turns key capture on itself and posts
`WM_APP_HINT_OPEN`. The overlay adopts the running scan, and `uia.c` posts
partial and final `ClickList`s. Typed keys arrive as `WM_APP_HINT_KEY`; the
overlay clicks through `mouse_click_at`. Every scroll, click or back /
forward of ours posts `WM_APP_PAGE_ACTED` (once per burst), and a Chromium
window's own scroll report arrives through the WinEvent hook: either starts
the wait for that page to catch up right then (`settle_poke`), hints open
or not, and when it ends the scan runs ahead of the hotkey (`scan_ahead`,
`ahead_start`), so an open that follows adopts a finished scan and shows
its labels at once. A foreground change runs the scan ahead too; a focus
change in the front window or keys that reached the app drop it.

Invariants stated in `common.h` and enforced by history:
- `g_cfg` is written only by the main thread (load and reload); other threads
  only read it.
- UI Automation never runs on the UI thread. Querying our own windows from
  there can deadlock.
- The hook is the only keyboard dispatcher. The old `RegisterHotKey` approach
  (`0964d95`) is gone; every hotkey is matched inside `kb_proc`.
- Shutdown order in `main_wnd_proc` `WM_DESTROY`: watcher, editor, scheduled
  shutdown (`shutdown_stop`), hints, **`input_stop` before `uia_stop`**
  (`4dda943`), the touchpad back on (`touchpad_stop`, after `input_stop` so
  its wait for the quit hotkey's keys blocks no hook), then tray.

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
`8b0a194` `b9a8d6f` `b474c89` `e51fecb` `27e5efe` `da32599` `7855485` `5ea3a61` `6622ce0` `ba62724` `37b210c` `1c0a83b` `ee4f7fb` `33a979f` `515eb02` `443b135` `17d27bc` `a7a6d9f` `d53e87d` `b7871de` `03cb03e` `1b6a156` `1514be3`, merges `0979ae1` `40657d6` `30f7f62` `735de23`

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
- Asked to find it without a Windows machine, `a7a6d9f` built
  `tools/ax_sim`. It found the container drop on a Google-like page and a
  second fault: page elements under a panel kept labels that clicked the
  panel. `17d27bc` handles dialogs, menus and frames as overlays
  (`cover_overlays`).
- `d53e87d`: `uia_probe -d -n -w` records the `log = 2` lines without a
  key press in the program, so an agent linked to the user's PC can scan
  a menu while it is open. The real account menu's markup, read through
  Claude in Chrome, went into `google.html` `mode=ogs`.
- `b7871de`: that scan, on the user's laptop, found the cause. Chromium
  lists the menu's rows at the right places but calls them all offscreen
  (a stale clip inside its copy of the page), so the scan's condition left
  them out. In a Chromium window, a web dialog or menu that shows almost
  nothing of its own now takes back the offscreen elements inside it
  (`rescue_offscreen`). Confirmed by replaying the laptop's scan in
  `tools/ax_sim`; the labels on screen are still to be seen.
- Asked to make the hints as fast as possible ("even with assembly"),
  measuring first showed that our own code takes under 1 ms of a scan: all
  of it is the apps answering UI Automation. The scan now asks a classic
  window's parts at the same time and leaves out the title bar's system
  menu (`scan_split`): File Explorer about 200 to 105 ms, Character Map
  135 to 60 ms. Chromium answers one question at a time and stays one
  search.
- Asked again for "revolutionary" browser speed, and for the CapsLock state
  to stay off: the wait for Chromium after a scroll (650 ms) was the
  largest delay left in a browser. Chromium's source showed that a UI
  Automation hit test makes the page send its held-back changes at once,
  and that the browser reports each scroll it applies as a WinEvent. The
  settle wait now asks and listens instead of sleeping (section 5), covers
  physical scrolls, and a browser's labels no longer wait for the other
  windows (`browser_first`). `capslock_off` keeps the CapsLock state off
  from the input thread, where the hook cannot see the key.
- Asked to make the program more convenient, the hint overlay got what
  the Alt keys and the status bar could not offer before: `;` switches
  between the labels and the grid while the hints are open (an app whose
  element got no label no longer needs `hint_mode = grid` in `vim.ini`),
  `Alt` + label moves the pointer without clicking, `Ctrl+Shift` + label
  middle-clicks (a link into a new browser tab), and a right, double or
  middle click waits for Shift and Ctrl to come up, since apps read the
  modifier state when the click arrives.
- "Still not fast enough" in Chrome, and "some labels don't appear on
  clickable elements". Reading the open path again showed the wait for
  the page (`77ec7f4`) only ever started when the hotkey came, and that
  for 650 ms after any scroll it also threw the prescan away, so a hotkey
  300 to 650 ms after `j` paid the flush and the whole search after F.
  The wait now starts with the action itself (`hint_page_acted`,
  `settle_poke`), hints open or not, and records when it ended
  (`fresh_tick`), so a later open adopts a normal prescan. For the labels,
  Chromium's source says a `div` whose click handler sits on an ancestor
  (how React attaches every handler) gets no Invoke pattern, only the
  keyboard-focusable state when it has a `tabindex`; browser windows now
  also fetch keyboard-focusable elements (`scan_focusable`) with filters
  against what else Blink calls focusable, checked on Linux with
  `tools/ax_sim` (`focus.html`).

### Phase 8: scheduled shutdown (2026-11-02)
`2d82ade` `2216b2a`

- Asked to "implement scheduling shut off computer", a new module,
  `shutdown.c`. The tray menu's **Schedule shutdown** turns the computer off
  in 15 or 30 minutes, 1 or 2 hours, or at a typed time or wait (`23:30`,
  `90`, `2h`, `1h30`); `shutdown_at` in `vim.ini` does it every day, and a
  cancel from the tray skips one day. A countdown window
  (`shutdown_warning`, 60 s) offers Cancel, 10 more minutes and Shut down
  now, then `ExitWindowsEx` powers off (`shutdown_force` for apps with
  unsaved work). The help window gets no row: it lists keys, and this has
  none. The rules it keeps are in section 5. There was no Windows
  machine, so the schedule got the first automated test here,
  `tools/shutdown_test.c` (a fake clock: sleep, late wake, daylight saving,
  the one-off against `shutdown_at`), and the real `vim.exe` was driven
  under Wine and Xvfb through its tray commands: the dialog with good and
  bad input, the countdown ticking, `Esc`, 10 more minutes, Shut down now
  reaching `ExitWindowsEx`, and both states of the submenu. An independent
  review before the push found six faults, all fixed and given test
  scenarios: `WM_TIMECHANGE` (Windows resyncs the clock on waking) turned
  the sleep check off and dropped a `shutdown_at` just passed; Cancel left
  a one-off typed for `shutdown_at`'s time, which counted down again at
  once; a click could land on Shut down now as the window appeared; 10
  more minutes reopened at once with a 600 s countdown (the range is now
  0-300); the clock set back left the countdown up for an hour; a one-off
  days away dropped every `shutdown_at` day before it for good.

### Phase 9: nothing in the pointer's way (2026-11-03)
`009d671` `03cd1d4` `d8d05b0` `a630c0f`

- Asked for a program that feels natural and smooth, "not laggy", as
  close to built in as possible. The pointer path measured clean on the
  user's machine (section 5, scrolling), so the lag was in what waited: the
  scroll-target lookup on the movement thread, which held back every Alt
  and bare scroll press and froze the pointer with it, and the `MOUSE` pill
  on its 15 ms timer. `uia_scroll_aim` replaces the waiting
  `uia_scroll_target`: the stream starts under the pointer at once and is
  re-aimed when the answer comes; the pill moves with each pointer move.
  Not changed, for lack of evidence: the speeds, the velocity smoothing,
  the per-frame wheel. Still waiting somewhere: `uia_scroll_end` (300 ms,
  `Shift+g` only) and the hook's `uia_focus_wait` (100 ms, only while a
  focus answer is pending).
- "Better, but not satisfied, especially on the navigator (f)." The scan
  itself is quick here (`uia_probe`: a Chrome window of 75 targets in 93
  to 141 ms), so the cost of `f` was everything that only started with the
  key: the wait for the page to catch up after a scroll (about 330 ms),
  then the scan. `scan_ahead` (default 1) runs that scan when the screen
  changes instead: after our scroll, click or back / forward once the page
  has caught up (`settle_done`, `hint_page_acted`), after a scroll the
  browser reported, and 700 ms after another window came to the front
  (`fg_event_proc`, a WinEvent hook of its own, since the input thread's
  focus hooks exist only with `bare_keys`). The result is the prescan of a
  new kind (`PRE_AHEAD`) and is adopted like the Alt prescan, up to 3 s
  old; a list adopted more than 300 ms after its scan is refreshed at once.
  It is dropped, with no new scan, by a focus change inside the front
  window and by keys that reached the app (`ahead_key_passed`, at most one
  post per 200 ms): typing opens and moves things, and the hotkey then
  scans as before. A page action drops it too and a new one follows the
  catch-up wait (or 300 ms later where none applies). The Alt prescan's
  cancel (`HINT_PREPARE_CANCEL`) leaves a scan ahead alone, and an Alt
  press with a young scan ahead starts no second scan.
- "The navigator appears twice (the first one flickering)": the adopted
  scan made the open paint later than before, and the overlay window
  still held the previous open's labels, which Windows composed for a
  frame (section 5: a window shown again shows what it last painted).
  `paint_blank` clears it before it hides.
- "Navigator speed on Chrome is still not satisfying; I use `f` right
  after scrolls, my typing is quite fast." A real session's log (`log = 1`
  turned on for a minute) showed `f` 312 to 422 ms after the last wheel
  tick, while the wait ended at about 350 ms and the scan it started at
  about 500: the wait, not the search, was the delay. Measured here with
  a scratch probe (now `tools/flush_probe.c`: one wheel notch posted to
  Chrome, hit tests at chosen times, `EVENT_SYSTEM_SCROLLINGEND` timed):
  Chrome applied the scroll at once, since Windows' animation effects are
  off on this machine and Chrome's smooth scrolling follows them; it
  reported the scroll 2 ms after the wheel when its 500 ms throttle
  allowed and otherwise on the next hit test, whose report arrives within
  a millisecond of the call returning (a hit test takes 4 to 38 ms); a
  search of the YouTube window took 103 ms and re-reading its 149
  elements' rectangles 14 ms (kept in mind, not used). So the first flush
  goes out 30 ms after the last wheel tick (`SETTLE_INPUT_MS`, was 230),
  from a thread of its own (`flush_worker`: on the scan worker it queued
  behind a scan); its verdict is read when it returns
  (`WM_APP_FLUSH_DONE`, `hint_flush_done`) plus a 15 ms grace
  (`SETTLE_GRACE_MS`, was a 70 ms timer); only a hit test that did not
  reach the page (`flush_at` compares the element's process with the
  window's) proves nothing. With `scan_early` (default 1: when animation
  effects are off; 2 always; 0 never) the first flush that reaches the
  page starts the scan at once (`settle_early_start`: the open's
  `begin_scan`, or `ahead_start_now`; `settle_blocks` lets an open adopt
  it while the wait runs) and the next flush only confirms it; movement
  found after that marks the scan stale (`g_settle_stale`) and
  `settle_done` scans again. A browser-reported scroll now ends its wait
  in 110 to 160 ms with the scan already running; after our own scroll
  the labels are ready about 50 ms plus the search after the last wheel
  tick, before a fast `f` arrives.
- "While not activating CapsLock, disable the mouse pointer so I'm not
  interrupted reading articles; also don't move the pointer to the point I
  clicked with the navigator." Two settings. `hide_pointer` (default 2000
  ms): outside mouse mode every system cursor is swapped for a blank one
  (`SetSystemCursor`, session-wide; `SPI_SETCURSORS` brings the user's
  scheme back), from a pointer thread of its own (`ptr_thread_proc`), so
  neither the hook thread nor the UI waits on the swap or on the cursor
  files; the input thread decides (`ptr_update`, posted through
  `ptr_schedule`, `WM_INPUT_PTR_UPDATE`): never in mouse mode, paused,
  locked or asleep (`s_ptr_away`, restored on suspend and session lock),
  otherwise once the real mouse has been still for `hide_pointer` ms
  (`PTR_TIMER` on the power window). A `WH_MOUSE_LL` hook (`mouse_proc`)
  tells the real mouse from our own input: `SendInput` carries
  `LLMHF_INJECTED`, and a `SetCursorPos` of ours (`own_move`) lands exactly
  on `s_own_x` / `s_own_y`. Leaving mouse mode and a hint click backdate
  the mouse (`ptr_forget_mouse`) so the pointer hides at once. The thread
  restores the cursors before it ends; a crash leaves them blank until the
  next run. `click_return` (default 1): `mouse_click_at` puts the pointer
  back where it was once the clicks are in (same input queue, so the app
  sees the click first), unless mouse mode is on, where the user carries
  on from the element; `Alt` + label and a drag's drop still move it.
  `page_acted_at(PAGE_CLICK, &pt)` now records the click point rather than
  the cursor before the move (the flush asks the browser there).
- "Enter twice for moving the mouse cursor to the center." `key_center`
  (default Enter) twice within `center_tap_ms` (300 ms) puts the pointer in
  the middle of the monitor that holds the active window (the pointer's
  own when there is none; the pointer may be hidden anywhere). Decisions:
  the key is never held back or swallowed, since Enter is every app's key
  and a first press cannot know whether a second follows, so both reach
  the app and only the second has the side effect (`center_key_down`, at
  the two places in `handle_down` where a key goes on to the app or is
  swallowed as a typing key in mouse mode); outside mouse mode it needs
  the focus known to be outside a text field, like the bare keys, read
  when the first press arms it (`s_center_ok`), because that press asks
  the focus again (`focus_key_passed`) and the answer at the second is
  often still pending; nothing waits in the hook, and the move itself runs
  from the input thread's queue (`WM_INPUT_CENTER`, `center_pointer`),
  not from the hook callback; it counts as the real mouse for
  `hide_pointer`, so the pointer shows up where it landed and hides again,
  and the pill follows in mouse mode. A single mouse-mode letter was
  considered and not taken: the user asked for Enter, and a bare letter
  would take a key from apps.

### Phase 10: labels the page hides (2026-11-03)

- "The navigator is not appearing on the `<h3>`" of a Google result (the
  namu.wiki title). `tools/hint_check.mjs --scroll` on the user's query
  showed the link found and labelled, and its label "covered by Group":
  every result block on Google is a target of its own (Invoke through
  `jsaction`), the title link starts at the block's corner, and the two
  labels sat on one spot, the block's on top. What the user saw was one
  label, which clicked the block. `labels_apart`: a label's box is found
  once (`place_labels`, `labelbox.h`), smallest element first, and a box
  that would cover a placed one moves right of it, or below it at the
  monitor's edge. The checker's own layout runs the same code.
- "The navigator doesn't appear on `<label for="selection_all">`" in
  Naver Mail. The browser extension refuses webmail, so Naver's `.blind`
  pattern was rebuilt on a mock page and Chromium's tree read with a
  managed UI Automation dump: the input is a CheckBox of 1x2 px (so "too
  small"), the `<label>` a nameless Group of 25x25 that is not a control
  element, `LabeledBy` null, and a label with plain text is not in the
  tree at all (Chromium prunes it as redundant). `scan_hidden_checkbox`:
  such a box takes the Group next to it (next sibling, previous sibling,
  then the parent for a wrapping label; `label_stand_in`). The walks
  need elements with full references, which the scan cache does not
  carry (section 5), and full references cost about a fifth more per
  scan, so `find_all_window` fetches them only for a window whose last
  scan saw such a box (`full_memory`), and once more at the first sight
  of one, in the same scan.
- "Add shortcut for pause/restart vim navigator", first as `Alt+0 0`,
  then corrected to `Alt+p p`. The pause hotkey existed but was unbound;
  it is now `Alt+P` and, like `g g`, takes two presses within
  `pause_tap_ms` (300 ms), so one Alt+P hit by accident among the other
  Alt letters does nothing. Both presses are swallowed (`Alt+P` is a
  program key now, like `Alt+F`), the tray menu and the help show
  "Alt+P P", and `pause_tap_ms = 0` makes it one press.
- "Instead of shadowing the typed char, remove the typed char of the
  navigators so I can view it more cleanly instead of overlapped big
  boxes": an element label used to stay whole once a character was typed,
  the prefix in `match_fg`, so the remaining labels kept their full boxes.
  `draw_element_label` now drops the typed prefix and draws only what is
  still to type, in a box that shrinks from the right (its place was found
  for the whole label, so it stays clear of its neighbours). The grid still
  shows its typed row letter in `match_fg`.

### Phase 11: system UI above the desktop (2026-11-03)

- "My vim cannot trace system UI (notification, system app etc) with
  navigator." Measured on Windows 11 26300 with scripts around
  `WindowFromPoint`, `GetWindowBand` and the managed UI Automation client:
  the Start menu and Search (`Windows.UI.Core.CoreWindow` windows of
  StartMenuExperienceHost and SearchHost, band 6), the notification centre
  and notification banners (ShellExperienceHost, band 4) live in z-order
  bands above the desktop band that holds every ordinary window, topmost
  ones included. `EnumWindows` does not list them for a process without
  UIAccess, and neither do the UI Automation root's children, so the scan
  never saw them (`log = 2` said "the active window ... is not scanned"
  with the Start menu open) and labelled the windows underneath as if
  nothing covered them. `GetForegroundWindow`, `WindowFromPoint`,
  `GetWindowRect`, `IsWindowVisible` and `ElementFromHandle` all answer for
  them, so `add_unlisted_windows` takes the foreground window the list
  lacks, samples the monitor every 64 px with `WindowFromPoint` (16 ms at
  most for 1920x1080), puts what it finds in front of the listed windows
  ordered by band, and `log = 2` marks them "above the desktop band".
  Checked with `uia_probe -d`: Start 15 targets, Search 1 (its box), the
  notification centre 74, a banner 8, and the banner "covered" while Start
  was open. The Settings app (`ApplicationFrameWindow`) is an ordinary
  window and had 24 labels already.
- The overlay cannot be drawn above those bands from an ordinary process:
  `WindowFromPoint` picked a banner over a topmost test window, and a
  window created with the banner as owner (or re-owned to it afterwards)
  stayed in the desktop band with no owner set. Windows reserves that for
  UIAccess programs (manifest `uiAccess="true"`, signed by a certificate the
  machine trusts, started from Program Files), so `make uiaccess` builds the
  same program with `vim.uiaccess.manifest`, `tools/uiaccess_setup.ps1`
  makes the certificate, signs and installs it (one administrator prompt),
  `app_has_uiaccess` logs the state at start, and the tray update refuses
  to replace that install with the unsigned release. Not yet run on the
  user's machine: the trust step needs the administrator prompt (section 7).
- "Change centering shortcut from enter twice into 00." `key_center`
  defaults to `0` (was Enter); a digit also matches its number-pad key
  (`center_key_is`). A `vim.ini` written before keeps `key_center = Enter`
  until edited.
- "00 doesn't work though; make Alt + 00 also work" and "sometimes gg
  doesn't move to the top, I have to retype them": the cause of `29def43`,
  one step further. Right after a hint click the focus answer is pending
  for `FOCUS_CLICK_DELAY_MS` plus the classification, and the *second* tap
  can land inside that too: it typed and only re-armed, so the top needed
  a third and fourth `g`; `0 0` did not even arm, since the first tap
  required a known "no text field". Now the first `0` arms unless the
  focus is known to be a text field, a second tap waits `BARE_WAIT_MS`
  like the bare keys, and a second tap (`g g` or `0 0`) that still typed
  with the answer pending is deferred (`tap_defer`, `DEFER_TOP` /
  `DEFER_CENTER`): the focus worker posts `WM_INPUT_FOCUS_ANSWER` to the
  input thread after every answer (`uia_focus_notify`, `focus_notify`),
  and `tap_answer` acts within `TAP_DEFER_MS` (1 s) when the answer is
  "no text field", the focus window (`focus_hwnd_now`) is still the
  tap's, no other key came (`tap_defer_cancel` in `handle_down` and
  `alt_resolve`) and no menu opened; a text-field answer drops it, so a
  key typed into a text field still never turns into a jump. Hook and
  input thread are one thread, so the state needs no lock. `Alt` +
  `key_center` twice (`alt_center_down`, on both Alt paths) jumps
  anywhere, text fields included, both taps swallowed like the other Alt
  keys; `first_key_taken` warns a text shortcut starting with that key.
  `log = 1` writes why a tap did not jump.
- "Still gg doesn't move to the top, instead some frames up. It often
  occurs at initial try": the taps now registered (the jump started), but
  the page moved a few screens up and stopped. The top was the one
  scroll-to-end still sent on the wheel: 64 full-range wheel deltas glided
  over half a second through Chrome's wheel pipeline (a latched sequence,
  animation retargeting, whatever the page does with wheel events), while
  `Shift+g` has gone through ScrollPattern's `SetScrollPercent` since
  `be52799` without a report. Which step in Chrome dropped the rest was not
  pinned down (`log = 0` on the user's machine); the route was. Now the
  top jump asks UI Automation first too (`uia_scroll_end` with a `top`
  flag: percent 0, "already at the top" chains to the parent like the
  bottom), exact and at once, and the wheel glide is only the fallback
  where it finds nothing to scroll. The movement thread waits for the
  scroll-target lookup before a top jump as it did for the bottom, and the
  deferred `g g` (`tap_answer`) aims like the bare path. `log = 1` writes
  which route the top took. Confirmed by the user: "Yes, works".

### Phase 12: the label alphabet (2026-11-04)

- "When it comes to navigator alphabet combinations, as I know vimium uses
  left hand friendly pattern. I can be wrong so investigate and if there's
  better option for it, apply that." Checked against the sources: Vimium's
  default is `sadfjklewcmpgh` (the home row of both hands first, then
  `e w c m p g h`), Vimium C's `sadjklewcmpgh`, Tridactyl's
  `hjklasdfgyuiopqwertnmzxcvb` (all 26, home row first); the left-hand-only
  set is Surfingkeys' `asdfgqwertzxcvb`, so that the right hand can stay on
  the mouse. Ours was the old program's `abcdefhjklmnopqrstuvwxz`:
  alphabetical, minus `g`, `i` and `y` (they read as `q`, `j` and `v` in
  the label font). The order matters: `build_labels` expands the head of
  the alphabet first, so its first letters begin every two-letter label (on
  a page of 150 elements the first eight are prefixes) and are the
  one-letter labels of a quiet screen, while the tail letters are the
  one-letter labels of a busy page. Alphabetical put `b` and `c`, bottom-row
  keys, among the prefixes. The default is now `asdfhjklqwertuopzxcvbnm`:
  the home row, then the top row, then the bottom row, each in keyboard
  order. The same 23 letters, so the code space (529 two-letter labels) and
  the grid (23 columns at 1080p) are unchanged. A left-hand-only set was
  not taken: both hands are on the keyboard here, and 15 letters would push
  busy pages to three-letter labels and the grid to 15 columns. The user's
  own `vim.ini` names the alphabet explicitly, so it got the new line too
  (the default alone would have changed nothing there).

### Phase 13: the touchpad off while vim mouse runs (2026-11-08)

- "while vim is enabled, disable touchpad". `touchpad_off` (default 1,
  `touchpad.c`): off while vim mouse runs and is not paused; pausing,
  quitting or `touchpad_off = 0` (when it was ours) turn it back on.
  Measured on Windows 11 build 26300: `SPI_GETTOUCHPADPARAMETERS` (24H2)
  reads the Settings toggle (`touchpadEnabled`, bit 3 of the first flags
  word; the Settings registry value `PrecisionTouchPad\Status\Enabled`
  agrees), and `SPI_SETTOUCHPADPARAMETERS` changes tap-to-click but
  silently ignores `touchpadEnabled` with any `fWinIni`. Ctrl+Win+F24, the
  key a laptop's touchpad key sends, flips it (both read back at once). So
  the state is read first and the shortcut pressed only when it differs, in
  one `SendInput` call, only once Shift, Ctrl, Alt and Win are all
  physically up (after `Alt+p p` that is the Alt release; up to 15 s), then
  read again after 600 ms, at most two presses. The goal changes only on
  pause, resume, reload or quit (mouse-mode changes post the same
  `WM_APP_STATE_CHANGED` but leave it), so a touchpad the user switches on
  by hand stays on until the next of those. At quit `touchpad_stop` waits up
  to 2 s for the quit hotkey's keys. Tested live: start (stays off), tray
  Pause (on), resume (off), `touchpad_off = 0` in the test build's ini (on)
  and back to 1 (off), graceful quit (on). A self-update quits then starts,
  so the touchpad flicks on and off once.

### Phase 14: pages still loading (2026-11-08)

- "when browser loading is slow, navigator cannot find clickable elements
  during that". Probed first (`tools/load_probe.c`, `load_probe.mjs`: slow
  local pages in a temporary Chrome, a WinEvent hook on its browser process
  and the document's MSAA state every 100 ms). From the navigation until
  the first content reaches the accessibility tree, the new document says
  `STATE_SYSTEM_BUSY` and has no children (1.5 s on a page streamed
  slowly), so a scan found only the browser's tabs and toolbar; the hints
  showed those, and the page's links came in later through refreshes with
  only the few codes `keep_labels` left free, or the hints closed after
  3 s with nothing. The content arrives as `EVENT_OBJECT_SHOW` of the new
  document's root (`OBJID_CLIENT` on its `Chrome_RenderWidgetHostHWND`);
  later parts reorder it and show each element.
  `IA2_EVENT_DOCUMENT_LOAD_COMPLETE` comes with the load event: a 5 s image
  held it 5 s with all the links on screen at 50 ms, so it only ends the
  watch. A slow first byte keeps the old page, not busy, until the commit:
  labelling it is right.
- `scan_loading` (default 1). The scan asks the foreground window's page
  whether it is busy (`uia.c` `page_busy`: the largest visible page window,
  `WM_GETOBJECT` `OBJID_CLIENT` under a 50 ms timeout, `get_accState`)
  before that window's search starts, and passes it on
  (`ClickList.page_busy`). The open then waits up to 10 s
  (`LOAD_LIMIT_MS`, `scan_hard_limit`) instead of 3, the status bar says
  "Page loading", and `load_watch` hooks `EVENT_OBJECT_SHOW` to
  `EVENT_OBJECT_REORDER` and the IA2 load event of the page window's
  process only, out of context, for as long as it watches; a burst of
  reports (`LOAD_GATHER_MS`) starts a refresh at once (`load_content_in`).
  Labels that went up while it was busy are made afresh by the first full
  refresh that finds it in (`relabel_all`), unless a label key was typed
  (`element_append` clears `g_load_relabel`). Tested live with
  `load_probe.mjs --open`: on the streamed page the hints showed 14
  toolbar labels at once, the content was reported 1.1 s later and 48
  targets were labelled afresh within 100 ms; the slow-image page opened
  at once with its links; the slow first byte labelled the old page.

### Phase 15: Esc leaves a text field (2026-11-08)

- "implement new function: pressing esc disables current text input focus
  entirely". Vimium's `Esc` in insert mode, for the whole browser: after
  typing in a search box the keys without Alt need a click on the page
  first. Measured first in a temporary Chrome (managed UI Automation plus
  CDP for `document.activeElement`): UI Automation `SetFocus` on the page's
  Document element leaves `activeElement` on `<body>` (Blink focuses the
  document element for that action), from an input inside an iframe the
  iframe's Document leaves the frame's `<body>` focused, and from the
  address bar the page Document takes the focus into the page. The Document
  is not what `ElementFromHandle` of `Chrome_RenderWidgetHostHWND` returns
  (a Pane) but that Pane's child, and the address bar has no Document
  above it, so `leave_page_document` takes that route.
- `esc_unfocus` (default 1). `Esc` still reaches the app; the hook never
  holds it back. The hook arms at a plain `Esc` press outside mouse mode
  and the hints (`unfocus_key_down`) unless a menu is open or the focus is
  known to be outside text fields, and the focus worker looks at the field
  at once (`leave_look`): the field and where the focus goes. At the
  release (`handle_up`, `uia_focus_leave_up`) plus `LEAVE_AFTER_MS` (30) it
  checks again that the focus is still in a text field of the same window,
  so an `Esc` that closed a dialog or moved the focus itself is left
  alone, then calls `SetFocus` on the nearest Document above the field (or
  the page's) and asks the focus answer again. Another key before the
  release, a reset, or `Esc` held over `LEAVE_HOLD_MS` (1.5 s) drops it.
  1 is web browsers by process name (`browser_process`: Chrome, Edge,
  Firefox, Brave, Vivaldi, Opera, Whale and others); Electron and WebView2
  apps only with 2, since their editors (VS Code's, Slack's) use `Esc`
  constantly. Native apps are never touched: a dialog or Notepad sends the
  focus straight back to its edit control on `WM_SETFOCUS`, and their
  `Esc` means cancel or close. Tested against the temporary Chrome through
  a scratch harness linked with `uia.c` (the hook ignores injected keys, so
  a sent `Esc` cannot drive it): page input to `<body>`, the expanded
  combobox kept, a link left alone, the iframe input to the frame's body,
  the address bar to the page Document.
- "Works well but on google search, i have to press esc twice". The first
  version gave an `Esc` to an open suggestion list (an expanded combobox,
  or Chromium naming the highlighted option as the focus), so a second
  `Esc` left the field. Measured on the results page in the temporary
  Chrome: a click into Google's search box opens its suggestions every
  time (`aria-expanded="true"`, one listbox shown), while a JS `focus()`
  does not, so every click-then-`Esc` paid two presses. The exception is
  gone: the page still gets the `Esc` first and closes its list, and the
  same press leaves the field (`esc: focus to the page`, `<body>` active).

### Phase 16: the magnet (2026-11-08)

- "when mouse pointer changes to finger (on clickable elements) during
  moving it using capslock (hjkl), slightly stop instead of just passing by
  the element ... like magnetize function of adobe premiere pro. this
  function also should not disrupt smooth usability." `magnet` (default
  200 ms, 0 = off): in `movement_thread`, while a move key is held, a tick
  that finds the cursor turned into the system hand (`cursor_is_hand`:
  `GetCursorInfo` against `LoadCursor(NULL, IDC_HAND)`, one shared handle
  in every process; Chromium, Firefox and Win32 links use it) pauses the
  pointer for that long. The pause only inserts time: the velocity is zero
  and the acceleration's hold time waits, so after it the pointer goes on
  at the speed it came (the velocity smoothing ramps it back in about
  20 ms). What keeps it out of the way: releasing ends it (the pointer
  stays on the element, which is the point), another move key, `Shift` or
  the left button held end it at once, a burst that starts on the hand
  never pauses there, and the next pause needs the cursor seen off the
  hand first (one pause per element entered; adjacent hand areas with no
  arrow between them are one).
- An app shows the hand only after it handled the move, and Chromium hit
  tests in the page on its next frame, so at 2400 px/s the pointer is tens
  of pixels on when the hand shows, past a short link. The thread keeps
  where it put the pointer (`MagTrail`, 256 moves); if the hand goes off
  during the pause (the pointer stands still, so the app caught up and
  found it outside), the time from the last move to that is the app's
  lag, and the pointer goes back to where it was put that long before the
  hand showed (`mag_trail_at`, at most `MAG_MAX_LAG_US`, 100 ms), with
  the pause restarted there. Off the hand for longer than that ends the
  pause. `GetCursorInfo` does not wait (section 5: never wait in the
  movement thread) and is asked only while a move key is held or a pause
  runs. The hook ignores injected keys, so the pause could not be driven
  from a script here; it was built and handed to the user to try.

### Phase 17: hold a scroll key to leave a text field (2026-11-08)

- "let pressing d, j, k, u for long enough disable text input focus. focus
  on usability so the duration must not be too long." `hold_unfocus`
  (default 300 ms, 0-2000, 0 = off): in a text field where `Esc` would
  leave (`leave_look`'s rules: browsers, or web apps with `esc_unfocus =
  2`), the bare scroll keys held that long leave the field like `Esc` and
  then scroll while still held. 300 ms is under Windows' default repeat
  delay (500 ms; the fastest setting is 250) and well over a typing key's
  hold (about 100 ms, rarely past 200).
- Typing never waits. The alternative, holding the letter back until the
  release or the next key, would have delayed every `j k u d` typed in a
  browser and reordered rolled-over keys. So the press types as usual
  (`hold_key_down` on the pass-through path of `handle_down` marks the key
  `OWN_HOLD` and arms the worker with `uia_focus_leave_hold`), and the
  leave takes the letter back: a Backspace in a page, an `Esc` in the
  browser's own UI (the address bar's inline completion would eat a
  Backspace; `Esc` reverts the typing there), sent by the focus worker
  (`leave_erase`) `LEAVE_ERASE_MS` (30) before the `SetFocus`, since
  typed input waits behind the sent messages a UI Automation call arrives
  as. A field whose selection the letter replaced loses that selection;
  undo would need Ctrl, which is never injected.
- The worker looks only once the key has been held long enough
  (`leave_step` returns the time left before `leave_look`), so the look's
  UI Automation calls are not paid on every typed letter; the hook only
  pre-filters by window (`hold_window_ok`: `uia_browser_window` of the
  foreground window, cached). Its repeats are held back while the hold is
  pending (`hold_repeat`), so a key held in Notepad still repeats once the
  worker says "no" (`uia_focus_leave_result` = `LEAVE_NO`). Another key, a
  modifier included (it would make the Backspace Ctrl+Backspace), or the
  release cancels (`hold_cancel`, also in `alt_resolve` and
  `reset_key_state`). On `LEAVE_DONE` the worker posts the focus answer's
  message; `hold_answer` on the input thread starts the key's scroll
  (`scroll_press`, like a bare key) if it is still down, and its release
  stops it and still reaches the app (the app saw the press).
- The Esc and the hold share the worker's leave state (`g_lv_*`, a new
  press replaces the old one); `g_lv_hold_ms` tells them apart, and every
  press now records how it ended (`g_lv_res_seq`, `g_lv_res`). Not tested
  by key injection (the hook ignores injected keys); built and handed to
  the user to try.

### Phase 18: the touchpad works while the keyboard is locked (2026-11-08)

- "while ctrl + alt + l is enabled (keyboard lock), allow touchpad. when
  disabled, return to normal behavior". `touchpad_update`'s goal is on
  while paused or locked (`input_is_locked`); `apply_locked` already posts
  `WM_APP_STATE_CHANGED`, so the lock hotkey and the tray item both reach
  it. Locking: the hotkey's Ctrl and Alt passed through before the lock,
  so `GetAsyncKeyState` sees them and the press waits for their release.
  Unlocking: the lock swallows the chord's Ctrl and Alt, so
  `GetAsyncKeyState` says "up" while they are held; `modifiers_down` now
  also asks `input_modifier_held` (the hook's own `s_down`, read from the
  UI thread; a stale byte only delays the press), so the injected
  Ctrl+Win+F24 still waits for the physical release.

### Phase 19: pinch zoom on the keyboard (2026-11-08)

- "alt+= or - to zoom in and out just like with gathering or spreading
  with touch pad using two fingers on a current mouse pointer in laptop.
  remember, it's different from ctrl + = and -". `Ctrl+=` and `Ctrl` +
  wheel are the browser's page zoom (reflow, kept per site); a touchpad
  pinch is Chromium's pinch zoom (the visual viewport magnified around the
  fingers, no reflow), which no key or wheel message reaches. Windows has
  no way to inject touchpad contacts (`CreateSyntheticPointerDevice` takes
  pen and touch only), so `zoom.c` injects two touch-screen contacts
  (`InjectTouchInput`, Windows 8) either side of the pointer and moves them
  apart or together. Browsers, Photos, Maps and PDF viewers take that as
  their touch pinch; apps without touch gestures get Windows' legacy
  `Ctrl` + wheel.
- Geometry from Chromium's gesture settings (`gesture_configuration`): a
  pinch needs fingers at least 125 DIP apart (`min_scaling_span`) and
  starts only once their spread has changed by 30 (`span_slop`). So the
  near limit is 160 DIP, the far one 640 (or what the monitor leaves,
  along whichever axis has more room), and the first frame jumps the
  slop (32) so the following frames are all zoom. The spread then changes
  by `zoom_speed` percent a second, exponentially, since a pinch
  magnifies by the ratio of the spreads. A tap goes on until the spread
  has changed by `ZOOM_TAP_STEP` (1.25) past the slop; at a limit the
  fingers lift and grip again after 40 ms.
- The hook only records presses (`zoom_press`, `zoom_release`; the last
  press wins, a press of the other key mid-pinch starts its own grip) and
  `reset_key_state` lifts the fingers (`zoom_cancel`). Alt + the key only,
  never as a bare key: the focus check calls a spreadsheet cell "not a
  text field", where `=` starts a formula. Mouse mode takes `=` / `-`
  alone. Excel's `Alt+=` (AutoSum) is lost; `key_zoom_in = none` returns it.
- Touch may move the pointer to the first finger and hide it, so each
  grip ends with an injected absolute move back to where the pointer was;
  the mouse hook now also ignores mouse messages Windows makes from touch
  (`MI_WP_SIGNATURE` in `dwExtraInfo`), so a pinch does not show the hidden
  pointer. Built for 32 and 64 bit; touch injection does not run under
  Wine and the hook ignores injected keys, so it was handed to the user to
  try in a browser.

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
| 80 | `dd1e12f` | 10-31 | 7 | Commit-log rows for `965acd0` and `443b135` | |
| 81 | `17d27bc` | 10-31 | 7 | Window (web dialog) and Menu elements are fetched; in web content the frames and the dialogs and menus that leave part of the page free are overlays, their content the run after them in tree order that touches them; `cover_overlays` gives an element whose click point is on an overlay it is not part of the largest part of it outside as rect and click point, or drops it under a later dialog or menu | Page elements under a panel kept labels that clicked the panel (Google's knowledge panel links under the account menu; ticket links under a Zendesk menu or modal) |
| 82 | `a7a6d9f` | 10-31 | 7 | `tools/ax_sim`: Chromium's real tree (headful, AT-SPI) through `uia.c`'s own filters, on Linux | The filters could only be tried on Windows; this found 17d27bc's fault and confirmed 443b135 |
| 83 | `c2ccce6` | 10-31 | 7 | Commit-log rows for `dd1e12f`, `17d27bc` and `a7a6d9f` | |
| 84 | `d53e87d` | 11-01 | 7 | `uia_probe`: `-d` (the `log = 2` lines for the active window), `-w` (wait before each run), `-o` (output file), time per run. `ax_sim` `google.html` `mode=ogs`: aria-hidden, visibility and height 0 together, as Google hides the account menu | `log = 2` needs a key press in the running program, which an agent cannot send (injected keys are ignored). On Linux the real hiding pattern still exposes the menu |
| 85 | `a5fb156` | 11-01 | 7 | Commit-log rows for `c2ccce6` and `d53e87d` | |
| 86 | `b7871de` | 11-01 | 7 | In a Chromium window, a web dialog or menu on the page with at most three elements of its own on screen (`popup_onscreen`) is asked for its offscreen descendants; `rescue_pick` takes back the enabled ones mostly inside it, clipped to it, when they are at least two and outnumber the on-screen ones, not one mostly under an on-screen element, and none when together they fill its rect exactly (a menu closed by height 0). Inserted after it in tree order (`els`, `scan_layout`). With `log = 2` they are marked "offscreen per Chromium, taken back". `ax_sim`: offscreen elements flagged, the same taking back, `INPUT=` replays a saved input | Google's account menu had no labels: on Windows its 17 rows were listed at the right places with IsOffscreen TRUE, while the dialog and its Close button were on screen. Chromium's IsOffscreen is the clipped walk of its stored tree, and a clipping box inside the menu stays stale there |
| 87 | `df9f64d` | 11-01 | 7 | Commit-log row for `b7871de` | |
| 88 | `03cb03e` | 11-01 | 7 | `scan_split` (default 1): a non-Chromium window's root children are fetched (Full element mode), Pane, Group and Custom containers down to depth 3 split again, a TitleBar gives its children but its MenuBar, every piece is a `TreeScope_Subtree` FindAll on the helper pool (a work-item queue per job, `work_sem`); the window's last item puts the pieces back in tree order and filters once (`scan_filter`). `uia_probe -1` | Asked for the fastest hints: our code is under 1 ms of a scan, the rest the apps' UI Automation. File Explorer 203 to 112 ms, Character Map 134 to 61 ms (medians); the system menu alone was 64 to 70 ms |
| 89 | `03d7dc9` | 11-01 | 7 | Commit-log row for `03cb03e` | |
| 90 | `12502dc` | 11-01 | 7 | `capslock_off` (default 1): the hook swallows every CapsLock press that is not the mouse-mode key; the input thread checks the state every 250 ms and after each CapsLock or modifier key-up (`caps_check`) and taps CapsLock off with `SendInput`, never with a modifier held, never injecting Shift, backing off 0.5 to 8 s when Windows drops the tap, not while paused or suspended | The user had to turn CapsLock off by hand: `Shift+CapsLock` passed through, and administrator windows, the lock screen, a paused program and the state at start were out of the hook's reach |
| 91 | `77ec7f4` | 11-01 | 7 | The settle wait asks the Chromium page to send its held-back changes (`uia_flush`: a UI Automation hit test) and ends when a flush finds nothing new, listening to `EVENT_SYSTEM_SCROLLINGEND`; scrolls the browser reports itself start it too (`scan_flush`); with a browser active its labels show as soon as it answered (`scan_ready`, `browser_first`, `ClickList.pending`); page actions record kind and point | Asked for radically faster browser hints: the fixed 650 ms after a scroll was the largest delay left, and physical scrolls were labelled where the page had been |
| 92 | `c33324e` | 11-01 | 7 | Review fixes to `77ec7f4`: the confirming flush 35 ms after a report, the first flush after our scroll at 230 ms, a report alone never ends the wait, back / forward keep the fixed wait (`PAGE_NAV`), the flush point over the page area, our own scroll's reports start nothing, a self-scrolling page is left alone for 10 s | An independent review found the wait could end mid-animation (a flush at once after a report found nothing new) and that a cursor over the toolbar flushed nothing after back / forward |
| 93 | `d742026` | 11-01 | 7 | Commit-log rows for `12502dc`, `77ec7f4` and `c33324e` | |
| 94 | `1b6a156` | 11-02 | 7 | `key_grid` (default `;`): while the hints are open, the grid instead of the labels and back (`toggle_view`, read before the key queue); `Alt` + label moves only; `Ctrl+Shift` + label middle-clicks; a right, double or middle click waits for Shift, Ctrl and Alt to come up (`click_when_free`, `TIMER_ACT_ID`, 3 s at most); a key typed into the hints consumes the withheld Alt; the status bar names the click kinds before a label is typed; the help always shows the grid | An element without a label meant editing `vim.ini` for the grid; move-only was only possible for the one remaining match; clicks went out with Ctrl or Shift still down, so Explorer toggled the selection and browsers opened a new tab |
| 95 | `2e8aad5` | 11-02 | 7 | Commit-log row for `1b6a156` | |
| 96 | `1514be3` | 11-02 | 7 | The wait for a Chromium page starts with our scroll, click or back / forward (`WM_APP_PAGE_ACTED`, once per 100 ms burst, `hint_page_acted`) and with a scroll the browser reports, hints open or not (`settle_poke`, `settle_arm`, `settle_rebase`); when it ends the tick is kept per window (`fresh_tick`) and a later open scans at once with its prescan; an open during it joins it (`g_settle_wait`); a wait ahead of the hotkey that runs out backs off 5 s (`SETTLE_BACKOFF_MS`) instead of marking the page restless. Browser windows search with `web_condition` (keyboard-focusable elements too, `scan_focusable`, default 1); `focusable_left_out` drops text, scroll areas, what lies outside the page Document and what covers over a quarter of it, and such elements are never actionable for `filter_containers`; `log = 2` says `focusable only: ...`. `tools/ax_sim`: focusable-only elements (actionable 2) and `focus.html` | "Still not fast enough" in Chrome: the wait only started with the hotkey, and the prescan was refused for 650 ms after any scroll, so a hotkey 300 to 650 ms after `j` paid a flush and the whole search. "Some labels don't appear on clickable elements": Blink gives a div whose click handler sits on an ancestor (React, jsaction) no Invoke pattern, only the focusable state with a tabindex |
| 97 | `999227e` | 11-02 | 7 | Commit-log rows for `2e8aad5` and `1514be3` | |
| 98 | `2d82ade` | 11-02 | 8 | `shutdown.c`: tray "Schedule shutdown" (in 15 / 30 min, 1 / 2 h, "At a time or after...": `23:30`, `90`, `2h`, `1h30`), `shutdown_at` every day (a tray cancel skips one day), `shutdown_warning` countdown window (Cancel, 10 more minutes, Shut down now off the keyboard), `ExitWindowsEx` with `SeShutdownPrivilege` (`shutdown_force` for `EWX_FORCE`); wall-clock UTC, a shutdown slept through by over 5 min skipped with a balloon, a late wake counts down in full; `parse_clock`; tooltip; `WM_TIMECHANGE`; `tools/shutdown_test.c` (`make shutdown-test`, built in CI) | Asked to "implement scheduling shut off computer". Checked under Wine: a fake-clock test and the real `vim.exe` driven through its tray commands; not on Windows |
| 99 | `2216b2a` | 11-02 | 8 | Review fixes to `2d82ade`: `WM_TIMECHANGE` keeps the last check time and a due `shutdown_at`; Cancel and `power_off` use up shutdowns close together (`consume_between`); Shut down now disabled for 1 s (`NOW_DELAY_MS`); `shutdown_warning` 0-300; clock set back closes the countdown; a one-off replaces only the next `shutdown_at` day and gives it back (`undo_replace`) | An independent review found a sleep with the countdown showing could shut down at once after the clock resync on waking, Cancel could leave a second countdown, and a 72 h one-off dropped three `shutdown_at` days for good |
| 100 | `009d671` | 11-03 | 9 | `uia_scroll_aim` (never waits; `SCROLL_AIM_*`) replaces the blocking `uia_scroll_target`; the movement thread polls it each tick (`aim_asking`, `aim_pt`, `aim_start_us`), starts every stream under the pointer at once and re-aims it, resending to the target what a tap sent to the pointer meanwhile (`scrOwed`); the UIA bottom jump alone waits; the `waiters` field is gone. `indicator_moved`: the pill follows each pointer move through one queued `WM_INPUT_IND_UPDATE` (`s_ind_posted`) | "Still don't feel natural and smooth, I don't want a laggy program": the pointer path measured exact on the user's machine, but every Alt / bare scroll press waited up to 500 ms on the movement thread (pointer frozen too), and the pill trailed on a 15 ms timer |
| 101 | `03cd1d4` | 11-03 | 9 | `scan_ahead` (default 1): the scan runs when the screen changes, ahead of the hotkey (`ahead_start` from `settle_done`, `hint_page_acted` with `AHEAD_DELAY_MS`, `fg_event_proc` with `AHEAD_SWITCH_MS`, `TIMER_AHEAD_ID`); kept as the prescan of kind `PRE_AHEAD` (`g_pre_kind`), adopted up to `AHEAD_MAX_AGE_MS` (3 s, the Alt prescan too, was 2 s), refreshed at once when older than `AHEAD_REFRESH_MS`; dropped by page actions, browser scroll reports, foreground and focus changes (two WinEvent hooks of `hint.c`) and keys that reached the app (`input.c` `ahead_key_passed`, `HINT_PREPARE_FORGET`); `hint_prepare` takes `HINT_PREPARE_*` | "Better, but not satisfied, especially on the navigator (f)": the scan is 100 to 140 ms here, but after a scroll `f` also waited about 330 ms for the page, all of it after the key |
| 102 | `d8d05b0` | 11-03 | 9 | `paint_blank`: the overlay is filled with the key colour before it hides (`hint_close`) and before it moves to another screen (`cycle_monitor`) | "The navigator appears twice (the first one flickering)": a window shown again composes what it last painted, and since `03cd1d4` the open paints only once its adopted labels are built, so a frame of the old labels showed first |
| 103 | `a630c0f` | 11-03 | 9 | The first flush after our scroll goes out 30 ms after the last wheel tick (`SETTLE_INPUT_MS`, was 230) from a flush thread of its own (`flush_worker`, `WM_APP_FLUSH_DONE`, `hint_flush_done`); a 15 ms grace after it returns replaces the 70 ms answer timer; a flush whose hit test did not reach the page (`flush_at`) is the only one that proves nothing; `scan_early` (default 1: Windows' animation effects off; 2 always; 0 never) starts the scan on the first flush that reached the page (`settle_early_start`, `settle_blocks`, `g_settle_stale`) and lets the next one confirm it | "Navigator speed on Chrome is still not satisfying, I use f right after scrolls": the log showed `f` 312 to 422 ms after the scroll, the wait ending at 350 and the scan after it; measured on Windows, Chrome scrolls without animation here (animation effects off) and the hit test's report comes with its answer |
| 104 | `207505f` | 11-03 | 9 | Commit-log row for `a630c0f`; `tools/flush_probe.c` kept (`make flushprobe`) | |
| 105 | `9d1a3e6` | 11-03 | 9 | `hide_pointer` (default 2000 ms): outside mouse mode the system cursors are blank (`SetSystemCursor` from `ptr_thread_proc`, `SPI_SETCURSORS` back), decided by `ptr_update` on the input thread (`ptr_schedule`, `WM_INPUT_PTR_UPDATE`, `PTR_TIMER`, `s_ptr_away` on lock and sleep); a `WH_MOUSE_LL` hook (`mouse_proc`) shows it for the real mouse (`LLMHF_INJECTED`, `own_move` with `s_own_x` / `s_own_y`); `ptr_forget_mouse` on leaving mouse mode and after a hint click. `click_return` (default 1): `mouse_click_at` puts the pointer back after the clicks unless mouse mode is on; `focus_clicked_at` records the click point | "While not activating CapsLock, disable the mouse pointer so I am not interrupted reading articles; and do not move the pointer to the point I clicked with the navigator". Checked here: blank arrow while running, the real one back after a graceful close, blank again after a restart |
| 106 | `14883fb` | 11-03 | 9 | `key_center` (default Enter) twice within `center_tap_ms` (300 ms): the pointer to the middle of the active window's monitor (`center_key_down` on the pass-through paths of `handle_down`, `s_center_ok` read at the first press, `WM_INPUT_CENTER`, `center_pointer` on the input thread); both presses still reach the app; shows the hidden pointer like the real mouse | "Enter twice for moving mouse cursor to the center": Enter cannot be swallowed, so the jump is a side effect of the second press, outside text fields or in mouse mode |
| 107 | `709bcb9` | 11-03 | 9 | Commit-log row for `14883fb` | |
| 108 | `f536827` | 11-03 | 10 | `labels_apart` (default 1): `place_labels` finds each label's box once, smallest element first, and `labelbox_apart` (`labelbox.h`, shared with `tools/hint_check.c`) moves a box that would cover a placed one right of it or below it (`HintLabel.box`, `placed`, `g_labels_replace`); `scan_hidden_checkbox` (default 1): a 1 px checkbox or radio button in a browser takes its label's box (`label_stand_in`: next sibling, previous sibling, parent Group within 64 px, `STAND_IN_MAX` 40), which needs elements with full references, so `find_all_window` fetches them (`full_cache`) for a window `full_memory` remembers and once more at the first sight of such a box (`tiny_boxes_listed`); the log = 2 cache is full too; `hint_check.mjs` counts a `<label>` whose control is hidden and the tools run with the app's browser settings | "The navigator is not appearing" on a Google result's title (the result block's label painted over the link's at the same corner) and on Naver Mail's "select all" label (the input is 1 px, the label a nameless Group that is not a control) |
| 109 | `8bca056` | 11-03 | 10 | Commit-log row for `f536827`; the Naver Mail "select all" label confirmed by the user | |
| 110 | `9ce3e54` | 11-03 | 10 | `pause_hotkey` defaults to `Alt+P` and takes two presses within `pause_tap_ms` (0-1000, default 300; 0 = one press): `handle_hotkeys` arms `s_pause_tap` on the first and toggles on the second, both swallowed, cleared by any other key and in `reset_key_state`; `pause_label` shows "Alt+P P" in the help and the tray menu | "Add shortcut for pause/restart vim navigator", asked as `Alt+0 0` and corrected to `Alt+p p` |
| 111 | `5c687e5` | 11-03 | 10 | `draw_element_label` drops the typed prefix: only the characters still to type are drawn, in a box shrunk from the right of the placed one (`match_fg` now colours the grid's typed row letter only) | "Instead of shadowing the typed char, remove the typed char of the navigators so I can view it more cleanly instead of overlapped big boxes" |
| 112 | `29def43` | 11-03 | 10 | A bare `g` that typed because the focus was not known yet still arms `g g` (`s_bare_asked` set by `bare_key_ok`, read on the pass-through path of `handle_down`); the second `g` jumps only where the answer by then is "no text field" and types otherwise; `scroll_end_tap_ms` defaults to 500 (was 300); `log = 1` writes the tap timing and the reason a second tap did not jump | "Sometimes g g doesn't work at once so I have to press this twice": right after a hint click (`FOCUS_CLICK_DELAY_MS` and the classification) or on a page still loading, the first `g` reached the app and the second only armed, so the top needed `g g g` |

| 113 | `c85c40f` | 11-03 | 11 | `add_unlisted_windows`: the scan's window list takes the foreground window and whatever `WindowFromPoint` hits on a 64 px grid of the monitor (`BAND_STEP_PX`, `BAND_MAX` 8) when `EnumWindows` left them out, in front of the listed windows, ordered by `GetWindowBand` (`window_band`; `WinInfo.above`, `log = 2` says "above the desktop band"); `window_info_fill` is the shared window check; `make uiaccess` builds `vim-uiaccess.exe` with `vim.uiaccess.manifest` (`vim.rc` `#ifdef UIACCESS`), `tools/uiaccess_setup.ps1` signs and installs it, `app_has_uiaccess` (TokenUIAccess) is logged at start and stops the tray update | "My vim cannot trace system UI (notification, system app etc) with navigator": the Start menu, Search, the notification centre and banners sit in z-order bands above the desktop, unseen by `EnumWindows`, and the overlay cannot be drawn above them without UIAccess |
| 114 | `45e8e6c` | 11-03 | 11 | `key_center` defaults to `0` (was Enter) | "Change centering shortcut from enter twice into 00" |
| 115 | `fe9d22c` | 11-03 | 11 | `Alt` + `key_center` twice (`alt_center_down`, both Alt paths, both taps swallowed) jumps anywhere; the bare first `0` arms unless the focus is a known text field and the second waits `BARE_WAIT_MS`; a second tap (`g g` or `0 0`) that typed while the focus was still being asked for is deferred (`tap_defer`, `TAP_DEFER_MS` 1000) and acts from `tap_answer` on `WM_INPUT_FOCUS_ANSWER`, posted by the focus worker through `uia_focus_notify` after every answer, if the answer is "no text field", the focus window is the same and no other key came; a digit `key_center` also matches its number-pad key; `log = 1` says why a tap did not jump | "00 doesn't work though; make alt + 00 also work" and "sometimes gg doesn't move to the top, i have to retype them": the second tap landed while the focus answer was still pending (right after a hint click), so both typed and only a third and fourth tap jumped; confirmed working by the user |
| 116 | `c64baeb` | 11-04 | 11 | `uia_scroll_end` takes a `top` flag (`scroll_end_at`: percent 0, `UIA_SCROLL_END_ABOVE`; an element already at the top chains to its parent) and the movement thread asks it before any top jump (`idx == 4`, after the scroll-target lookup like the bottom); the wheel glide goes out only where it finds nothing to scroll; `tap_answer` aims the deferred `g g` with `alt_scroll_auto`; `log = 1` writes which route the top took | "Still gg doesn't move to the top, instead some frames up. It often occurs at initial try": the jump started, but the 64-delta wheel glide came out in Chrome as a few screens up, then nothing, while the bottom through `SetScrollPercent` (`be52799`) never did; the Chrome-side step was not pinned down, the route was; confirmed working by the user |
| 117 | `2c9e4d0` | 11-04 | 12 | The default `alphabet` is `asdfhjklqwertuopzxcvbnm` (home row, top row, bottom row, in keyboard order; was the alphabetical `abcdefhjklmnopqrstuvwxz`), the same in `tools/hint_check.c`; the ini template says the first letters begin every two-letter label | "When it comes to navigator alphabet combinations, as I know vimium uses left hand friendly pattern. I can be wrong so investigate and if there's better option for it, apply that": Vimium's `sadfjklewcmpgh` is the home row of both hands first (Surfingkeys is the left-hand-only one); `build_labels` makes the alphabet's head the two-letter prefixes, and alphabetical put `b` and `c` there |
| 118 | `78cee46` | 11-08 | 13 | `touchpad_off` (default 1, `touchpad.c`): the touchpad is off while vim mouse runs and is not paused, back on when paused, quit or the setting turned off; `tp_state` reads `SPI_GETTOUCHPADPARAMETERS`, `press_toggle` injects Ctrl+Win+F24 only with no modifier physically down, `step` checks the result after `TP_CONFIRM_MS` (600 ms, two presses at most); `main.c` calls `touchpad_update` at start, on `WM_APP_STATE_CHANGED` and reload, `touchpad_stop` in `WM_DESTROY` after `input_stop` | "while vim is enabled, disable touchpad": `SPI_SETTOUCHPADPARAMETERS` ignores `touchpadEnabled`, so the switch is the touchpad key's shortcut |
| 119 | `4ef8776` | 11-08 | 14 | `scan_loading` (default 1): `uia.c` `page_busy` (MSAA `STATE_SYSTEM_BUSY` of the largest visible page window through `WM_GETOBJECT`, `msaa_state`, asked before the foreground search) fills `ClickList.page_busy`; `hint.c` `load_note` keeps the hints open up to `LOAD_LIMIT_MS` (10 s, `scan_hard_limit`) with "Page loading" in the status bar, `load_watch` hooks `EVENT_OBJECT_SHOW`..`EVENT_OBJECT_REORDER` and `IA2_EVENT_DOCUMENT_LOAD_COMPLETE` of the page window's process while watched, `load_content_in` refreshes at once, `relabel_all` makes labels shown while busy afresh unless a label key was typed; `tools/load_probe.c`, `load_probe.mjs`, `vim_post.c` (`make loadprobe`, also in CI) | "when browser loading is slow, navigator cannot find clickable elements during that": a loading page has nothing in its tree until its first content arrives, so the hints labelled only the toolbar or closed after 3 s |
| 120 | `aa3ce3f` | 11-08 | 15 | `esc_unfocus` (default 1; 2 adds Electron and WebView2 apps): `input.c` `unfocus_key_down` arms a plain `Esc` on its way to the app (no menu open, focus not known to be outside text fields), `unfocus_cancel` on any other key and in `reset_key_state`, `handle_up` sends the release; `uia.c` `uia_focus_leave_down` / `_up` / `_cancel` (under `g_fx_cs`), `leave_step` in the focus worker's loop: `leave_look` at the press (browser by `browser_process`, text by caret or `focus_element_kind`, a focused option or an expanded combobox keeps the focus, target `leave_document_above` or `leave_page_document`), `leave_act` after `LEAVE_AFTER_MS` (same focus window, still text, `SetFocus`, `uia_focus_changed`); typed `SetFocus` and `GetFirstChildElementBuildCache` in the vtables | "implement new function: pressing esc disables current text input focus entirely": after typing in a field the keys without Alt needed a click on the page |
| 121 | `806f361` | 11-08 | 15 | `leave_look` no longer keeps the focus for an open suggestion list (a focused option or an expanded combobox; `leave_expanded` and the ExpandCollapse cache properties removed): the page gets the `Esc` first and closes its list, the same press leaves | "on google search, i have to press esc twice": a click into Google's search box opens its suggestions every time |
| 122 | `6c41854` | 11-08 | 16 | `magnet` (default 200 ms, 0-1000): `movement_thread` pauses a held move where `cursor_is_hand` (`GetCursorInfo` against `LoadCursor(NULL, IDC_HAND)`) first shows the hand (`magHold`, `magArmed`, `was_still`), freezing velocity and hold time; release, another move key, Shift or a drag end it; `MagTrail` / `mag_trail_at` take the pointer back by the app's lag when the hand goes off during the pause (`MAG_MAX_LAG_US` 100 ms); help row, ini template | "when mouse pointer changes to finger ... slightly stop instead of just passing by the element ... like magnetize function of adobe premiere pro" |
| 123 | `124dcb1` | 11-08 | 17 | `hold_unfocus` (default 300 ms, 0-2000): `input.c` `hold_key_down` on the pass-through path of `handle_down` (bare scroll key, no modifier, no menu, focus not known outside text fields, `hold_window_ok` browser window) marks the key `OWN_HOLD` and calls `uia_focus_leave_hold`; `hold_repeat` holds repeats back until `uia_focus_leave_result` says `LEAVE_NO`; `hold_cancel` on any other key or modifier, in `alt_resolve` and `reset_key_state`; `hold_key_up` passes the release; `hold_answer` on `WM_INPUT_FOCUS_ANSWER` starts the scroll on `LEAVE_DONE`. `uia.c` `leave_arm`, `g_lv_hold_ms`, `g_lv_res_seq` / `g_lv_res`, `leave_result` (posts the focus message on `LEAVE_DONE`), `leave_step` looks at a hold only after `hold_ms`, `leave_erase` (Backspace in a page, Esc in the browser's UI, `LEAVE_ERASE_MS` 30) before `SetFocus`; help row, ini template | "let pressing d, j, k, u for long enough disable text input focus. focus on usability so the duration must not be too long." |
| 124 | `b5cf4e4` | 11-08 | 18 | `touchpad_update` turns the touchpad on while the keyboard is locked (`input_is_locked`) and off again after unlocking; `modifiers_down` also asks `input_modifier_held` (the hook's `s_down`), since the lock swallows the unlock chord's Ctrl and Alt from `GetAsyncKeyState` | "while ctrl + alt + l is enabled (keyboard lock), allow touchpad" |
| 125 | `650f11e` | 11-08 | 19 | `Alt+=` / `Alt+-` (`key_zoom_in` / `key_zoom_out`, `=` / `-`; alone in mouse mode): `zoom.c` injects two touch contacts either side of the pointer (`InjectTouchInput`, own `VmTouchInfo` declaration, size checked at compile time) and spreads or pinches them every 8 ms while held on its own thread (`zoom_thread`, `pinch`, `grip_place`: 160 to 640 DIP, the 32 DIP slop jumped at once, `zoom_speed` default 150 %/s, a tap one 1.25 step, re-grip at a limit); `pointer_back` after each grip; `mouse_proc` ignores touch's mouse messages (`MI_WP_SIGNATURE`); `zoom_cancel` in `reset_key_state`; help rows, ini template | "alt+= or - to zoom in and out just like with gathering or spreading with touch pad using two fingers on a current mouse pointer ... it's different from ctrl + = and -" |

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
- **The mouse hook (`mouse_proc`) is as fast as the keyboard one, and the
  cursor swap never runs on it.** `hide_pointer` adds a `WH_MOUSE_LL` hook
  on the input thread, called for every mouse event (a gaming mouse: 1000 a
  second): it writes a tick and posts at most one message. `SetSystemCursor`
  (16 calls) and `SPI_SETCURSORS` (reads the cursor files) run on the
  pointer thread; the input thread only sets `s_ptr_want` and signals.
  Anything of ours that moves the pointer with `SetCursorPos` goes through
  `own_move`, or the hook takes it for the real mouse and shows the pointer.
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
- **Keep the CapsLock state off from the input thread, not only the hook.**
  `capslock_off` (default 1): the hook swallows every CapsLock press it
  sees (the mouse-mode key, and with it Shift, Ctrl or Alt + CapsLock, which
  used to pass through as the real CapsLock). The state still comes on
  where the hook is blind: a window running as administrator gets keys a
  normal process's hook never sees, the lock screen, a paused program, or
  CapsLock already on at start. So a timer on the power window
  (`CAPS_CHECK_MS`, 250 ms) and a post from the hook on every CapsLock or
  modifier key-up (`caps_check_soon`) run `caps_check`, which taps
  CapsLock off with `SendInput`. Rules it follows: it reads `GetKeyState`
  (a thread that receives no keyboard input gets the current toggle state:
  Windows resyncs it on the call), never taps while a modifier is held
  (`caps_mods_held`; the key just released may still read as down), never
  injects Shift (with Windows set to turn Caps Lock off by Shift, it logs
  and gives up), backs off when a tap leaves it on (Windows drops input to
  an administrator window: 0.5 to 8 s, reset on a foreground change), and
  leaves it alone while paused or suspended. The hook ignores the injected
  tap like every injected key.
- **A synthetic click inherits the modifiers the user is still holding.**
  The hints' `Shift` + label and `Ctrl` + label fire on the label's key
  down, while the modifier is still physically down, and apps read
  `GetKeyState` when the click arrives: Explorer toggled the selection
  instead of opening on the double click, a browser opened the Ctrl-clicked
  link in a new tab (twice), and Shift + right click showed Explorer's
  extended menu. `click_when_free` in `hint.c` keeps the click until Shift,
  Ctrl and Alt are up (a 10 ms timer on the hidden overlay window, 3 s at
  most so a stuck key cannot lose the click). Only the plain click goes at
  once. Any new modifier-chosen click goes through it; never inject a
  modifier release to get there (the rule above). The hook already reports
  a withheld Alt in `mods`, which is how `Alt` + label (move only) arrives;
  a key typed into the hints marks that Alt consumed (`alt_resolve`), or
  its release would be replayed as a tap into the app once the hints
  closed, and a seen Alt or Win gets the mask key like a hotkey.
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
- **A typed first `g` still arms `g g`.** Right after a click or a page load
  the focus is unknown for up to half a second, so the first `g` types (the
  rule above) and only the second armed; the top needed `g g g`. Now the
  pass-through path arms the tap when the key typed for that reason alone
  (`s_bare_asked`), and the second `g` jumps only where the answer says the
  focus is outside a text field, so a `g` typed into a text field never
  turns into a jump. The window is 500 ms (`scroll_end_tap_ms`); vim itself
  has no timeout for `g g`, and a lone `g` is swallowed or harmless anyway.
  When the second `g` typed for the same reason, `tap_defer` hands the jump
  to the focus answer (`tap_answer`, within `TAP_DEFER_MS`), dropped when
  the answer says text field, the focus window changed or another key
  came; `0 0` (`key_center`) works the same way.
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
- **`Esc` leaves a text field only in what is a browser, and only after
  the app has had it.** `esc_unfocus` finds the field at the `Esc` press
  and moves the focus after the release, checking again that `Esc` left
  the focus in the field. One press: do not hand the first `Esc` to an
  open suggestion list (Google's box opens one on every click, phase 15). The look and the
  move run on the focus worker; the hook only arms, posts and cancels.
  Electron apps are not browsers here (VS Code's editor closes its widgets
  with `Esc`), and native apps cannot be left this way at all.
- **A held scroll key leaves a text field without delaying typing.**
  `hold_unfocus` lets the press type and takes the letter back when it
  leaves; do not hold the press back to avoid the erase (every typed
  `j k u d` would lag, and rolled-over keys would come out of order). Keep
  the look on the worker and after the hold time, not at every press, and
  cancel on any other key, modifiers included, so the erase key is always
  a bare Backspace or Esc.
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
  3 s hard limit (`SCAN_HARD_LIMIT_MS`). Keep both conditions. One
  exception, made when merging had become safe: with a browser active on
  the scanned monitor (`uia_browser_window`, `browser_first`), its labels
  show as soon as it answered (`scan_ready`). In `f6cb7d4`'s time the slower
  windows were lost; now the kept scan merges them in (`keep_scan`, needs
  `scan_refresh`), and `build_labels` plans free codes for the windows still
  out (`ClickList.pending` x `EARLY_ROOM_PER_WINDOW`), so `keep_labels` has
  codes to give them. A browser covers most of its monitor; what is left is
  usually the taskbar, which must not hold its labels back.
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
- **UI Automation cannot say what is on top inside a page, so infer it.**
  Chromium's synchronous `ElementFromPoint` is an approximate bounds
  search that it calls "wrong in cases of z-index" (`CachingAsyncHitTest`).
  It hides the rest of the page only under an `aria-modal` dialog. In web
  content a dialog (UIA Window), an open menu (Menu) or a frame's Document
  is on top of the page under it. `17d27bc` (`cover_overlays`) gives a page
  element whose click point is on one the part of itself outside it. Only
  when nothing is left and a dialog or menu later in the tree (a portal)
  covers it does the element go. A frame never drops anything, because a
  sticky header can sit over an embed. A dialog covering 90% of the page
  or more is not an overlay (apps that are one big dialog). Content is the
  run after the overlay in tree order whose rects touch it, the same idea
  as `content_end`. A menu kept with opacity 0 instead of hidden would
  still count as an overlay (none seen yet).
- **Check filter changes on Linux before Windows.** `tools/ax_sim/run.sh`
  runs `uia.c`'s own filters on Chromium's real tree for a page, and
  `OLD=<rev>` shows what changed. Chromium's AT-SPI tree has the same
  bounds and offscreen state as its UI Automation tree. What it cannot
  show is UI Automation's own view rules and other apps.
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

- **Chromium's IsOffscreen can be wrong where its rects are right.**
  `IsOffscreen` is `BrowserAccessibility::RelativeToAbsoluteBounds` with
  clipping: the walk up the stored tree intersects with every box that
  clips its children (`AXTree::RelativeToTreeBoundsInternal`), and the
  element is offscreen when one of them leaves nothing.
  `BoundingRectangle` is the same walk unclipped. So a clipping box whose
  stored size is stale hides everything inside it from the scan's
  condition while every rect looks right. Google's account menu
  (2026-11-01, `b7871de`): the dialog and its Close button on screen, all
  17 rows of its scrolling list offscreen. UI Automation has no clipped
  rect to check against, so `rescue_offscreen` infers it: a visible dialog
  or menu shows most of what lies inside it. It asks only when a dialog or
  menu shows at most three elements of its own, so pages without one pay
  nothing. The cost: a part of a dialog that is really clipped away (a
  section collapsed to height 0 and kept in the tree, rows under a sticky
  header) gets labels when the dialog shows almost nothing else. A menu
  closed by height 0 whose items fill its rect exactly is recognised and
  left out. Chromium also gives a box of no size the union of its
  children's bounds and never calls it offscreen (the start of
  `RelativeToTreeBoundsInternal`).
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
  measured from our last `SendInput`/`PostMessage`. Any new action that
  changes a page (a new key that scrolls, clicks or navigates) must call
  `page_acted` in `input.c` with its kind (`PAGE_SCROLL`, `PAGE_CLICK`) and
  point.
- **Ask Chromium to catch up instead of waiting for it.** The 500 ms is a
  throttle on location-only messages (`SerializeLocationChanges`). Any
  serialization of tree updates or events takes the pending position and
  scroll changes along (`SerializeUpdatesAndEvents`:
  `TakeLocationChangsForSerialization`), and an action's event is
  serialized immediately, past the 150 ms deferral too. A UI Automation
  `ElementFromPoint` over the page is such an action: the fragment root
  calls `BrowserAccessibility::HitTestSync`, which starts an asynchronous
  hit test in the renderer (`CachingAsyncHitTest`), and the renderer fires
  a hover event with immediate serialization
  (`RenderAccessibilityImpl::HitTest`). The browser applies it in
  `OnLocationChanges` and fires `EVENT_SYSTEM_SCROLLINGEND` (an
  out-of-context WinEvent) for every scroller whose offset changed; ignored
  nodes fire nothing, but Blink keeps scroll containers unignored. So the
  settle wait (`settle_begin`, `settle_step` in `hint.c`, `uia_flush` on the
  flush thread) flushes and listens. After our scroll the first flush goes
  out 30 ms after the last wheel tick (`SETTLE_INPUT_MS`: the wheel has
  reached the page and been applied in a frame), after a click at 300 ms
  (what it opened slides in). A flush answered by a scroll report means
  the page moved since the flush before: flush again, 35 ms later (Blink
  takes a new scroll offset once per frame, so a flush at once can find
  nothing new while the page still moves; the first draft did that). The
  verdict of a flush is read when its hit test returns
  (`WM_APP_FLUSH_DONE`, `hint_flush_done`) plus a 15 ms grace: the
  report, if any, is fired before the browser answers the hit test
  (measured: within a millisecond). A flush with no report then means the
  tree is current: "stopped" when an earlier flush moved the page,
  "asked" when two flushes a couple of frames apart found nothing (one
  alone could fall between the frames of an animation that has only just
  started). A report alone never ends the wait: Chrome with animation
  effects on animates a wheel scroll for 100 to 200 ms (`kInverseDelta`
  6 to 12 frames), Edge can animate longer, and only a flush that finds
  nothing new proves the page stopped. A hit test that did not reach the
  page (`flush_at`: the element found is not the window's process, so
  something covers the page) proves nothing, and the wait runs out
  `scan_settle` as before, as after back / forward (a loading page sends
  no positions). The same wait starts after a scroll the window reported
  itself (the mouse wheel, a touchpad, keys, the page's script), capped at
  300 ms, which covers physical scrolls for the first time; reports within
  `scan_settle` of our own action are its own and start nothing, and a
  page that scrolled through a whole such wait (a live chat) is left alone
  for 10 s. The flush point must be on top in that window and over its
  page area (`uia_page_point`: our action's point if it is on a
  `Chrome_RenderWidgetHostHWND`, else that window's centre), or there is no
  flush. Reports are matched by top-level window, not process: WebView2
  pages run in a process of their own. `scan_flush = 0` restores the fixed
  wait. Reasoned from Chromium's source (2026-11) and measured on Windows
  on 2026-11-03 with `tools/flush_probe.c` (`EVENT_SYSTEM_SCROLLINGEND`
  timed against posted wheel notches and hit tests): the throttle, the
  immediate first report and the flush behave as read. Still unverified:
  that the hit test passes through our click-through overlay while it
  shows the status text (the probe ran with the hints closed; `flush_at`
  now says whether the hit test reached the page, so waits that end at
  the time limit only with the hints open would show it).
- **Start the wait with the action, not with the hotkey.** The first
  version waited only inside an open, and `hint_prepare` and
  `adopt_prescan` refused the prescan whenever a wait was due (any action
  within `scan_settle`): a hotkey 300 to 650 ms after a scroll, the
  common case, waited for a flush and then for the whole search. Now
  `page_acted_at` posts `WM_APP_PAGE_ACTED` (once per 100 ms burst, so a
  scroll stream does not flood the UI thread), `scroll_event_proc` treats
  a scroll the browser reported the same way, and `settle_poke` runs the
  same state machine with the hints closed. `settle_done` writes the end
  tick into `fresh_tick`; `settle_window` ignores actions and reports
  before it, so the open that follows scans at once and keeps its
  prescan. An open during the wait joins it (`settle_begin`,
  `g_settle_wait`) and `settle_done` starts its scan. A newer action moves
  a running wait instead of starting another (`settle_rebase`, on every
  timer step and on each post), since the floor and the proof belong to
  the newest action. Nothing else may end a running wait: `cancel_scan`
  and `start_scan` only drop `g_settle_wait`; killing the wait with the
  overlay would lose the freshness record. A page that scrolls by itself
  is treated by who waited: the hints waiting through a whole wait leave
  it alone for 10 s as before (`g_restless_root`), a wait ahead of the
  hotkey that ran out only stops its own kind for 5 s
  (`SETTLE_BACKOFF_MS`), or a live chat would be flushed without end and
  a long wheel scroll would turn the hotkey's wait off. The flush runs on
  a thread of its own (`flush_worker`): on the scan worker it queued
  behind a scan, and the scan started early would have delayed the very
  flush that confirms it.
- **Measure the browser; do not assume its animation.** The 230 ms floor
  before the first flush was Chrome's worst-case animation, but on this
  machine Chrome does not animate at all: Windows' animation effects are
  off (`SPI_GETCLIENTAREAANIMATION`, which Chrome's
  `gfx::Animation::ScrollAnimationsEnabledBySystem` follows), so every
  scroll paid 200 ms for nothing, and the hotkey (312 to 422 ms after
  the scroll in the log) arrived before the scan had even started.
  `scan_early` reads that setting and starts the scan on the first flush
  that reached the page (`settle_early_start`); the confirming flush
  still runs, and movement after the early scan marks it stale
  (`g_settle_stale`) so `settle_done` scans again, which keeps the proof
  for browsers that do animate. What must hold: the early scan starts
  only on a flush that reached the page; a newer action makes it stale
  (`settle_arm`); an open adopting it keeps `g_settle_wait`, so a stale
  verdict reaches it (`adopt_prescan`, `start_scan`); `cancel_scan`
  drops the early mark of the open's own scan; `settle_blocks`, not
  `settle_needed`, decides whether an open must wait. Measure with
  `make flushprobe` before touching the `SETTLE_*` constants.
- **Chromium shows a page's clickable divs only when they say so
  themselves.** Blink's `AXNodeObject::IsClickable` tests the node's own
  mouse listeners (`HasAnyEventListeners`) and the role; a handler on an
  ancestor gives the element `kClickAncestor`, which `IsClickable` and so
  `IsInvocable` (the UIA Invoke pattern) ignore. React attaches every
  handler to the root container, Google's `jsaction` to the document, so
  their plain divs expose nothing but, with a `tabindex`, the focusable
  state (`UIA_IsKeyboardFocusablePropertyId` = `kFocusable`). Browser
  windows (`scan_web_window`) now search with `web_condition`, the usual
  one OR keyboard-focusable, and `focusable_left_out` keeps of the
  elements that came in that way only those worth a click. What else
  Blink calls focusable, and why each is left out: anything with
  `tabindex="-1"` (`<main>` and headings that scripts focus after a route
  change: Text control type, or over a quarter of the page's visible
  area, or a container of real targets, which `filter_containers` drops
  because a focusable-only element is never actionable); every scroll
  container since keyboard-focusable scrollers (Scroll pattern
  available); the browser's own toolbar focus stops (outside the page
  Document). Iframes are not focusable in Chromium's UIA. A `div` with
  neither role nor `tabindex` nor own handler (only `cursor: pointer`)
  is not in the tree at all; the README says so. Checked on Linux with
  `tools/ax_sim/run.sh focus.html` (Chromium gives a `tabindex` div the
  AT-SPI `section` role with the `focusable` state; `conv.py` marks such
  elements actionable 2 and `mk.py` applies the same test), and against
  `google.html` and `regress.html` with `OLD=` for no change. Not yet
  seen on Windows; `log = 2` writes `focusable only: ...` for each
  element it leaves out. Classic windows keep the old condition.
- **The time is in the app, not in our code; measure before tuning.**
  Asked to make the hints faster "even with assembly", a split of one scan
  showed window enumeration at 0.2 ms, the filters and labels under 1 ms,
  and the rest in the one `FindAllBuildCache` per window: Chrome on a
  Google page 27 to 60 ms, File Explorer on `C:\Windows` about 200 ms,
  Character Map 140 ms. No other query shape was faster (raw view, the
  condition reordered, fewer cached properties, or fetching everything
  and filtering here). A global "nothing changed on screen" test that
  could have shown an earlier scan at once does not exist: DWM composes
  every frame while anything animates, and a 1080p `BitBlt` takes 17 to
  48 ms.
- **Search a classic window's parts at the same time; never a browser's.**
  File Explorer's ~200 ms were its title bar 87 to 99 ms (of that, the
  system menu on the window icon 64 to 70 ms for its one MenuItem, through
  UI Automation's non-client proxy), the file list 27 ms, the folder tree
  23 ms and the command bar 12 to 25 ms, asked one after another. With
  `scan_split` (default 1) the root's children are fetched (Full element
  mode, `kids_cache`), Pane, Group and Custom containers down to depth 3
  are split again, a TitleBar gives its children but its MenuBar, and every
  piece is one `TreeScope_Subtree` FindAll, each a work item on the helper
  pool. The window's last item puts the pieces back in tree order
  (`scan_tree_flatten`; the filters need FindAll's order) and filters once.
  A split container is kept by the same rule the condition applies
  (`cached_matches_condition`, one table `g_scan_types`/`g_scan_patterns`
  for both). Explorer 200 to about 105 ms, Character Map 135 to about
  60 ms, the same targets but the system menu. Chromium windows and
  Firefox are not split: Chromium answers every accessibility call on its
  UI thread one at a time, and splitting was equal or slower. Three faults
  of the first draft are worth knowing for any work queue here: an
  auto-reset event woke one helper for several queued items, so the rest
  waited out a 20 ms poll (one count per item now, `work_sem`); a job
  handed to the helpers before its first items were queued let them find
  nothing outstanding and leave for good, so the scan never ended (queue
  first, then publish `g_job`); and a part UI Automation could not answer
  must leave the rest of the window labelled (`partial`), not blank it.
- **The grid key is read before the label letters, even during the scan.**
  `is_grid_key` is tested in `hint_on_key` ahead of the key queue, so `;`
  opens the grid at once in an app whose scan is finding nothing (the keys
  typed before it are dropped with the scan). A label character set as
  `key_grid` could never be typed, so `config.c` `check_grid_key` warns
  after the whole file is read (the alphabet may come later in the file).
  The hotkey again still reloads what the hotkey opened (`g_start_mode`),
  not the view the user switched to.
- **A full label code cannot take late elements.** Vimium-style labelling
  uses the whole code space, and `keep_labels` may not lengthen a shown
  label, so before `515eb02` an element that appeared during the open got
  one of the few codes left over (0 to 21 with the default alphabet) or
  none. `build_labels` now plans for a quarter more elements, at least an
  alphabet's worth.
- **The alphabet's order is part of the design.** `build_labels` expands
  the alphabet's head first: with `keep_labels` its first letter is always
  a prefix (its codes wait for late elements), the next ones become
  prefixes as the page grows, and the tail letters stay one-letter labels.
  So the default (phase 12) puts the home row first and the bottom row
  last; keep that when adding or removing letters, and keep `g`, `i` and
  `y` out (they read as `q`, `j` and `v` in bold Segoe UI at label size,
  the old program's lesson).
- **Two labels at one corner hide each other.** Labels were drawn at
  their element's top-left in target order, so where a card and the link
  at its corner were both targets (Google: every result block exposes
  Invoke through its `jsaction`, and the title link starts at its corner;
  `tools/hint_check` on the user's query showed the link's label "covered
  by Group"), the later label painted over the earlier one and the user
  saw a single label that clicked the block, "no navigator on the
  title". Now `place_labels` (`hint.c`) finds every label's box once,
  smallest element first, and `labelbox_apart` (`labelbox.h`) moves a
  box that would cover more than a quarter of a placed one (or be
  covered that much) right of it, or below it at the monitor's edge. A
  box is found when the label is first painted and then stays, so with
  `keep_labels` nothing the user reads moves: a merge's new labels go
  around the shown ones. The box is found again only when its element
  moved (`keep_labels = 0`), its text grew (`make_room`) or the fonts
  changed (`g_labels_replace`). `tools/hint_check.c` `layout_labels`
  runs the same code, so its "hidden" count means what the overlay
  draws; its DOM list also counts a `<label>` whose control is hidden.
- **A loading page has nothing to label: ask whether it is busy, then
  listen.** A Chromium document says `STATE_SYSTEM_BUSY` and holds no
  children from the navigation until its first content is in the tree
  (measured 2026-11-08, phase 14), and nothing a scan does makes it come
  sooner. Do not wait for the load event: images and ads hold it back long
  after the links are there. Read the busy flag before the foreground
  window's search, never after it: read after, a page whose content came
  in during the search counts as loaded while its elements are the toolbar
  alone, and no relabel follows (the first draft did that). The content
  reports (`EVENT_OBJECT_SHOW` / `EVENT_OBJECT_REORDER` with `OBJID_CLIENT`
  on the page window) fire for every element of busy pages too: hook them
  only while a page is watched, and only in the page window's process. A
  Chrome window that a background process starts does not come to the
  front (the foreground lock), and a covered one hides its page and
  reports nothing unless started with
  `--disable-features=CalculateNativeWinOcclusion`; both spoiled probe runs
  before `vim_post front` and the flag.
- **An element from the scan cache cannot walk the tree.** The scan's
  cache request uses `AutomationElementMode_None` (the elements carry
  only their cached properties, no reference to the app), and the tree
  walker's `GetNextSiblingElementBuildCache` and friends find nothing
  from such an element: `label_stand_in` labelled none of the mock
  page's hidden checkboxes until the same scan ran with
  `AutomationElementMode_Full` (`tools/hint_check.mjs`, 2026-11-03).
  Full references are not free: Wikipedia's Vim page (116 targets)
  scanned in 141 ms with the plain cache and 140 to 235 ms (mostly
  about 172) with full ones, six runs each. So `full_cache` is used
  only where it pays: a window whose last scan listed a hidden checkbox
  (`full_memory`, per window like the slow memory), the first scan that
  lists one (fetched again at once, `find_all_window`), and `log = 2`.
  Anything new that needs to walk, search or ask current properties from
  a scanned element must come from `full_cache`, `detail_cache` or
  `kids_cache` (full for the split scan), never from `cache`.
- **A page can hide a control behind its label.** Chromium exposes a
  `<label>` that holds more than text as a Group that is not a control
  element, with no name and no `LabeledBy` from the input (null on the
  mock page), and prunes a plain-text label whose text names the input.
  The 1 px input is the only target, so the label's box is found by
  walking from it, within 64 px, and only a Group counts: the hidden
  `.blind` text next to it is a Text of 14x101 px whose centre lies
  below the square. A visible control (`opacity: 0` over the label, 24
  px) needs nothing and gets nothing.
- **System UI lives above the desktop band, where `EnumWindows` does not
  look.** Phase 11: the Start menu and Search (band 6), the notification
  centre and notification banners (band 4) are left out of `EnumWindows`
  and of the UI Automation root's children for a process without UIAccess,
  while `GetForegroundWindow`, `WindowFromPoint`, `GetWindowRect`,
  `IsWindowVisible` and `ElementFromHandle` answer for them.
  `add_unlisted_windows` finds them through the foreground window and a
  64 px `WindowFromPoint` grid and puts them first in z-order
  (`GetWindowBand`, undocumented but exported, orders them; with Start open
  the taskbar moves up there too, and Start's window spans the screen above
  the taskbar, so everything else is "covered"). Nothing in the desktop
  band draws above them: a topmost window loses, and giving a window one
  of them as owner is refused (the owner stays null). Only a UIAccess
  process can, which is a signing and install matter, not a code one.

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
- **Jump to an end through UI Automation, not the wheel.** The glide to the
  top (2 M wheel units over half a second) came out in Chrome as a few
  screens up, then nothing, mostly on a first try, while the bottom through
  `SetScrollPercent` (`be52799`) never did. A wheel jump is at the mercy of
  the app's wheel handling (Chrome's latched sequence and animation
  retargeting, a page's own wheel handler); a ScrollPattern percent lands
  exactly and at once. Both ends go through `uia_scroll_end` first; the
  glide is only the fallback where UI Automation finds nothing to scroll.
- **Redirecting the wheel can scroll the wrong panel.** `729e682`: a cached
  list up to 10 s old sent the wheel to the nearest area. `e987c6a`: Teams
  does not report its conversation as scrollable, so a redirect only goes to
  an area in line with the pointer or at most 64 px beside it. If in doubt,
  leave the wheel at the pointer.
- **Never wait in the movement thread.** Until phase 9, every Alt or bare
  scroll press waited there for the scroll-target list (up to 500 ms from a
  cold cache, 150 ms from a stale one): the scroll started late, and the
  pointer, driven by the same thread, stopped with it. Anything the thread
  needs from UI Automation is asked without waiting and polled each tick
  (`uia_scroll_aim`), and the stream starts under the pointer meanwhile.
  What a tap sent to the pointer while asking is sent again to the target
  once known (`scrOwed`): a redirect means the pointer's spot does not
  scroll that way, so nothing was lost there. `uia_scroll_end` (`Shift+g`)
  still waits up to 300 ms: one shot, rare.
- **A window shown again shows what it last painted.** The overlay hid
  with the labels still in its surface, and the next open painted only
  once its adopted labels were built and drawn (several ms), so one frame
  of the old labels showed first: "the navigator appears twice, the first
  one flickering" (phase 9). `paint_blank` fills the window with the key
  colour before it hides (`hint_close`) and before it moves to another
  screen (`cycle_monitor`). Anything that shows or moves that window
  must leave it blank first, or paint it before the next frame.
- **Do the scan before the key, not after it.** UI Automation needs 100 to
  400 ms for a window, and after a scroll a Chromium page another 330 ms
  to report where things are; nothing makes that faster once the hotkey
  is down. `scan_ahead` (phase 9) runs the scan when the screen changes,
  so the hotkey adopts it. What must hold: a scan ahead is dropped by
  every sign of change this program can see (its own actions, browser
  scroll reports, foreground and focus changes, keys that reached the
  app), it is never older than `AHEAD_MAX_AGE_MS` when adopted, and an
  adopted one older than `AHEAD_REFRESH_MS` is refreshed at once. Signs
  it cannot see: physical mouse clicks and wheel scrolls outside
  Chromium, and a page changing by itself; `keep_labels` holds a label
  where it was drawn, so those cost at most 3 s of staleness.
- **The cursor shape trails the pointer.** An app sets the hand only once
  it has handled the move (Chromium on its next frame), so the shape seen
  in a tick describes where the pointer was, not where it is. Anything
  that reacts to the shape (the magnet, phase 16) must expect to be past
  the element by then and keep the recent path to go back along.
- **Anything that follows the pointer moves with it, not on a timer.** The
  `MOUSE` pill on a 15 ms timer trailed a pointer that moves every
  millisecond and stepped after it. `indicator_moved` posts an update from
  each pointer move (one in flight at a time); the timer stays for the
  physical mouse. The pointer path itself was measured on the user's
  machine before this (a scratch program making the same absolute
  `SendInput` moves on the 1 ms timer): every move landed exactly and was
  visible at the next tick, with ticks of 1.66 ms. Measure before tuning
  speeds.

- **A touchpad gesture is injected as touch, from its own thread.**
  Windows injects touch contacts but not touchpad ones, and apps take a
  two-finger touch pinch for their touchpad pinch (phase 19). Contacts in
  contact must be updated every frame, so the pinch runs on `zoom.c`'s
  thread, never on the hook. Touch may move the system pointer to the
  first contact: put it back after the contacts lift (`pointer_back`), and keep
  the mouse hook from taking touch's mouse messages (`MI_WP_SIGNATURE`)
  for the real mouse. Mind the browser's slop: a contact that moves less
  than it does nothing, so a tap must still move past it.

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
- **A scheduled shutdown must never surprise.** `shutdown.c` keeps times on
  the wall clock in UTC (FILETIME units), never in ticks: a timer does not
  run while the computer sleeps, and a shutdown it slept through by more
  than 5 minutes (`MISSED_MS`) is skipped with a balloon, not carried out the
  moment someone wakes the computer. A late wake inside that, or a sleep
  with the countdown on screen (a check more than `GAP_MS` after its tick),
  shows the countdown in full again; the clock set back closes it until it
  is due. `WM_TIMECHANGE` comes on waking too (Windows resyncs the clock),
  before the timer's tick: it must keep `g_prev_now` so the jump still
  reads as a sleep, and leave a `shutdown_at` already due to `sd_check`.
  The countdown takes the focus so `Esc` can cancel it, so whatever is
  typed into it by surprise must be harmless: no mnemonics, Cancel is the
  default button, and "Shut down now" is neither a tab stop nor in the
  others' group, so it can only be clicked, and it is disabled for the
  first second (`NOW_DELAY_MS`) so a click on its way cannot hit it.
  `shutdown_warning` stays under "10 more minutes" (0-300 s), or postponing
  would reopen the countdown at once. Shutdowns close together are one to
  the user (`consume_between`): `power_off` uses up everything due before
  its countdown could end, and Cancel what is due within a minute of the
  one cancelled. The shutdown is used up (one-off cleared, `shutdown_at`
  day marked done in `g_daily_done`, a local day number) before
  `ExitWindowsEx`, so an app that stops it is not asked again a second
  later. A one-off replaces only the next `shutdown_at` day before it, and
  gives it back when cancelled or replaced (`undo_replace`) unless that day
  is too close to count down in full. `shutdown_at` is turned into UTC for
  its own date (`TzSpecificLocalTimeToSystemTime`), so daylight saving does
  not move it, and `WM_TIMECHANGE` recomputes it for a new time zone. A
  one-off lives in memory only; quitting or updating drops it.
- **Turning the computer off needs the shutdown privilege enabled first.**
  `power_off` enables `SeShutdownPrivilege` on the process token, then
  `ExitWindowsEx(EWX_SHUTDOWN | EWX_POWEROFF | EWX_FORCEIFHUNG)` (a full
  shutdown, like `shutdown /s`, not the Start menu's fast-startup one).
  `EWX_FORCE` only with `shutdown_force`, since it loses unsaved work.
- **The touchpad switch is a key, not a setting.** Phase 13:
  `SPI_SETTOUCHPADPARAMETERS` returns success and ignores
  `touchpadEnabled`, so `touchpad.c` presses Ctrl+Win+F24, which toggles.
  Never press it without reading the state first and again after, and never
  while a modifier is physically held (an injected Ctrl or Win up would
  release the user's own key, and a held Alt turns the shortcut into
  another one). It persists like the Settings toggle: a crash, a kill or a
  Windows shutdown with vim mouse running leaves the touchpad off until vim
  mouse pauses or quits. "Physically held" means `GetAsyncKeyState` or the
  hook's `s_down` (`input_modifier_held`): keys the hook swallows, such as
  the lock's unlock chord, never reach `GetAsyncKeyState` (phase 18). An
  elevated window in front swallows the keys
  (UIPI); the press is retried once, logged, and tried again at the next
  pause or resume.
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
| Outside mouse mode the pointer is hidden; the real mouse shows it for `hide_pointer` ms (2000; `hide_pointer = 0` restores) | phase 9 | Always visible |
| A hint click puts the pointer back where it was, except in mouse mode (`click_return = 0` restores) | phase 9 | The pointer stayed on the clicked element |
| CapsLock tap toggles mouse mode, hold is momentary | phase 1, `0bb6577` | CapsLock *state* meant mouse mode |
| The CapsLock state stays off while vim mouse runs; `Shift+CapsLock` does nothing (`capslock_off = 0` restores it as the real CapsLock) | `12502dc` | `Shift+CapsLock` was the real CapsLock, and the state stayed on after an administrator window or the lock screen until turned off by hand |
| Hints label UIA elements; the grid opens only with `hint_mode = grid` | `64a5805`, `e51fecb` | Grid only (phase 0 and 1); grid as automatic fallback (`64a5805` to `1e8677d`) |
| A scan that finds nothing retries up to 3 s, then closes the hints | `8b0a194`, `e51fecb` | Opening the grid |
| A browser page still loading keeps the hints open up to 10 s ("Page loading"), its content starts a rescan at once, and labels shown while it was busy are made afresh if no label key was typed (`scan_loading = 0` restores) | phase 14 | The toolbar's labels stayed and the page's links got only the free codes, or the hints closed after 3 s |
| Hotkey again rescans (and forgets slow-window skips) | `1e8677d`, `729e682` | Opening the grid |
| Labels refresh while open and never change meaning; shown labels stay put (`keep_labels = 0` restores the moving merge) | `8b0a194`, `33a979f` | Static labels; labels that followed their elements, vanished and grew letters (`8b0a194` to `33a979f`) |
| `Alt+h` / `Alt+l` go back / forward; `Alt+,` / `Alt+.` scroll sideways (`alt_nav = 0` restores) | `b474c89` | `Alt+h/l` scrolled sideways (`09b7fdb`) |
| `g g` / `Shift+g` scroll to the top / bottom; `Alt+g g` / `Alt+Shift+g` in text fields | `6622ce0` | `Alt+u u` / `Alt+d d` (`b54285f`) |
| The bottom jump fires on press through UI Automation; where that cannot scroll, the wheel goes after Shift is released | `be52799`, `ba62724` | Wheel at once (sideways in browsers, before `ba62724`); wheel only after Shift release (`ba62724` to `be52799`) |
| The top jump (`g g`) goes through UI Automation too; the wheel glide only where that finds nothing to scroll | phase 11 | The wheel glide alone (`aad1eaa` to phase 11), cut short in Chrome |
| Scroll-to-end glides | `aad1eaa` | 64-event burst (still used with `smooth_scroll = 0`) |
| After our own scroll or click in a Chromium window, the first scan waits for the page, at most `scan_settle` (650 ms; `scan_settle = 0` restores) | `515eb02` | Scan at once, labelling where the page had been |
| That wait asks the browser to send its changes (`uia_flush`) and ends when they are in, about 330 ms after a scroll (back / forward keep the fixed wait); scrolls the browser reports itself start it too, up to 300 ms (`scan_flush = 0` restores the fixed wait) | `77ec7f4`, `c33324e` | A fixed `scan_settle` after our own actions only; physical scrolls labelled the old page |
| The first flush goes out 30 ms after the last wheel tick and its verdict is read when it returns; with Windows' animation effects off the scan starts on that first flush and the next one only confirms it (`scan_early = 0` restores the scan after the confirmation) | `a630c0f` | The first flush at 230 ms, a 70 ms answer timer, the scan only after the confirming flush |
| With a browser active, its labels show as soon as it answered; other windows' merge in (`browser_first = 0` restores) | `77ec7f4`, `c33324e` | Every window waited for up to `scan_timeout` |
| The wait for a Chromium page starts with our scroll or click, or with the scroll the browser reported, hints open or not; an open after it scans at once with its prescan, an open during it joins it | `1514be3` | The wait started when the hotkey came, and the prescan was refused for `scan_settle` after any action |
| In browser windows, keyboard-focusable elements without a clickable type or pattern get labels unless text, scroll areas, outside the page or over a quarter of it (`scan_focusable = 0` restores) | `1514be3` | Only control types and the four patterns |
| With `keep_labels`, the first labels leave free codes (a quarter more, at least an alphabet's worth); up to three one-letter labels become two letters | `515eb02` | Exactly as many codes as elements |
| A page element under a web dialog, menu or frame is labelled and clicked on its uncovered part, or not at all under a dialog or menu that covers it | `17d27bc` | Labels under a panel clicked the panel |
| In a Chromium window, the offscreen elements inside a web dialog or menu that shows almost nothing of its own are labelled | `b7871de` | Google's account menu had no labels (Chromium calls its rows offscreen) |
| A classic window's parts are searched at the same time and the title bar's system menu gets no label (`scan_split = 0` restores) | `03cb03e` | One search per window |
| `;` switches between the element labels and the grid while the hints are open (`key_grid = none` restores) | `1b6a156` | The grid only with `hint_mode = grid` |
| A right, double or middle hint click is sent once Shift and Ctrl are up; `Alt` + label moves only; `Ctrl+Shift` + label middle-clicks | `1b6a156` | Clicks sent with the modifier still down (Ctrl+click semantics in the app) |
| Bare keys outside text fields (`bare_keys = 0` restores) | `b474c89` | Alt always needed |
| Text shortcuts are pasted through the clipboard (`shortcut_method = type` restores) | `5b3d7e4` | Typed with `KEYEVENTF_UNICODE` |
| Settings next to the exe, else `%APPDATA%\vim mouse` | `e04d6a0` | Exe folder only |
| Defaults: movement 600 to 2400 px/s over 0.45 s; scroll 10 to 40 notches/s; `scan_timeout` 400 ms | `f6cb7d4` | 500 to 1500 over 0.65 s; 5 to 20 notches/s; 1000 ms |
| Two labels never sit on one spot: the smaller element keeps its corner, the other label moves right of it, or below it at the screen's edge (`labels_apart = 0` restores) | phase 10 | Every label at its element's top-left corner, so a card's label hid the label of the link at its corner (Google's result title) |
| In a browser, a checkbox or radio button hidden behind its label (a 1 px input) gets the label's box (`scan_hidden_checkbox = 0` restores) | phase 10 | The input was too small for a label and the label itself is never a target |
| `Alt+P P` pauses and resumes everything (`pause_tap_ms = 0` makes it one press; `pause_hotkey = none` restores) | phase 10 | The pause hotkey was unbound; pausing only from the tray menu |
| An element label drops its typed characters and shrinks to what is still to type | phase 10 | The whole label stayed, the typed prefix coloured `match_fg` (the grid still does that) |
| The touchpad is off while vim mouse runs, back on while paused and after quitting (`touchpad_off = 0` restores) | phase 13 | The touchpad was left alone |
| The touchpad is on while the keyboard is locked, off again after unlocking (with `touchpad_off`) | phase 18 | The touchpad stayed off under the lock, so only a mouse worked |
| In a web browser, `Esc` in a text field (or the address bar) moves the focus to the page once the field has had the `Esc`, one press even with its suggestion list open (`esc_unfocus = 0` restores, 2 adds Electron and WebView2 apps) | phase 15 | `Esc` only did what the field did with it; the keys without Alt needed a click on the page first |
| A held move key pauses the pointer `magnet` ms (200) where the cursor turns into the hand (`magnet = 0` restores) | phase 16 | The pointer passed links at full speed |
| In a browser text field, `j k u d` held `hold_unfocus` ms (300) take their letter back, leave the field like `Esc` and scroll (`hold_unfocus = 0` restores) | phase 17 | A held letter repeated in the field; leaving needed `Esc` or a click |
| `Alt+=` / `Alt+-` zoom around the pointer with a two-finger touch pinch while held, not the page zoom (`key_zoom_in = none`, `key_zoom_out = none` restore the app's own keys) | phase 19 | The keys reached the app (Excel: AutoSum) |
| The label alphabet is `asdfhjklqwertuopzxcvbnm`: home row, top row, bottom row (`alphabet = abcdefhjklmnopqrstuvwxz` restores) | phase 12 | Alphabetical minus `g`, `i`, `y`, from the old program: `b` and `c` were among the two-letter prefixes |

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
- **Late browser trees** (`515eb02`, and the flush that replaced most of
  its wait) was reasoned from Chromium's source
  (`GetLocationSerializationDelay`, `GetDeferredEventsDelay`,
  `RenderAccessibilityImpl::HitTest`, `OnLocationChanges`) and measured
  on Windows on 2026-11-03 (section 5): the flush, the throttle and the
  reports behave as read. Check with `log = 1`: scroll with `j` and press
  `f` straight away; `vim.log` should say "the page caught up" about 50
  to 150 ms after the scroll ("stopped" or "asked"; with Windows'
  animation effects off also "scanned on the first flush"), and the
  labels sit on the scrolled page. Then
  wheel with the mouse and press `f`: a short wait ("asked" or "stopped"),
  labels in place. Click the Google account avatar through a hint and press
  `f` at once: labels on the panel. A "time limit" ending after our own
  scroll means no report came back (a page that did not move, or a flush
  that did not reach it); if it happens on ordinary pages, the hit test is
  not reaching the renderer and `uia_page_point` needs another point. Since
  the wait starts with the action, most of those lines should end with
  "ahead of the hotkey", and a hotkey pressed later than about 150 ms
  after a scroll should log no "waiting" line at all; if the hints still
  feel slow there, the "N targets after M ms" line says how long the
  browser's own search took, which is the part left (a long page). Still
  open: physical mouse clicks (no settle wait); page elements behind a
  non-modal overlay are still in the tree and get labels drawn over it
  (Chromium hides the rest of the page only for `aria-modal` dialogs,
  `IsBlockedByAriaModalDialog`).
- **Keyboard-focusable elements** (`scan_focusable`, section 5) were
  reasoned from Chromium's source and checked on Linux, not on Windows.
  Two things to look at there: labels on things that do nothing (a
  `tabindex="-1"` box of middling size, a toast; `log = 2` names the
  element with `kept`, and `scan_focusable = 0` is the way out), and the
  cost of one more property per element in a browser's search ("N targets
  after M ms" before and after). The scroll-area test (Scroll pattern)
  cannot be simulated on Linux.
- **Google's account menu and Zendesk overlays without labels.** After
  `515eb02` the user reported the account menu (from a Google results page)
  still unlabelled, and not a timing problem. `443b135` (panel rows dropped
  as containers) and `17d27bc` (labels under a panel clicking it) fixed
  real faults found on mock pages, but not this one. On 2026-11-01 an
  agent linked to the user's laptop ran `uia_probe -d` with the menu open
  (the user switched to the tab; the agent cannot click in Chrome) and
  found the cause: UI Automation lists the "Account and settings" dialog
  and its Close button on screen, and the 17 rows of its scrolling list at
  the right places but offscreen (section 5). `b7871de` takes them back.
  It is confirmed by replaying that scan in `tools/ax_sim`
  (`pages/google_menu_win.txt`), not yet by seeing the labels on the menu
  on Windows: open the menu, press `f`, and expect labels on its rows
  (`log = 2` says "offscreen per Chromium, taken back"). Zendesk was not
  seen again; if its overlays still have no labels, the `log = 2` lines
  for one say whether it is the same case (rows offscreen inside a dialog
  or menu that shows a few elements of its own, or more than three).
  The menu's markup (read through Claude in Chrome): a cross-origin frame
  (`name="account"`, `role="presentation"`, 424x899) that Google loads
  ahead of time in an absolutely placed box with `aria-hidden="true"`,
  `visibility: hidden` and height 0; the avatar click sets
  `aria-hidden="false"`, visibility and the height. `google.html`
  `mode=ogs` (and `late=` for a later height) does the same, but Chromium
  on Linux keeps its clip right there, so the stale box is not reproduced
  on Linux.
- **Scheduled shutdown** (`shutdown.c`) was checked with
  `tools/shutdown_test.c` and by driving `vim.exe` under Wine, not on
  Windows. To check: "In 15 minutes" from the tray, then the countdown a
  minute before, Cancel, 10 more minutes and `Esc` there, and one real
  shutdown; an app with unsaved work (Notepad) holding it up with
  `shutdown_force = 0` and closed with 1; sleeping through a one-off
  (expect "Shutdown skipped" on waking); `shutdown_at` a few minutes ahead,
  then cancelling that day from the tray. `log = 1` writes a "shutdown:"
  line for each step.
- **Hidden checkboxes** (`label_stand_in`, phase 10) were seen working on
  a mock page (`tools/hint_check.mjs` on `tools/ax_sim/pages/label.html`,
  Naver's `.blind` pattern: the four variants that leave a 1 px input in
  the tree get their label's box), and the user confirmed the label on
  `mail.naver.com`'s "select all" square the same day (this session
  could not open the site: the browser extension refuses webmail). Not
  yet looked at there: each row's box, and whether a long list runs
  past `STAND_IN_MAX`; `log = 2` says "hidden behind its label, which
  stands in" for each box taken. If a page has more than
  `STAND_IN_MAX` (40) such boxes on screen, the rest stay unlabelled. A
  `<label>` with plain text for a 1 px input ("Remember me") stays
  unlabelled by design: Chromium prunes that label as redundant and
  nothing in the tree holds the text's place. A question this raised:
  `rescue_offscreen` (`b7871de`) asks `FindAllBuildCache` under an
  element from the plain scan cache, which carries no reference to the
  app (section 5: the tree walks from such an element find nothing). If
  Google's account menu still has no labels, that FindAll may be failing
  for the same reason; `log = 2` would then never say "offscreen per
  Chromium, taken back", and the fix is to take the dialog or menu from
  `full_cache` (or ask again under the root with `kids_cache`).
- **Labels apart** (`labelbox_apart`) was checked with `tools/hint_check`
  on a mock card (an `onclick` div holding a title link at its corner: both
  labelled, none hidden), not on Google itself: the checker's fresh
  Chrome profile got Google's bot check (`sei=` page) after a few runs.
  On the user's query the earlier run showed the cause (the link's label
  "covered by Group"), so expect two labels at a result's corner now: the
  link's at the corner, the block's to its right.
- **The UIAccess install** (phase 11: `make uiaccess`,
  `tools/uiaccess_setup.ps1`) was built and the script parsed and tried on
  its error path on 2026-11-03, but trusting the certificate needs an
  administrator prompt, so it has not run on the user's machine. To check:
  run the script, then `vim.log` (in `%APPDATA%\vim mouse` from then on)
  must start with "uiaccess: on", and `Alt+F` must show labels on the Start
  menu, the notification centre and a banner (`f` with Start open types
  into its search box). The expectation that a topmost window of a
  UIAccess process sits above those bands comes from the on-screen
  keyboard and Magnifier, which do; if the labels still hide,
  `GetWindowBand` on the overlay (`tools/uia_probe.c` could print it) says
  which band it got, 1 being the desktop band. The scan side is checked:
  with the probe, Start, Search, the notification centre and a banner are
  listed "above the desktop band" and answer with their elements.
- **Pages still loading, beyond phase 14.** Firefox is not asked whether
  its page is busy (`page_busy` is Chromium only). The wait after vim
  mouse's own click or back / forward still runs out `scan_settle`
  (650 ms) on a loading page before the scan that then notices it; a
  page that keeps `aria-busy` on its root would keep the hints waiting
  up to 10 s when nothing is found. Only tested on local mock pages in
  Chrome; not yet seen on a real slow site.
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
   state in `reset_key_state`. A key that must still reach the app
   (`key_center`) is matched in `center_key_down` instead, on the
   pass-through paths of `handle_down`; its Alt form in `alt_center_down`,
   on both Alt paths (`handle_down`, `alt_resolve`). A two-tap key whose
   second tap may type while the focus is still being asked for defers to
   the answer through `tap_defer` / `tap_answer` (a `DEFER_*` value). A
   key that reaches the app and acts after its release (`Esc` for
   `esc_unfocus`) arms in `handle_down`, cancels on any other key
   (`unfocus_cancel`, both paths and `reset_key_state`) and fires from
   `handle_up`. A key that reaches the app and acts once held
   (`hold_unfocus`) takes an owner of its own (`OWN_HOLD`) for its repeats
   and release, and its action comes back from the worker as a message to
   the input thread (`hold_answer`).
4. `snippet.c` `first_key_taken`: if it is a new Alt-key letter.
5. `tray.c` `build_help_rows`: the help window.
6. `README.md`: the Keys tables, Settings, and Troubleshooting if it takes a
   letter away from apps.

A hotkey of `[general]` (a `Hotkey`: hint, quit, pause, lock) is matched in
`handle_hotkeys`, the first call for every key down on both the normal path
(`handle_down`) and the withheld-Alt path (`alt_resolve`); a hotkey that
takes two presses keeps its pending state there (`s_pause_tap`), cleared by
any other key. Its `format_hotkey` text reaches the tray menu, the help and
`snippet.c` `first_key_taken`, which already covers an `Alt` + letter hotkey.

`6622ce0` is the reference example: it touched all of these. A key of the
hint overlay (`key_grid`) lives in `[hints]` instead: `process_hints`, the
`[hints]` part of `build_default_ini`, `hint.c` `element_key` / `grid_key` /
`hint_on_key` and `build_status`, and the "Hint mode" rows of the help.

### Add a setting

`common.h` field and comment, then `config.c`: `config_set_defaults`, the
right `process_*` function with an `apply_*` helper (range-checked, with a
warning on bad values), and `build_default_ini`. Then the `README.md`
Settings section. If a running thread caches the value, react in
`input_config_changed` (input) or on `WM_APP_RELOAD_CONFIG`. Reload is live:
the watcher thread notices the file change.

### Change when the pointer is hidden

`input.c`: `ptr_update` is the one decision (mouse mode, pause, away,
idle time); `ptr_schedule` asks for it from any thread; `mouse_proc` is
what counts as the real mouse; `ptr_cursors_blank` lists the cursor ids.
Anything new that moves the pointer with `SetCursorPos` must use
`own_move`. The README's "The pointer stays out of the way" paragraph and
the "pointer is gone" troubleshooting entry describe it.

### Change hint scanning or filtering

Where a label's box is drawn: `labelbox.h` (`labelbox_natural`,
`labelbox_apart`), used by `hint.c` `place_labels` (once per label, before
the paint that first shows it; `HintLabel.placed`) and by
`tools/hint_check.c` `layout_labels`, which must stay the same code.
Change both when the box geometry changes.

`uia.c`: `scan_root_item`, `scan_split_item`, `scan_titlebar_item` and
`scan_window_finish` (how a window is split into parts on the helper pool
and put back in tree order; `scan_can_split` says which windows),
`g_scan_types` / `g_scan_patterns` and `uia_build_condition` (what is
fetched; `web_condition` adds keyboard-focusable elements for
`scan_web_window` windows, and `focusable_left_out` in `scan_filter`
judges those), `scan_filter` (everything after the elements are in;
`scan_window_uia` is the single search), `scan_layout` (scroll bars,
Documents, dialogs and menus), `rescue_offscreen` (offscreen elements
taken back into a dialog or menu), `filter_containers` and
`choose_outer_click_point` (containers and click points), `cover_overlays`
(elements under a web dialog, menu or frame), `slow_check` and
`lazy_tree_window` (skip rules), `uia_scan_monitor` (window list and
merging; `add_unlisted_windows` adds the windows above the desktop band that
`EnumWindows` leaves out, `BAND_STEP_PX`, `BAND_MAX`, `window_band`), `find_all_window` (a window's FindAll: full references through
`full_cache` for a window `full_memory` remembers, fetched once more when
`tiny_boxes_listed` first sees a hidden checkbox) and `label_stand_in`
(that checkbox's label, from `scan_filter`; `STAND_IN_MAX`,
`STAND_IN_NEAR_PX`). Verify with `make probe` (`tools/uia_probe.exe -v`) and, for web
pages, `make check` then `node tools/hint_check.mjs <url>`, which reports
misses with the drop reason (`tools/ax_sim/pages/label.html` is the mock
page for hidden checkboxes and a card holding a link at its corner).
`powershell -File tools/uia_dump.ps1 -TitlePart <part> -All` prints a
window's raw UI Automation tree (type, rect, offscreen, patterns,
`LabeledBy`) without bringing it to the front; `-Match` (or `-MatchB64`
for Korean) prints the chain around a named element. `tools/uia_probe.exe -1` scans with one
search per window, to compare with the split. `tools/uia_probe.exe -d` writes the
`log = 2` lines for the active window without a key press (`-w 5` to
bring it to the front first). Without Windows, run `tools/ax_sim/run.sh` on a mock page (add one to
`tools/ax_sim/pages` for a new layout, and keep `mk.py`'s main in step
with `scan_window_uia`; `INPUT=<file>` replays a scan written down from
Windows, offscreen flags included). For a window or panel without labels on Windows,
`log = 2` lists every element of the active window with the reason (a
focusable-only element: `focusable only: ...`; `focus.html` is the mock
page for those). Overlay timing lives in `hint.c` (`settle_poke`,
`settle_arm`, `settle_begin`, `settle_rebase`, `settle_step`,
`settle_answered`, `settle_done`, `settle_early_start`, `settle_blocks`,
`hint_flush_done`, `scan_ready`, `start_scan`, `begin_scan`,
`adopt_prescan`, `finalize_scan`, `scan_limit_ms`, `SCAN_HARD_LIMIT_MS`,
the `SETTLE_*` constants); the wait's triggers are `hint_page_acted`
(from `input.c` `page_acted_at`) and `scroll_event_proc`; the flush
itself is `uia.c` `flush_worker` and `flush_at`. `make flushprobe`
(`tools/flush_probe.exe`, with Chrome in front) posts a wheel notch to
the page, sends hit tests at chosen times and prints every scroll report
with its time: how the browser behaves on that machine. The scan ahead of the hotkey (`scan_ahead`):
`ahead_start`, `ahead_forget`, `ahead_schedule`, `fg_event_proc`,
`TIMER_AHEAD_ID`, the `AHEAD_*` constants, `g_pre_kind`; what drops it is
listed above `ahead_forget`. The hook tells it about keys that reached the
app (`input.c` `ahead_key_passed`, `HINT_PREPARE_FORGET`). A page still
loading (`scan_loading`): `uia.c` `page_busy` (asked in `uia_scan_monitor`
before the foreground search) and `msaa_state`; `hint.c` `load_note` (every
result with the foreground window in), `load_watch` and `load_event_proc`
(the hooks), `load_content_in` (`TIMER_LOAD_ID`), `relabel_all` (from the
refresh path of `hint_on_elements`), `scan_hard_limit`, `LOAD_LIMIT_MS`,
`LOAD_GATHER_MS`. `make loadprobe`, then `node tools/load_probe.mjs` shows
what the browser reports while slow pages load; `--open` adds a live test
of the overlay (it takes the keyboard for a few seconds each time).

### Change scrolling or movement

`input.c` `movement_thread` (streams, glides, timer), `scroll_press` /
`scroll_release`, `scroll_end_press`, `post_wheel`, `send_wheel_jump`. The
jumps to an end: `uia.c` `scroll_end_at` (`uia_scroll_end`, its `top` flag),
asked by the movement thread before any wheel glide.
Targeting and its cache: `uia.c` `uia_scroll_aim` (never waits; the movement
thread polls it each tick and re-aims the streams, `aim_asking`, `scrOwed`),
`scroll_area_answer`, `scroll_side_gap`, the `SCROLL_*_MS` constants.
Re-check the Teams cases in section 5 after any targeting change. The mode
indicator: `indicator_update`, `indicator_moved`. The magnet (phase 16):
`magHold` and its neighbours in `movement_thread`, `cursor_is_hand`,
`MagTrail`; a pause must stay something the user can always leave at once
(release, another key, `Shift`).

### Add a tray menu item

`tray.c`: an `IDM_*` id, `append_item` in `show_menu`, handling in
`tray_on_command`. State shown in the menu or icon comes from `tray_update`
(`WM_APP_STATE_CHANGED`).

A submenu works the same way: `append_shutdown_menu` builds one with
`CreatePopupMenu` and appends it with `MF_POPUP`; its items' `IDM_*` ids
reach `tray_on_command` like any other.

### Scheduled shutdown

`shutdown.c`: the state is `g_once_due` (tray one-off) and `g_daily_due`
(next `shutdown_at`), and `sd_check` is the one place that decides, on
every timer tick and after every change: skip a missed one, open the
countdown, or `power_off`. A new way to schedule calls `schedule_once`
(it replaces the next `shutdown_at` day before it, restorably, and says
so); anything that uses shutdowns up goes through `consume`,
`consume_between` or `undo_replace`. The countdown's
buttons are in `cd_wnd_proc` and `countdown_open` (keep "Shut down now"
off the keyboard); the typed time or wait is `parse_when`, clock times
`parse_clock` in `config.c`. Settings are `shutdown_at`, `shutdown_warning`
and `shutdown_force` in `[general]` (`process_general`); `main.c` routes
`WM_TIMECHANGE`, the reload (`shutdown_config_changed`) and the dialog keys
(`shutdown_dialog_message`). Run `make shutdown-test` and
`tools/shutdown_test.exe` after a change (on Linux: `wine
tools/shutdown_test.exe`, with `TZ=America/New_York` for the daylight
saving case); add a scenario there for new behaviour. It includes
`shutdown.c` with `GetSystemTimeAsFileTime` and `ExitWindowsEx` defined to
fakes, so it reaches the statics and never shuts down.

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

### The touchpad switch

`touchpad.c`: `touchpad_update` picks the goal (off, on, or leave alone)
from `touchpad_off`, pause and the keyboard lock, and `set_goal` acts only when it changes;
`step` is the one place that reads the state, waits for the modifiers and
presses Ctrl+Win+F24. `main.c` calls `touchpad_update` at start, on
`WM_APP_STATE_CHANGED` and after a reload, and `touchpad_stop` in
`WM_DESTROY`. Test on a machine with a precision touchpad: `tp_state`'s
value can be read from a console tool with the same struct.

### Pinch zoom

`zoom.c`: `grip_place` (where the fingers go and how far apart),
`grip_span` (the spread over time, `zoom_speed`), `pinch` (one press:
grips, the tap minimum, the limits, re-grips), the `ZOOM_*` constants.
The keys: `input.c` `zoom_key_down` (both Alt paths, `handle_down` and
`alt_resolve`), `binding_action` (mouse mode), the `OWN_MOVE` release in
`handle_up`, `zoom_cancel` in `reset_key_state`; `zoom_start` /
`zoom_stop` beside the movement thread. Test in Chrome on a real page: a
held `Alt+=` magnifies around the pointer without reflowing, a tap zooms
one step, `Alt+-` back to normal, and the pointer stays put.

### Bump the version or change the icon

Version: `version.h` only (numbers and both strings). Icon: edit
`tools/make_icon.c`, run `make icon` on Windows, and commit `vim.ico`.

### The UIAccess build

`make uiaccess` links the same objects with `vim.res.uiaccess.o` (`vim.rc`
under `-DUIACCESS` embeds `vim.uiaccess.manifest`, `uiAccess="true"`).
Windows starts such an exe only signed by a certificate in the machine's
Trusted Root store and placed under Program Files (`EnableSecureUIAPaths`):
`tools/uiaccess_setup.ps1` does both and restarts vim mouse from there.
`app_has_uiaccess` (`main.c`, TokenUIAccess) tells the code; `update.c`
refuses the unsigned release on that install. Keep both manifests in step
when the manifest changes.

### Build and check

- Local (Windows): `mingw32-make`, or double-click `build.bat` (finds MinGW in
  `C:\MinGW` or `C:\msys64\mingw32`). Tools: `mingw32-make probe check icon
  shutdown-test`.
- Linux cross-compile, the same as CI (needs `gcc-mingw-w64-i686`):
  `make CC=i686-w64-mingw32-gcc WINDRES=i686-w64-mingw32-windres CFLAGS="-Wall -Wextra -O2 -std=gnu11 -Werror"`,
  then the same with `probe check`. Also try `x86_64-w64-mingw32-*`, since CI
  builds both.
- Linux check of the element filters (no Windows needed):
  `tools/ax_sim/run.sh 'google.html?xs=1' '[{"click":"#avatar"},{"wait":1500}]'`
  prints what the scan keeps and drops on Chromium's real tree for that
  page; `OLD=<rev>` diffs against an older `uia.c`. Needs Xvfb, D-Bus,
  at-spi2-core, python3-gi with gir1.2-atspi-2.0, Node with Playwright and
  gcc.
- CI: `build.yml` (every branch except `master`, and PRs: 32 and 64 bit,
  `-Werror`, app and tools, `shutdown-test` built but not run); `release.yml` (`master` and `v*`: 32-bit build,
  zip, rolling `latest` release).
- The only automated test is `tools/shutdown_test.c` (scheduled shutdown;
  `wine64` runs it on Linux). Other behaviour is verified on Windows; the logs
  (`log = 1`, `log = 2` for a scan) and the tools are the debugging aids,
  and `tools/ax_sim` checks the element filters without Windows.

### Conventions seen in the history

- Commit subjects are plain imperative sentences about user-visible
  behaviour ("Keep hints working in Explorer windows"), not code terms. The
  body gives the symptom, the measured cause, the change, and any new
  setting with its default and how to turn it off.
- Every new behaviour gets a setting to turn it off (`prescan`,
  `smooth_scroll`, `bare_keys`, `alt_nav`, `alt_scroll_auto`, `scan_refresh`,
  `keep_labels`, `scan_settle = 0`, `scan_focusable`, `scroll_end_tap_ms = 0`,
  `shortcut_method = type`, `key_grid = none`).
- README, help window, default `vim.ini` and the `common.h` comments change
  in the same commit as the behaviour.
- Comments explain *why* (often with the app that forced the rule); match
  that density. Newer Windows constants are guarded with `#ifndef` at the top
  of the file that uses them.
