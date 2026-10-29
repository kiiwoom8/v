# vim mouse

vim mouse is a tiny Windows tray program that lets you drive the mouse pointer
entirely from the keyboard. Tap CapsLock for mouse mode and move with `hjkl`,
or press Alt+F to open click hints: labels appear on the clickable elements of
the screen and one or two keypresses move the pointer and click one. When an
app does not expose its elements, a labelled grid that can click anywhere
opens instead. It runs
in the background with no console window, is configured by a plain-text
`vim.ini` next to the executable (or in `%APPDATA%\vim mouse` when that folder
is read-only), and reloads that file as soon as you save it.

## Quick start

1. Download [vim.exe](https://github.com/kiiwoom8/v/releases/latest/download/vim.exe)
   from the latest release (or
   [vim-mouse.zip](https://github.com/kiiwoom8/v/releases/latest/download/vim-mouse.zip)
   with `build.bat` and this README). To build from source instead, install
   MinGW (32-bit) and double-click `build.bat` (or run `mingw32-make`).
2. Run `vim.exe`. Look for the mouse icon in the notification area (it may be
   hidden under the "^" arrow).
3. The first run creates `vim.ini` next to `vim.exe`, or in
   `%APPDATA%\vim mouse` when that folder is read-only (for example under
   `C:\Program Files`). Edit it, save, and the changes apply immediately.

## Keys

### Global

| Key | Action |
|---|---|
| `Alt+F` | Open click hints on the monitor under the cursor; press again to rescan |
| `Alt+j` / `Alt+k` | Scroll down / up, anywhere (tap = one notch, hold = continuous) |
| `Alt+h` / `Alt+l` | Go back / forward (sent as `Alt+Left` / `Alt+Right`: browsers, Explorer, VS Code and most apps) |
| `Alt+,` / `Alt+.` | Scroll left / right |
| `Alt+u` / `Alt+d` | Fast scroll up / down (tap = 5 notches, hold = fast continuous) |
| `Alt+u u` / `Alt+d d` | Scroll to the top / bottom (two quick taps) |
| `Alt` + abbreviation | Type a [text shortcut](#text-shortcuts) (release Alt to type it) |
| `CapsLock` tap | Toggle mouse mode |
| `CapsLock` hold + keys | Mouse mode only while held (release returns to normal) |
| `Shift+CapsLock` | Real CapsLock (passes through) |
| `Ctrl+Alt+Q` | Quit |
| `Ctrl+Alt+L` | Lock the keyboard: every key is ignored until `Ctrl+Alt+L` is pressed again |
| pause hotkey (unbound by default) | Pause / resume everything |

CapsLock no longer changes the CapsLock state, so no more accidental
UPPERCASE text. Hotkeys are configurable in `vim.ini`.

**Without Alt.** When the keyboard focus is not in a text field (a web page
you are reading, a list, the desktop), the Alt keys above also work without
Alt: `f` opens the hints, `j` / `k` scroll, `h` / `l` go back / forward,
`u` / `d` scroll fast (`u u` / `d d` to the top / bottom) and `,` / `.` scroll
sideways. In a text field (an address bar, a search box, a chat box, a
document, a terminal) those letters type as usual and the keys need Alt as
before. vim mouse asks UI Automation where the focus is after every focus
change; while it is not sure (right after a click, in a game or a remote
desktop, with a menu open) the letters type as usual. Text shortcuts always
need Alt. `bare_keys = 0` turns this off.

While the keyboard is locked (for cleaning it, or with a cat around) the tray
icon turns grey and a balloon says how to unlock; the tray menu's **Lock
keyboard** item also unlocks it with the mouse. Windows still handles
`Ctrl+Alt+Del` itself, and the mouse keeps working.

Alt scrolling uses the mouse-mode keys (`key_left`, `key_down`, `key_up`,
`key_right`, `key_scroll_up`, `key_scroll_down`, `key_scroll_left`,
`key_scroll_right`), so remapping those remaps it too. It works with or
without mouse mode; `alt_scroll = 0` turns it off and lets those Alt
combinations reach the app again. `alt_nav = 0` makes `Alt+h` / `Alt+l` scroll
left / right again instead of going back / forward.

Alt scrolling finds what to scroll by itself: when the pointer is not over
something that scrolls that way, it scrolls the scrollable area nearest to the
pointer in that window (a web page's side panel, a list, a document), found
through UI Automation, without moving the pointer. Within the window it only
picks an area in line with the pointer (a list below its toolbar) or right
beside it; over another panel the wheel stays with the pointer, since some
apps (Teams) do not report everything that scrolls. Over the desktop or the
taskbar it uses the active window. `alt_scroll_auto = 0` always scrolls what
is under the pointer, like a mouse wheel.

### Mouse mode

While mouse mode is on, a small `MOUSE` pill follows the cursor (`DRAG` while
the left button is held). Unbound typing keys are swallowed; Enter, Tab,
Backspace, Delete, arrows, Home/End/PgUp/PgDn and F-keys pass through, and any
key with Ctrl, Alt or Win passes through (so Ctrl+C still works). Shift is not
a pass-through modifier.

| Key | Action |
|---|---|
| `h j k l` | Move left / down / up / right, accelerating (hold two for diagonals) |
| `Shift` + `hjkl` | Slow, precise movement |
| `a` | Left button: tap = click, tap twice = double click, hold = drag |
| `s` | Right click |
| `m` | Middle click |
| `v` | Press and hold the left button (drag/select); `v` or `a` again releases |
| `u` / `d` | Scroll up / down (tap = one notch, hold = continuous, accelerating) |
| `,` / `.` | Scroll left / right |
| `f` | Click hints (mouse mode stays on afterwards) |
| `/` or `?` | Help window |
| `i`, `Esc`, `CapsLock` tap | Leave mouse mode |

### Hint mode (click hints)

A transparent, click-through overlay opens on the monitor under the cursor.
By default it finds the clickable elements of the visible windows through UI
Automation and labels each one. Type the label shown on the element to click
it; labels are short (usually one letter) and never a prefix of another. The
status bar says "Finding clickable elements" while the scan runs. While the
hints stay open the monitor is scanned again every `scan_refresh` ms (default
500), so elements that appear or move get labels by themselves; a label you
have already read never clicks something else (when labels run out one may
grow a letter, and a label whose element went away is never reused). The
hotkey again still forces a full rescan, and a scan that finds nothing keeps
trying for up to 3 s before the grid. Hints work
on open menus and flyouts too: while a menu is open vim mouse holds back the
hotkey's Alt, so the menu stays open.

| Key | Action |
|---|---|
| label | Click the element showing that label |
| `Shift` + label | Right click instead of left click |
| `Ctrl` + label | Double click |
| `Enter` / `Space` | Act on / move to the only matching element |
| `Backspace` | Undo one typed character |
| `Tab` / `Shift+Tab` | Move to the next / previous monitor and rescan |
| hotkey again | Rescan the monitor now (labels also update by themselves) |
| `Esc` | Cancel |

If an app does not expose UI Automation (some games, remote desktops,
custom-drawn apps) the scan finds nothing and vim mouse falls back to the grid
for that open; the hotkey then tries the scan again.

#### Grid

The grid labels every cell of the monitor. Each cell has a two-letter label:
**row letter + column letter**, like spreadsheet coordinates, so column letters
line up vertically.

| Key | Action |
|---|---|
| first letter | Pick the row (only that row's labels stay) |
| second letter | Pick the column: the cursor jumps to that cell and its subgrid opens |
| subgrid key | Click at that subcell's centre |
| `Enter` | Click at the cell centre |
| `Space` | Move the cursor to the cell centre and close (no click) |
| `Shift` + final key | Right click instead of left click |
| `Ctrl` + final key | Double click |
| `Backspace` | Undo one step |
| `Tab` / `Shift+Tab` | Move the overlay to the next / previous monitor |
| `Esc` | Cancel |

The subgrid uses the keys laid out like the keyboard (`q w e r` / `a s d f` /
`z x c v` by default). With `subgrid = 0`, the click happens right after the
second letter. If a drag is in progress, the final action only moves the
pointer (a drop target), so `v` ... hints ... `v` drags across the screen.

**Example:** a label reading `dm` means row `d`, column `m`. Type `d`, then
`m`, then `s` to click the `s` subcell inside that cell. Typing ahead works, so
`dms` at speed is fine. Keys typed while the element scan is still running are
replayed once it finishes. The status bar at the bottom explains the current
step.

## Text shortcuts

Text shortcuts type a saved text from a short abbreviation. Hold `Alt`, type
the abbreviation, and release `Alt`: the text is pasted into the focused
window. For example, with `reor` set to `다시 주문하시겠습니까?`, holding Alt
and typing `reor` types `다시 주문하시겠습니까?` when Alt is released. Any
text works, including Korean and other non-Latin scripts, and several lines.

- Abbreviations are 1-31 letters (`a`-`z`, not case sensitive) or digits.
- While Alt is held, `Backspace` removes the last typed letter and `Esc` (or
  any key that is not a letter or digit) cancels; nothing is typed then.
- A shortcut only starts when its first letter is not already an Alt key of
  vim mouse: `Alt+F` (hints) and the Alt scroll keys (`h j k l u d`) keep
  their meaning, so abbreviations should not start with those letters. Letters
  after the first can be anything.
- Alt plus a letter that starts no abbreviation reaches the app as usual (menu
  accelerators keep working).

Manage them from the tray menu: **Text shortcuts...** opens a window that lists
them. Select one to edit it, fill in the abbreviation and the text, then press
**Add / Update** (Enter) to save it, **Delete** to remove the selected one, or
**New** to clear the fields. Changes apply at once.

**Export...** saves all shortcuts to a text file, and **Import...** adds the
shortcuts from such a file, e.g. to move them to another PC or share them with
a team. The file uses the same `abbreviation = text` lines as `vim.ini` and can
be edited in any text editor; a whole `vim.ini` can be imported too (only its
`[shortcuts]` section is read). When imported abbreviations already exist,
you choose whether the imported ones replace them.

They are stored in the `[shortcuts]` section of `vim.ini`, one per line, so
they can also be edited by hand:

```ini
[shortcuts]
reor = 다시 주문하시겠습니까?
addr = 123 Main Street\nSpringfield
```

In the text, `\n` is a new line, `\t` is a tab, `\s` is a space (needed only
for a leading or trailing space) and `\\` is a backslash. The editor window
rewrites only the `[shortcuts]` section and keeps the rest of the file as it is.

The text goes in through the clipboard (`Ctrl+V`), which works in browser
address bars, web editors and with the Korean IME. What was on the clipboard
is put back right after, and the shortcut's text is kept out of the clipboard
history. For an app that blocks pasting, set `shortcut_method = type` in
`[general]` to type the text key by key instead.

## Settings

`vim.ini` lives next to `vim.exe`, or in `%APPDATA%\vim mouse` when the
executable's folder is read-only. It is watched either way: saving it reloads
the settings automatically (a balloon confirms, or shows any problems). The file
created on first run documents every option with a comment. The most useful
ones:

- `hint_hotkey`, `quit_hotkey`, `pause_hotkey`, `lock_hotkey`, `mouse_mode_key`
- `speed`, `max_speed`, `accel_time`, `accel_curve`, `slow_speed` (movement,
  in screen pixels per second; Windows' pointer speed and "Enhance pointer
  precision" settings do not change it)
- `scroll_speed`, `scroll_max_speed`, `smooth_scroll` (1 = small steps once
  per display frame, default; 0 = whole notches only)
- `alt_scroll` (1 = Alt + `jk` scroll, Alt + `hl` back / forward and Alt +
  `u`/`d` fast scroll anywhere), `alt_scroll_auto` (1 = scroll the nearest
  scrollable area in line with the pointer when it is not over one),
  `alt_nav` (1 = Alt + `h`/`l` go back / forward and Alt + `,`/`.` scroll
  sideways, default; 0 = Alt + `h`/`l` scroll sideways),
  `bare_keys` (1 = outside text fields the Alt keys work without Alt,
  default; 0 = always Alt), `fast_scroll_lines` (notches per Alt + `u`/`d` tap, 1-50, default
  5), `fast_scroll_speed`, `fast_scroll_max_speed` (while held),
  `scroll_end_tap_ms` (two Alt + `u` / Alt + `d` taps within this many ms
  scroll to the top / bottom, 0-1000, default 300; 0 = off)
- `hint_mode` (`elements` or `grid`: what the hint hotkey opens first),
  `scan_scope` (`monitor` or `window`), `scan_timeout` (100-5000 ms, default
  400: how long to wait for every window's elements before showing the ones
  found so far. The active window's elements are always waited for, up to
  3 s; with nothing found by then, the hints fall back to the grid),
  `scan_refresh` (0-5000 ms, default 500: while the hints are open, rescan
  this often so elements that appear or move get labels by themselves;
  existing labels never change meaning. 0 = off),
  `prescan` (1 = start finding elements and scrollable areas as soon as Alt
  goes down, so labels and Alt scrolling respond sooner; 0 = wait for the
  whole key combination)
- `cell_size`, `subgrid`, `subgrid_keys`, `font_size`, `alphabet`, `opacity`
- `label_bg`, `label_fg`, `label_border`, `match_fg` colours (`#RRGGBB`)
- `key_left`, `key_down`, `key_up`, `key_right`, `key_left_button`,
  `key_right_click`, `key_middle_click`, `key_drag_toggle`, `key_scroll_up`,
  `key_scroll_down`, `key_scroll_left`, `key_scroll_right`, `key_hints`,
  `key_exit`, `key_help` remaps
- `indicator`, `log`
- `shortcut_method` (`paste` or `type`: how text shortcuts insert their text)
- `[shortcuts]`: text shortcuts, see [Text shortcuts](#text-shortcuts)

## Tray menu

Left- or right-click the tray icon:

- Show click hints (with the configured hotkey)
- Mouse mode (checked when on)
- Pause (checked when paused)
- Lock keyboard (checked while locked; click to unlock)
- Keyboard shortcuts...
- Text shortcuts... (add, change, delete, import or export
  [text shortcuts](#text-shortcuts))
- Edit settings... (opens the settings file in use, falling back to Notepad)
- Reload settings
- Start with Windows (checked when enabled)
- Quit

## Troubleshooting

- **A hotkey clashes with another program.** Change it in `vim.ini` and save;
  it reloads immediately. Use `none` to unbind a hotkey.
- **A window cannot be clicked.** Programs running as administrator cannot be
  controlled from a normal process (Windows UIPI). Run vim mouse as
  administrator too if you need to drive those windows.
- **Hints on an open menu.** They work: while a menu or flyout is open vim
  mouse holds back the hotkey's Alt, so the menu stays open.
- **An app shows no labels.** It does not expose UI Automation (some games,
  remote desktops, custom-drawn apps), so the hints fall back to the grid.
  Set `hint_mode = grid` to open the grid every time.
  `mingw32-make probe` builds `tools\uia_probe.exe`, which lists what the scan
  finds on the monitor under the cursor (`-v` for every element).
- **Few labels the first time in Chrome, Edge or an Electron app.** These build
  their accessibility tree only when first asked. vim mouse asks each window
  you switch to in the background, so this only happens in a window that was
  already active when vim mouse started; press `Esc` and open the hints again.
- **A web page element gets no label.** With Node and Chrome installed,
  `mingw32-make check` builds `tools\hint_check.exe` and
  `node tools\hint_check.mjs [url ...]` opens each page in a separate Chrome
  profile and lists the clickable elements the scan misses, with the reason.
- **An app's Alt+H/J/K/L/U/D/,/. shortcut no longer works.** vim mouse uses
  those for scrolling and back / forward; set `alt_scroll = 0` in `vim.ini` to
  give them back.
- **A letter such as `j` or `f` does not reach an app.** Outside text fields
  vim mouse takes `f h j k l u d , .` without Alt (YouTube's `j k l f`, for
  example). Click into a text field to type them, or set `bare_keys = 0`.
- **A letter scrolls or goes back instead of typing.** vim mouse did not
  recognise that text field. With `log = 1`, `vim.log` shows what it decided
  after each focus change ("focus: text field" or "focus: no text field").
  `bare_keys = 0` turns the Alt-less keys off.
- **A text shortcut types nothing.** Its first letter may be an Alt key of vim
  mouse (`f h j k l u d` by default); the editor warns about this when you
  save. Programs running as administrator cannot receive typed text either
  (see above).
- **I need the real CapsLock.** vim mouse intercepts CapsLock; use
  `Shift+CapsLock` when you really want the CapsLock state.
- **Start automatically at login.** Tick "Start with Windows" in the tray menu
  (it writes a value under `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`).
- **Something is wrong.** Set `log = 1` in `vim.ini` and read `vim.log` in the
  same folder as the settings file.

## Building

Use MinGW.org GCC 6.3 (32-bit, `C:\MinGW\bin\gcc`). From the source directory:

```
mingw32-make
```

or double-click `build.bat`, which finds MinGW in `C:\MinGW` or `C:\msys64\mingw32`
and falls back to calling `gcc` directly. Every push to `master` rebuilds
`vim.exe` on GitHub Actions and publishes it to the
[latest release](https://github.com/kiiwoom8/v/releases/latest); pushing a `v*`
tag publishes a versioned release.

The build uses `-std=gnu11` and `-mwindows`. The version lives in `version.h`;
`vim.rc` embeds the icon (`vim.ico`), the manifest (`vim.manifest`, which brings
in Common Controls v6, per-monitor DPI and the supported Windows versions) and
the version info. Regenerate the icon with `mingw32-make icon` after changing
its drawing code. A GitHub Actions build check (`build.yml`) compiles 32-bit and
64-bit with `-Werror` on every branch except `master` and on pull requests.

Source layout:

- `main.c` - entry point, single instance, DPI awareness, main window, message
  routing, settings watcher, logging
- `config.c` - settings file defaults/parsing, key-name formatting, text
  shortcut table
- `input.c` - low-level keyboard hook, mouse mode, movement, Alt scrolling,
  text shortcut typing, sleep/lock handling
- `hint.c` - click-hint overlay
- `tray.c` - tray icon and menu, help window, start with Windows
- `snippet.c` - text shortcut editor window
- `common.h` - shared contract between modules
- `version.h` - version numbers and strings, shared by the code and `vim.rc`
- `vim.rc`, `vim.manifest`, `vim.ico` - icon, manifest and version resources
