# vim mouse: redesign spec

Windows-only C program: drive the mouse from the keyboard. Build: MinGW.org
GCC 6.3 (32-bit, `C:\MinGW\bin\gcc`), `-std=gnu11` (NOT c11: strict ANSI hides
`_wcsicmp`, `_snwprintf` etc. in these headers), `-mwindows`, Unicode (W) APIs
only. Headers are old: guard any newer constant with `#ifndef` and load newer
APIs (GetDpiForMonitor, SetProcessDpiAwarenessContext, ...) with
GetProcAddress. No console exists at runtime: never rely on stdin/stdout/stderr;
use `app_log` for diagnostics.

`common.h` is the contract between modules. Do not change it; if something is
truly missing, declare a `static` helper in your own file instead.

The old single-file program is `vim.c` (kept until integration, for reference:
movement physics, sleep/lock handling). It will be deleted.

## 1. User-facing behaviour

### Global
| Key | Action |
|---|---|
| `Alt+F` (hint_hotkey) | open click hints on the monitor under the cursor; again = close |
| `CapsLock` tap (mouse_mode_key) | toggle mouse mode |
| `CapsLock` hold + keys | mouse mode only while held (released = back to normal) |
| `Shift+CapsLock` | real CapsLock (passes through) |
| `Ctrl+Alt+Q` (quit_hotkey) | quit |
| pause_hotkey (unbound by default) | pause/resume everything |

CapsLock no longer toggles the CapsLock state (no more UPPERCASE leakage).

### Mouse mode
While active, a small pill near the cursor reads `MOUSE` (or `DRAG` while the
left button is held). Typing keys that are not bound are swallowed so nothing
types by accident; Enter, Tab, Backspace, Delete, arrows, Home/End/PgUp/PgDn,
F-keys pass through. Any key combined with Ctrl, Alt or Win passes through
(Ctrl+C etc. keep working). Shift is not a pass-through modifier.

| Key | Action |
|---|---|
| `h j k l` | move left/down/up/right, accelerating (diagonals by holding two) |
| `Shift` + `hjkl` | slow precise movement (slow_speed, no acceleration) |
| `a` | left button: tap = click, tap twice = double click, hold = drag |
| `s` | right click |
| `m` | middle click |
| `v` | press left button and keep it held (drag/select); `v` or `a` again releases |
| `u` / `d` | scroll up / down (tap = one notch, hold = continuous, accelerating) |
| `,` / `.` | scroll left / right |
| `f` | click hints (mouse mode stays on afterwards) |
| `/` or `?` | help window |
| `i`, `Esc`, `CapsLock` tap | leave mouse mode |

### Hint mode (click hints)
- A transparent overlay on the monitor under the cursor. The screen stays fully
  visible; only small label boxes are drawn. The overlay never takes focus
  (the app under it stays active) and is click-through. Keys reach it through
  the keyboard hook's capture mode, never through window focus.
- The monitor is split into a grid of `cols x rows` cells. Every cell shows a
  2-letter label: **row letter + column letter** (`alphabet[r]`, `alphabet[c]`),
  like spreadsheet coordinates, so column letters line up vertically.
- Type the row letter: only that row's labels stay, the typed letter dimmed
  (`match_fg`). Type the column letter: the cursor jumps to that cell's centre
  and the cell is split into the **subgrid** (default 4x3, keys laid out like
  the keyboard: `q w e r` / `a s d f` / `z x c v`), each subcell labelled.
- Subgrid stage: subgrid key = act at that subcell centre; `Enter` = act at the
  cell centre; `Space` = just move the cursor to the cell centre and close.
- Action = left click. Holding `Shift` on the final key = right click; `Ctrl` =
  double click. If a drag is in progress (`input_left_button_held()`), the
  action is always "move only" (drop target), so `v`, hints, `v` drags across
  the screen.
- With `subgrid = 0` the action happens right after the 2nd letter, at the
  cell centre.
