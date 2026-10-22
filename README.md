# vim mouse

vim mouse is a tiny Windows tray program that lets you drive the mouse pointer
entirely from the keyboard. Tap CapsLock for mouse mode and move with `hjkl`,
or press Alt+F to open click hints: a grid of letter labels appears over the
screen and a couple of keypresses move the pointer and click anywhere. It runs
in the background with no console window, is configured by a plain-text
`vim.ini` next to the executable, and reloads that file as soon as you save it.

## Quick start

1. Install MinGW (32-bit) and make sure `mingw32-make` is on `PATH`.
2. Build:

   ```
   mingw32-make
   ```

3. Run `vim.exe`. Look for the mouse icon in the notification area (it may be
   hidden under the "^" arrow).
4. The first run creates `vim.ini` next to `vim.exe`. Edit it, save, and the
   changes apply immediately.

## Keys

### Global

| Key | Action |
|---|---|
| `Alt+F` | Open click hints on the monitor under the cursor; press again to close |
| `CapsLock` tap | Toggle mouse mode |
| `CapsLock` hold + keys | Mouse mode only while held (release returns to normal) |
| `Shift+CapsLock` | Real CapsLock (passes through) |
| `Ctrl+Alt+Q` | Quit |
| pause hotkey (unbound by default) | Pause / resume everything |

CapsLock no longer changes the CapsLock state, so no more accidental
UPPERCASE text. Hotkeys are configurable in `vim.ini`.

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

A transparent, click-through overlay labels the monitor under the cursor with
a grid. Every cell has a two-letter label: **row letter + column letter**, like
spreadsheet coordinates, so column letters line up vertically.

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
`dms` at speed is fine. The status bar at the bottom explains the current step.

## Settings

`vim.ini` lives next to `vim.exe` and is watched: saving it reloads the
settings automatically (a balloon confirms, or shows any problems). The file
created on first run documents every option with a comment. The most useful
ones:

- `hint_hotkey`, `quit_hotkey`, `pause_hotkey`, `mouse_mode_key`
- `speed`, `max_speed`, `accel_time`, `accel_curve`, `slow_speed` (movement)
- `scroll_speed`, `scroll_max_speed`
- `cell_size`, `subgrid`, `subgrid_keys`, `font_size`, `alphabet`, `opacity`
- `label_bg`, `label_fg`, `label_border`, `match_fg` colours (`#RRGGBB`)
- `key_left`, `key_down`, `key_up`, `key_right`, `key_left_button`,
  `key_right_click`, `key_middle_click`, `key_drag_toggle`, `key_scroll_up`,
  `key_scroll_down`, `key_scroll_left`, `key_scroll_right`, `key_hints`,
  `key_exit`, `key_help` remaps
- `indicator`, `log`

## Tray menu

Left- or right-click the tray icon:

- Show click hints (with the configured hotkey)
- Mouse mode (checked when on)
- Pause (checked when paused)
- Keyboard shortcuts...
- Edit settings... (opens `vim.ini`, falling back to Notepad)
- Reload settings
- Start with Windows (checked when enabled)
- Quit

## Troubleshooting

- **A hotkey clashes with another program.** Change it in `vim.ini` and save;
  it reloads immediately. Use `none` to unbind a hotkey.
- **A window cannot be clicked.** Programs running as administrator cannot be
  controlled from a normal process (Windows UIPI). Run vim mouse as
  administrator too if you need to drive those windows.
- **I need the real CapsLock.** vim mouse intercepts CapsLock; use
  `Shift+CapsLock` when you really want the CapsLock state.
- **Start automatically at login.** Tick "Start with Windows" in the tray menu
  (it writes a value under `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`).
- **Something is wrong.** Set `log = 1` in `vim.ini` and read `vim.log` next to
  `vim.exe`.

## Building

Use MinGW.org GCC 6.3 (32-bit, `C:\MinGW\bin\gcc`). From the source directory:

```
mingw32-make
```

The build uses `-std=gnu11` and `-mwindows`. Source layout:

- `main.c` - entry point, single instance, DPI awareness, main window, message
  routing, settings watcher, logging
- `config.c` - settings file defaults/parsing and key-name formatting
- `input.c` - low-level keyboard hook, mouse mode, movement, sleep/lock handling
- `hint.c` - click-hint overlay
- `tray.c` - tray icon and menu, help window, start with Windows
- `common.h` - shared contract between modules