- Typing ahead works (`dms` typed fast = row d, col m, subcell s).
- A key that matches nothing is ignored (input is kept) and the status bar
  flashes red briefly. `Backspace` undoes one step. `Esc` closes.
  `Tab` / `Shift+Tab` moves the overlay to the next / previous monitor.
- A status bar at the bottom centre of the monitor explains the current step,
  e.g. `Type a label · Esc cancel · Tab next screen`, then
  `d_ · Backspace undo`, then
  `dm · subgrid key: click there · Enter: centre · Space: move only · Shift: right · Ctrl: double`.
  When a drag is held it says `Drop target: ...`.

### Tray and help
- Tray icon, generated in code (no resource file): normal, mouse mode
  (highlighted colour), paused (greyed). Tooltip shows the main keys, plus
  "(paused)" when paused. Re-added when Explorer restarts (`TaskbarCreated`).
- Left or right click opens the menu:
  `Show click hints  (Alt+F)` / `Mouse mode  (CapsLock)` [checked when on] /
  `Pause` [checked when paused] / separator / `Keyboard shortcuts...` /
  `Edit settings...` (opens vim.ini with ShellExecute "open"; fall back to
  notepad.exe) / `Reload settings` / `Start with Windows` [checked; HKCU Run
  value named APP_NAME = quoted exe path] / separator / `Quit`.
  Hotkey texts in the menu come from the current config (format_hotkey).
- Help window: dark, topmost, centred on the monitor under the cursor, lists
  every binding from the live config (Global / Mouse mode / Hint mode), and
  the settings file path. Closes on Esc, any key, a click, or losing focus.
  `help_toggle` opens it or closes it if open. Must be DPI-scaled.
- First run (settings file was just created): balloon "vim mouse is running"
  with "Alt+F: click hints · CapsLock: mouse mode · Right-click this icon for
  settings" (use the configured key names).
- Settings warnings after a load: balloon "Settings problem" with the
  warnings text (truncate to fit).
- Settings file is watched; saving it reloads automatically (debounced
  ~300 ms), balloon "Settings reloaded" (or the warnings).
- A second instance: posts WM_APP_ALREADY_RUNNING to the first (FindWindowW
  by MAIN_WND_CLASS) and exits; the first shows balloon "Already running" and
  opens the help window.
- If the keyboard hook cannot be installed: MessageBox explaining it, exit.

## 2. Settings file (config.c)

`vim.ini` next to the exe (exe basename + `.ini`). UTF-8 (optional BOM).
Own parser (not GetPrivateProfileString): `[section]`, `key = value`, lines
starting with `;` or `#` (after whitespace) are comments; no inline comments.
Keys and section names case-insensitive. Unknown section/key and bad values
produce a warning line like `line 12: unknown key "foo" in [mouse]` and keep
the default. Missing file: write the default file with a comment above every
option explaining it (the default file is the documentation), set
`*created`. All option names = the Config field names in common.h, in sections
`[general]`, `[mouse]`, `[hints]`. Value formats:
- hotkeys: `Alt+F`, `Ctrl+Alt+Q`, `Ctrl+Shift+F12`, `none` (modifier names
  Ctrl/Control, Alt, Shift, Win, case-insensitive)
- keys: a single printable char (`h`, `,`, `.`, `/`, `[`, `'`, `-`, `=`,
  `` ` ``, `\`) or a name: `CapsLock Space Enter Tab Esc Backspace Delete
  Insert Home End PgUp PgDn Left Right Up Down F1..F24 ScrollLock Pause
  Apps Comma Period Slash Semicolon Quote LBracket RBracket Backslash Minus
  Equals Backtick Hash none`. `;` and `#` must be written by name. Characters
  map to vks with VkKeyScanW (layout aware); letters/digits map directly.
- `indicator = cursor | corner | off`
- numbers: plain decimal; booleans `0/1/true/false/yes/no/on/off`
- colours: `#RRGGBB`
- `alphabet`: 2-60 unique chars, letters/digits only, lowercased
- `subgrid_keys`: rows separated by `/`, all rows the same length, unique
  chars, at least 1x1 and at most 8x8
Ranges are clamped (opacity 30-255, cell_size 24-400, font_size 0 or 6-72,
speeds > 0). `format_key`/`format_hotkey` produce the names users would type
(`h`, `,`, `CapsLock`, `Alt+F`).

## 3. Input (input.c)

One "input thread" installs the WH_KEYBOARD_LL hook, owns a hidden window for
WM_POWERBROADCAST / WTS session notifications (as in vim.c), and the mode
indicator window, and pumps messages. The hook callback must be fast: only
flag updates, PostMessage, SetEvent, short SendInput. Never block.

Hook rules, in order, for each event:
1. `LLKHF_INJECTED` events: pass through untouched.
2. Track per-vk physical down state (filters autorepeat) and an owner per vk
   recorded at keydown (pass / swallowed-by-X). A keyup goes to whoever got
   the keydown: if the keydown was swallowed the keyup is swallowed, and a
   mouse-mode key's release is always processed by mouse mode (release button,
   stop moving) even if capture started meanwhile.
3. Modifier keys (Shift/Ctrl/Alt/Win, left and right) always pass through.
4. Hotkeys (quit, pause, hint) match on non-repeat keydown with exact
   modifiers (from GetAsyncKeyState). Swallow. If Alt or Win is held, inject
   a dummy key (vk 0xE8 down+up) first so releasing Alt/Win does not open the
   app menu / Start menu. Hint hotkey: if capture is off, turn capture on and
   post WM_APP_HINT_OPEN; if on, post WM_APP_HINT_CLOSE.
5. Paused: everything else passes.
6. Capture on: swallow, and on keydown post WM_APP_HINT_KEY(vk, mods). (Keys
   with no matching handler there are simply ignored by hint.c.)
7. mouse_mode_key without Ctrl/Alt/Win/Shift: tap/hold logic. On keydown
   (non-repeat): if mode off -> turn on, remember "entered by this press"; if
   on -> remember "exit on release". Any mouse-mode key pressed while it is
   physically held marks "used while held". On keyup: exit if "exit on
   release", or if "entered by this press" and "used while held". Swallow
   both. With Shift held it passes through (real CapsLock).
8. Mouse mode on: Ctrl/Alt/Win held -> pass. Bound key -> handle, swallow.
   Esc -> leave mouse mode, swallow. Unbound typing key (letters, digits,
   Space, OEM punctuation) -> swallow. Anything else -> pass.
9. Otherwise pass.

Mouse mode details:
- Leaving mouse mode (any way, also pausing, lock, sleep) releases any held
  button and stops movement/scroll.
- Movement thread: blocks on an event while nothing is held (zero idle CPU).
  While moving/scrolling, timeBeginPeriod(1) so Sleep(1) is really ~1 ms
  (timeEndPeriod when idle). Same acceleration physics as vim.c
  (speed -> max_speed over accel_time with accel_curve, smoothing), but read
  from g_cfg. Shift held (GetAsyncKeyState) -> constant slow_speed. Large time
  gap -> reset (sleep/resume).
- Scroll: keydown emits one notch (120) immediately. Held > 250 ms ->
  continuous from scroll_speed ramping to scroll_max_speed over accel_time,
  accumulating fractional delta and sending MOUSEEVENTF_WHEEL / HWHEEL in
  chunks of at least 40. Up and right are positive.
- Right/middle click on non-repeat keydown only (holding `s` must not spam).
- State changes (mouse mode on/off, paused, drag on/off) post
  WM_APP_STATE_CHANGED to g_main_wnd.
- Indicator (g_cfg.indicator): small click-through topmost layered window
  (WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|
  WS_EX_TOPMOST), rounded pill, bold text `MOUSE` (blue #2D7FF9, white text)
  or `DRAG` (orange #F2994A, dark text). `cursor`: follows the cursor at
  (+18, +22) logical px, DPI scaled, via a ~15 ms timer that runs only while
  mouse mode is on, kept inside the monitor. `corner`: top-left of the
  monitor under the cursor. Hidden when mouse mode is off.
- `mouse_click_at`: SetCursorPos, then one SendInput with down/up pairs
  (twice for count 2). `mouse_move_to`: SetCursorPos.
- input_set_mouse_mode/paused can be called from the tray (other thread):
  apply via PostThreadMessage or interlocked flags; keep it race-free enough
  that buttons never get stuck down.

## 4. Hint overlay (hint.c)

- One overlay window, created once in hint_init and reused (hidden when
  closed). Styles: WS_POPUP; ex WS_EX_LAYERED|WS_EX_TRANSPARENT|
  WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE. Background is a colour key
  (RGB(255,0,255), fully transparent) plus `LWA_ALPHA` = g_cfg.opacity. Show
  with SetWindowPos(HWND_TOPMOST, ..., SWP_NOACTIVATE|SWP_SHOWWINDOW). Never
  call SetForegroundWindow/SetFocus.
- Geometry per monitor (full monitor rect, not work area): dpi =
  dpi_for_monitor; cell = cell_size*dpi/96; N = alphabet length;
  cols = clamp(round(w/cell), 1, N); rows = clamp(round(h/cell), 1, N);
  distribute remainder pixels like vim.c. Subgrid dims from subgrid_keys.
- Drawing (double-buffered in WM_PAINT): fill colour key; each label is a
  box (label_bg fill, 1 px label_border, small radius via RoundRect) with
  bold Segoe UI text, centred in its cell. Auto font: about 13 px at 96 dpi,
  scaled, shrunk if the box would not fit the cell. Typed prefix chars in
  match_fg, rest in label_fg. Selected cell (subgrid stage): 2 px outline in
  label_border plus subgrid labels (smaller font) at subcell centres. Avoid
  drawing pure RGB(255,0,255) anywhere. Cache fonts/brushes per open.
- Status bar: rounded dark box (#1E1E1E, white text, ~12 px at 96 dpi scaled)
  at bottom centre, 24 logical px above the monitor bottom. Red (#C0392B) for
  ~180 ms on an invalid key (SetTimer).
- Key handling: vk -> char: 'A'-'Z' -> lowercase, '0'-'9', numpad digits,
  others via MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) lowercased. Esc, Backspace,
  Enter, Space, Tab handled as above. Shift/Ctrl from the mods argument.
- Acting: hide the overlay and call input_set_key_capture(FALSE) first, then
  mouse_click_at / mouse_move_to. hint_close also turns capture off. hint_open
  turns it on (harmless if already on); if opening fails, turn it off.

## 5. App (main.c) and tray (tray.c)

- main.c: `int main(void)` (built with -mwindows). Single instance via
  INSTANCE_MUTEX (second instance: post WM_APP_ALREADY_RUNNING, exit).
  DPI awareness: SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2 = (HANDLE)-4)
  -> SetProcessDpiAwareness(2) -> SetProcessDPIAware, all via GetProcAddress.
  Load config (config_load into g_cfg). Create the hidden main window
  (MAIN_WND_CLASS, message-only is NOT ok because tray + TaskbarCreated
  need a normal hidden top-level window). hint_init, tray_init, input_start
  (failure -> MessageBox + exit). Balloons as in section 1. Route all
  WM_APP_* messages. Watch the settings file (FindFirstChangeNotificationW on
  its directory in a thread, compare last-write time, post
  WM_APP_RELOAD_CONFIG debounced). app_reload_config: load into a temp
  Config, copy to g_cfg, input_config_changed(), tray_update(), balloon.
  On quit: hint_shutdown, input_stop, tray_shutdown, exit message loop.
- dpi_for_monitor: GetDpiForMonitor (shcore.dll, MDT_EFFECTIVE_DPI = 0) if
  available, else GetDeviceCaps(LOGPIXELSX) of the screen DC.
- app_log: `[HH:MM:SS] message` via OutputDebugStringW; if g_cfg.log, also
  append UTF-8 to vim.log next to the exe.
