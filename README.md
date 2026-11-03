# vim mouse

vim mouse is a tiny Windows tray program that lets you drive the mouse pointer
entirely from the keyboard. Tap CapsLock for mouse mode and move with `hjkl`,
or press Alt+F to open click hints: labels appear on the clickable elements of
the screen and one or two keypresses move the pointer and click one (or press
`;` for a labelled grid that can click anywhere). It runs
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
| `Alt+g g` / `Alt+Shift+g` | Scroll to the top (two quick taps) / bottom (on press). Both go through UI Automation, exactly and at once; where that cannot scroll the area, the wheel does it (for the bottom once Shift is released, so browsers do not scroll sideways) |
| `0` `0` | Move the pointer to the middle of the active window's screen (two quick taps, outside text fields; `0` still reaches the app) |
| `Alt+0 0` | The same anywhere, text fields included (both taps swallowed) |
| `Alt` + abbreviation | Type a [text shortcut](#text-shortcuts) (release Alt to type it) |
| `CapsLock` tap | Toggle mouse mode |
| `CapsLock` hold + keys | Mouse mode only while held (release returns to normal) |
| `Shift+CapsLock` | Nothing: the CapsLock state stays off (with `capslock_off = 0`, the real CapsLock) |
| `Ctrl+Alt+Q` | Quit |
| `Ctrl+Alt+L` | Lock the keyboard: every key is ignored until `Ctrl+Alt+L` is pressed again |
| `Alt+p p` | Pause / resume vim mouse (two quick taps): while paused every key passes through, except this and `Ctrl+Alt+Q` |

CapsLock no longer changes the CapsLock state, so no more accidental
UPPERCASE text. While vim mouse runs the CapsLock state is kept off
(`capslock_off = 1`, the default): if it is on when vim mouse starts, or
comes on where vim mouse cannot see the key (a window running as
administrator, the lock screen, while paused), vim mouse turns it off again
within a quarter of a second (once you are back in a normal window, or no
longer paused). Hotkeys are configurable in `vim.ini`.

**The pointer stays out of the way.** Outside mouse mode the pointer is
hidden (`hide_pointer`, default 2000), so it does not sit on what you are
reading. Moving or clicking the real mouse shows it again, and it hides
once the mouse has been still for that many milliseconds; leaving mouse
mode or clicking through the hints hides it at once. It shows while vim
mouse is paused, while the session is locked and while the computer
sleeps, and comes back when vim mouse quits. `hide_pointer = 0` never hides
it. (Every program sees the same blank pointer: vim mouse swaps the system
cursors. If vim mouse crashes while they are blank, start it again or sign
out and in.)

**Without Alt.** When the keyboard focus is not in a text field (a web page
you are reading, a list, the desktop), the Alt keys above also work without
Alt: `f` opens the hints, `j` / `k` scroll, `h` / `l` go back / forward,
`u` / `d` scroll fast, `g g` / `Shift+g` scroll to the top / bottom and `,` /
`.` scroll sideways. In a text field (an address bar, a search box, a chat
box, a document, a terminal, an autocomplete field with its suggestions open)
those letters type as usual and the keys need Alt as before. A focused link,
button, checkbox, dropdown or list counts as outside a text field; tooltips,
Chrome's link preview bubble and Chrome's panels (downloads, extensions,
permission prompts) do not count as an open menu, while Chrome's menus do. vim
mouse asks UI Automation where the focus is after every focus change; while it
is not sure (right after a click, in a game or a remote desktop, with a menu
open) the letters type as usual. A key pressed while it is still asking waits
up to 0.1 s for the answer instead of typing at once. A `g` that typed while
it was still asking still counts as the first of `g g`: the second `g` scrolls
to the top once the answer says the focus is outside a text field, and when
the second `g` typed for the same reason the jump still comes once the
answer arrives (within a second, unless another key or a focus change comes
first).
Text shortcuts always need Alt. `bare_keys = 0` turns this off.

**0 twice.** Two quick taps of `0` (within `center_tap_ms`, default
300 ms) put the pointer in the middle of the screen that holds the active
window: in mouse mode, and outside it wherever the keyboard focus is not in
a text field, so typing `00` in a chat box or a document moves nothing.
Both presses reach the app as usual; vim mouse never holds `0` back. It
uses the same focus answer as the keys without Alt, so with `bare_keys = 0`
it works in mouse mode only. With the pointer hidden outside mouse mode it
shows up in the middle and hides again after `hide_pointer` ms. `key_center`
picks another key (`none` turns it off), `center_tap_ms` the tap window.
`Alt+0 0` does the same anywhere, text fields included, and swallows both
presses like the other Alt keys. A second `0` that typed while vim mouse was
still asking where the focus is still makes the jump once the answer says
it is outside a text field (within a second, unless another key comes
first).

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
taskbar it uses the active window. The scroll starts under the pointer the
moment the key goes down, like a wheel, and moves to that area as soon as it
is found (at once in a window scrolled lately; a tap's notch arrives there in
full). `alt_scroll_auto = 0` always scrolls what is under the pointer.

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
| `g g` / `Shift+g` | Scroll to the top (two quick taps) / bottom, through UI Automation where it can scroll the area, else on the wheel |
| `0` `0` | Move the pointer to the middle of the screen (two quick taps) |
| `f` | Click hints (mouse mode stays on afterwards) |
| `/` or `?` | Help window |
| `i`, `Esc`, `CapsLock` tap | Leave mouse mode |

### Hint mode (click hints)

A transparent, click-through overlay opens on the monitor under the cursor.
By default it finds the clickable elements of the visible windows through UI
Automation and labels each one. Type the label shown on the element to click
it; labels are short (usually one letter) and never a prefix of another. The
status bar says "Finding clickable elements" while the scan runs; usually it
does not show at all, because the scan already ran when the screen last
changed (`scan_ahead`, see Settings) and the labels appear at once. While the
hints stay open the monitor is scanned again every `scan_refresh` ms (default
500), so elements that appear get labels by themselves. A label already on
screen stays put and keeps its letters (`keep_labels`, default 1): a label you
have read never moves, changes or disappears under you, and still clicks
where it is drawn. An element that only shifted a few pixels gets no second
label. New elements get labels in free space, as many as fit (the first
labels leave room for that, so a panel or menu that opens while the hints
are up gets labels too); to relabel everything, press the hotkey again. With `keep_labels = 0` labels follow
their elements instead and disappear when an element goes away (when labels
run out one may grow a letter, and a label whose element went away is never
reused). A scan that finds nothing keeps trying for up to 3 s, then closes
the hints. Chrome, Edge and apps built on them tell UI Automation about moved
and new elements up to half a second late, so when vim mouse scrolls or
clicks there, or the browser reports a scroll of its own (mouse wheel,
touchpad, keys), vim mouse asks the browser right then to send its changes
and waits for them: a few dozen milliseconds after a scroll when the
browser does not animate scrolling (Chrome and Edge follow Windows'
animation effects setting), else once its animation is over. Where it does
not animate, the search for the elements starts on the browser's first
answer and the next one only confirms it (`scan_early`). That starts
with the scroll or click itself, not with the hotkey, so a hotkey pressed
after it is over finds the page in and shows the labels at once, and one
pressed sooner waits for the rest of it (`scan_flush`; never longer than
`scan_settle`, default 650 ms, from the scroll or click; after back /
forward vim mouse waits it out, since a loading page reports nothing). The
labels
then sit where things are now, and a panel the click opened has its own. In
a browser the labels show as soon as the browser has answered; other windows
on the monitor get theirs a moment later (`browser_first`). In a browser an
element that only says it can take the keyboard focus gets a label too (a
`div` with `tabindex` whose click handler sits on an ancestor, as React
attaches them; a range slider), unless it is text, a scroll area, lies
outside the page or covers over a quarter of it (`scan_focusable`). A
checkbox or radio button that a page hides behind its own label (Naver
Mail's "select all": the input is one pixel, the label is drawn as the box)
gets the label's box and is clicked there (`scan_hidden_checkbox`). When
two labels would sit on one spot (a Google result's title link and the
result block around it both start at that corner), the smaller element
keeps its corner and the other label is drawn to the right of it, or below
it at the screen's edge (`labels_apart`), so both can be read and typed.
Once a character is typed, the labels that match drop it and show only
what is still to type, in a smaller box, so what is left is easy to read. A menu, dialog or embedded frame drawn over a web page
(Google's account menu, a Zendesk menu or modal) gets its own labels; page
links partly under it get their label and click on the part that shows, and
links entirely under a menu or dialog get none. Chrome sometimes reports
nearly everything inside an open web menu or dialog as off screen (Google's
account menu: all its rows); when such a menu or dialog shows almost
nothing of its own, vim mouse labels what lies inside it anyway. In classic
windows (File Explorer, dialogs) the minimize, maximize and close buttons
get labels but the window icon at the top left does not: finding its system
menu alone took about a third of File Explorer's whole search (`Alt+Space`
opens it; `scan_split = 0` labels it again). Hints work
on open menus and flyouts too: while a menu is open vim mouse holds back the
hotkey's Alt, so the menu stays open.

| Key | Action |
|---|---|
| label | Click the element showing that label (the pointer then goes back to where it was, `click_return`) |
| `Shift` + label | Right click instead of left click |
| `Ctrl` + label | Double click |
| `Ctrl+Shift` + label | Middle click (a link opens in a new browser tab) |
| `Alt` + label | Move the pointer there without clicking (hover something, or scroll that area with `Alt+j` / `Alt+k` next) |
| `Enter` / `Space` | Act on / move to the only matching element |
| `Backspace` | Undo one typed character |
| `;` | Switch to the [grid](#grid), for an element that got no label; `;` again returns to the labels (`key_grid`) |
| `Tab` / `Shift+Tab` | Move to the next / previous monitor and rescan |
| hotkey again | Rescan the monitor now (labels also update by themselves) |
| `Esc` | Cancel |

A right, double or middle click is sent once Shift and Ctrl are released, so
the app sees a plain click (with Ctrl still down, Explorer would only toggle
the selection and a browser would open the link in a new tab).

After a click the pointer goes back to where it was before (`click_return`,
default 1), so it is not left on the link or button you clicked while you
read on; with the pointer hidden outside mouse mode (`hide_pointer`) that
also means nothing shows up there. `Alt` + label and the drop of a drag
still move the pointer, and in mouse mode (hints opened with `f`) the
pointer stays on the element, so `hjkl` carry on from there.
`click_return = 0` leaves the pointer on the element after every click.

If an app does not expose UI Automation (some games, remote desktops,
custom-drawn apps) the scan finds nothing and the hints close again; the grid
never opens by itself, but `;` opens it while the hints are up, even while
they are still searching.

#### Grid

With `hint_mode = grid` the hotkey opens the grid instead of the element
labels; `;` (`key_grid`) switches to it from the labels at any time, and back.
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
| `Ctrl+Shift` + final key | Middle click |
| `Alt` + final key | Move the cursor only |
| `Backspace` | Undo one step |
| `;` | Back to the element labels |
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
  vim mouse: `Alt+F` (hints), `Alt+p` (pause) and the Alt scroll keys
  (`h j k l u d g`) keep their meaning, so abbreviations should not start
  with those letters. Letters
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

## Scheduled shutdown

The tray menu's **Schedule shutdown** turns the computer off later: **In 15
minutes**, **In 30 minutes**, **In 1 hour**, **In 2 hours**, or **At a time or
after...**, where you type a time such as `23:30` (today, or tomorrow once it
has passed) or how long to wait: `90` (minutes), `45m`, `2h` or `1h30`. A
balloon confirms it, and until then the menu item reads "Shutdown at 23:30"
and the tray icon's tooltip says so too. Choosing again replaces it; **Cancel
shutdown at ...** in the same menu cancels it.

To shut down every day, set `shutdown_at = 23:30` in the `[general]` section
of `vim.ini`. Cancelling from the tray menu then skips that day only, and a
shutdown chosen in the tray menu that comes later than the next day's
replaces it (cancelling that one gives the day back).

A minute before (`shutdown_warning`), a countdown window opens on top:

- **Cancel** (or `Esc`, `Enter`, or closing the window) keeps the computer on.
- **10 more minutes** postpones it.
- **Shut down now** shuts down at once. It can only be clicked, and only from
  a second after the window opens, so keys typed or a click made into the
  window by surprise never shut the computer down.

Then Windows shuts down and turns the computer off (a full shutdown, like
`shutdown /s`). Apps that do not respond are closed; an app with unsaved work
may ask to save it, which holds the shutdown up until someone answers. Set
`shutdown_force = 1` to close such apps anyway (their unsaved work is lost).

- If the computer was asleep at that time and wakes more than 5 minutes
  later, the shutdown is skipped and a balloon says so, instead of turning the
  computer off the moment someone wakes it. Woken sooner, it counts down in
  full first.
- A shutdown chosen in the tray menu is kept only while vim mouse runs:
  quitting or updating vim mouse cancels it. `shutdown_at` applies whenever
  vim mouse runs (see "Start with Windows").

## Settings

`vim.ini` lives next to `vim.exe`, or in `%APPDATA%\vim mouse` when the
executable's folder is read-only. It is watched either way: saving it reloads
the settings automatically (a balloon confirms, or shows any problems). The file
created on first run documents every option with a comment. The most useful
ones:

- `hint_hotkey`, `quit_hotkey`, `pause_hotkey`, `lock_hotkey`, `mouse_mode_key`,
  `pause_tap_ms` (two presses of the pause hotkey within this many ms pause
  or resume, 0-1000, default 300; 0 = one press)
- `capslock_off` (1 = keep the CapsLock state off while vim mouse runs, not
  while paused, default; 0 = `Shift+CapsLock` turns the real CapsLock on and
  off)
- `speed`, `max_speed`, `accel_time`, `accel_curve`, `slow_speed` (movement,
  in screen pixels per second; Windows' pointer speed and "Enhance pointer
  precision" settings do not change it)
- `scroll_speed`, `scroll_max_speed`, `smooth_scroll` (1 = small steps once
  per display frame, default; 0 = whole notches only)
- `alt_scroll` (1 = Alt + `jk` scroll, Alt + `hl` back / forward, Alt +
  `u`/`d` fast scroll and Alt + `g g` / Alt + `Shift+g` top / bottom
  anywhere), `alt_scroll_auto` (1 = scroll the nearest scrollable area in
  line with the pointer when it is not over one),
  `alt_nav` (1 = Alt + `h`/`l` go back / forward and Alt + `,`/`.` scroll
  sideways, default; 0 = Alt + `h`/`l` scroll sideways),
  `bare_keys` (1 = outside text fields the Alt keys work without Alt,
  default; 0 = always Alt), `fast_scroll_lines` (notches per Alt + `u`/`d` tap, 1-50, default
  5), `fast_scroll_speed`, `fast_scroll_max_speed` (while held),
  `scroll_end_tap_ms` (two `g` taps within this many ms scroll to the top,
  0-1000, default 500; 0 = `g g` and `Shift+g` off), `center_tap_ms` (two
  `0` taps within this many ms move the pointer to the middle of the
  screen, 0-1000, default 300; 0 = off)
- `hint_mode` (`elements` or `grid`: what the hint hotkey opens first),
  `key_grid` (the key that switches between the element labels and the grid
  while the hints are open, default `;`; `none` = off),
  `scan_scope` (`monitor` or `window`), `scan_timeout` (100-5000 ms, default
  400: how long to wait for every window's elements before showing the ones
  found so far. The active window's elements are always waited for, up to
  3 s; with nothing found by then, the hints close),
  `browser_first` (1 = with a browser active, show its labels as soon as it
  answered and add the other windows' as they answer, default; 0 = wait for
  every window up to `scan_timeout` as for other apps),
  `scan_refresh` (0-5000 ms, default 500: while the hints are open, rescan
  this often so elements that appear get labels by themselves. 0 = off),
  `keep_labels` (1 = a label already on screen stays put and keeps its letters
  while the hints refresh; new elements get labels in free space, and the
  hotkey again relabels everything. 0 = labels follow their elements and
  disappear when the element goes away. Default 1),
  `click_return` (1 = after a click through the hints the pointer goes back
  to where it was, unless mouse mode is on, default; 0 = it stays on the
  element),
  `prescan` (1 = start finding elements and scrollable areas as soon as Alt
  goes down, so labels and Alt scrolling respond sooner; 0 = wait for the
  whole key combination),
  `scan_ahead` (1 = find the clickable elements as soon as the screen
  changed, after a scroll or click of vim mouse once the page has caught
  up, a scroll the browser reported, or another window coming to the
  front, so the labels are ready when the hint key comes; keys typed into
  the app drop that scan. 0 = search only when the key is pressed),
  `scan_split` (1 = ask for the parts of a classic window, such as File
  Explorer's file list, folder tree and title bar, all at the same time,
  which finds its elements about twice as fast, and leave out the window
  icon's system menu, which alone takes about a third of File Explorer's
  search and which `Alt+Space` opens anyway. 0 = one search per window, system menu
  included. Default 1. Browsers are always searched in one go: they answer
  one question at a time),
  `scan_settle` (0-2000 ms, default 650: after vim mouse scrolled, clicked or
  went back or forward in Chrome, Edge or an app built on them, it waits at
  most this long from then for the page to catch up, because those report
  the page's new positions up to half a second late; hints opened before
  the wait is over wait for it, hints opened later scan at once. 0 = never
  wait), `scan_flush` (1 = during that wait ask the browser to send its
  changes at once and end it as soon as they are in, and wait like that
  after scrolls the browser reports itself, such as the mouse wheel,
  default; 0 = wait out `scan_settle` after vim mouse's own scrolls and
  clicks only), `scan_early` (after a scroll, the browser's first answer
  usually has the page in and the second only confirms it: 1 = search for
  the elements from the first answer on when Windows' animation effects
  are off, since Chrome and Edge then scroll without animating, default;
  2 = always, for a browser whose smooth scrolling is off some other way;
  0 = search only once the page is confirmed still), `scan_focusable` (1 = in a browser, also label elements
  that only say they can take the keyboard focus, such as a `div` with
  `tabindex` whose click handler sits on an ancestor or a range slider,
  unless they are text, scroll areas, lie outside the page or cover over a
  quarter of it, default; 0 = only elements with a clickable control type
  or pattern), `scan_hidden_checkbox` (1 = in a browser, a checkbox or
  radio button that a page hides behind its own label, the input one pixel
  and the label drawn as the box, gets the label's box and is clicked
  there, default; 0 = such inputs are too small for a label),
  `labels_apart` (1 = a label that would sit on another label is drawn to
  the right of it, or below it at the screen's edge, the smaller element
  keeping its corner, default; 0 = every label at its element's top-left
  corner, one over the other)
- `alphabet` (the label letters, 2-60 unique letters or digits, default
  `asdfhjklqwertuopzxcvbnm`: the home row, then the top row, then the bottom
  row, each in keyboard order. The first letters begin every two-letter
  label and are the one-letter labels of a quiet screen, so the easiest keys
  go first; `g`, `i` and `y` are left out because they look like `q`, `j`
  and `v` in the label font. The grid's rows and columns use the same
  letters in this order)
- `cell_size`, `subgrid`, `subgrid_keys`, `font_size`, `opacity`
- `label_bg`, `label_fg`, `label_border`, `match_fg` colours (`#RRGGBB`)
- `key_left`, `key_down`, `key_up`, `key_right`, `key_left_button`,
  `key_right_click`, `key_middle_click`, `key_drag_toggle`, `key_scroll_up`,
  `key_scroll_down`, `key_scroll_left`, `key_scroll_right`, `key_scroll_end`
  (`g`: tap twice for the top, with Shift for the bottom), `key_center`
  (`0`: tap twice for the middle of the screen, with Alt anywhere; `none` = off), `key_hints`,
  `key_exit`, `key_help` remaps
- `hide_pointer` (0-60000 ms, default 2000: outside mouse mode the pointer is
  hidden; the real mouse shows it, and it hides again this long after the
  mouse was last used. 0 = never hidden)
- `indicator`, `log` (1 = write `vim.log`; 2 = also write, for every hint
  open, each element of the active window with where it is and whether it
  got a label or why not, and turn the early scan off)
- `shortcut_method` (`paste` or `type`: how text shortcuts insert their text)
- `shutdown_at` (a 24-hour time such as `23:30`: turn the computer off every
  day then; `none` = only when chosen in the tray menu, default),
  `shutdown_warning` (seconds the countdown window shows before a scheduled
  shutdown, 0-300, default 60; 0 = none), `shutdown_force` (1 = close apps
  even when they have unsaved work; default 0), see
  [Scheduled shutdown](#scheduled-shutdown)
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
- Update to latest version (see [Updating](#updating))
- Schedule shutdown (in 15 or 30 minutes, 1 or 2 hours, or at a time you
  type; shows "Shutdown at ..." with a cancel item while one is scheduled; see
  [Scheduled shutdown](#scheduled-shutdown))
- Quit

## Updating

"Update to latest version" in the tray menu downloads `vim.exe` from the
[latest release](https://github.com/kiiwoom8/v/releases/latest) and compares
it with the running program. If they are the same, a balloon says you are up
to date. Otherwise the new `vim.exe` replaces the old one in the same folder
and vim mouse restarts; a balloon confirms it. `vim.ini`, your text shortcuts
and "Start with Windows" are kept, since the program keeps its name and
folder.

Windows cannot overwrite a running program, so the old one is first renamed
to `vim.exe.old` and deleted when the new one starts. The download goes to
`vim.exe.new` next to it, so the folder must be writable: in
`C:\Program Files` the update fails and offers to open the download page, and
you replace `vim.exe` yourself after quitting vim mouse. The same happens
when GitHub cannot be reached. The UIAccess install (next section) is not
updated this way at all: the release's exe is not signed, so the menu item
says to build and run the setup script again.

## Hints on the Start menu and notifications

The Start menu, Search, the notification centre, Quick Settings and
notification banners are drawn by Windows above every ordinary window,
topmost ones included. vim mouse finds their elements (the scan takes the
active window and whatever the mouse would hit on the monitor, not only the
windows Windows lists for ordinary programs), and elements they cover get
no label, but the labels inside them are drawn under them, where you cannot
see them. Windows lets a program draw above that system UI only with the
UIAccess privilege: an exe whose manifest asks for it, signed by a
certificate the machine trusts, started from `C:\Program Files`. The same
privilege lets the keyboard hook and the hint clicks reach windows running
as administrator.

To install that build:

1. `mingw32-make uiaccess` builds `vim-uiaccess.exe`: the same program with
   `vim.uiaccess.manifest` embedded instead of `vim.manifest`.
2. `powershell -ExecutionPolicy Bypass -File tools\uiaccess_setup.ps1`
   makes a code-signing certificate the first time (`CN=vim mouse UIAccess`
   in your user's certificate store, its private key not exportable), signs
   the exe, then asks once for administrator rights to trust the certificate
   on the machine and copy the exe to `C:\Program Files\vim mouse\vim.exe`.
   It copies your `vim.ini` to `%APPDATA%\vim mouse` if there is none there
   yet (the installed program keeps its settings there), points "Start with
   Windows" at the new exe if it is on, stops the running vim mouse and
   starts the installed one.
3. With `log = 1`, `vim.log` (now in `%APPDATA%\vim mouse`) starts with
   `uiaccess: on`. Windows refuses to start a UIAccess exe at all when the
   signature or the folder is wrong, so a running install is a working one.

Run the script again after every build. `tools\uiaccess_setup.ps1 -Uninstall`
removes the installed copy and the certificate's trust and starts `vim.exe`
next to the source again. Trusting the certificate means this machine
accepts anything signed with its key, which is why the key cannot leave your
user's store.

## Troubleshooting

- **No labels on the Start menu, Search, the notification centre, Quick
  Settings or a notification banner.** Without the UIAccess install (previous
  section) the labels are drawn under them. `log = 2` lists those windows
  with `above the desktop band` and says what was found in them.

- **A hotkey clashes with another program.** Change it in `vim.ini` and save;
  it reloads immediately. Use `none` to unbind a hotkey.
- **A window cannot be clicked.** Programs running as administrator cannot be
  controlled from a normal process (Windows UIPI). Run vim mouse as
  administrator too if you need to drive those windows.
- **Hints on an open menu.** They work: while a menu or flyout is open vim
  mouse holds back the hotkey's Alt, so the menu stays open.
- **An app shows no labels.** It does not expose UI Automation (some games,
  remote desktops, custom-drawn apps), so the hints close again. Press `;`
  while they are open to use the grid there, or set `hint_mode = grid` to
  open the grid first.
  `mingw32-make probe` builds `tools\uia_probe.exe`, which lists what the scan
  finds on the monitor under the cursor (`-v` for every element; `-d` for
  the `log = 2` lines below without pressing the hotkey, `-w 5` to wait
  5 s first, `-o file` to write to a file).
- **Few labels the first time in Chrome, Edge or an Electron app.** These build
  their accessibility tree only when first asked. vim mouse asks each window
  you switch to in the background, so this only happens in a window that was
  already active when vim mouse started; press `Esc` and open the hints again.
- **Labels sit where things were a moment ago, or a panel that just opened
  has none.** Chrome, Edge and apps built on them report moved and new
  elements up to half a second late. After vim mouse's own scrolls and clicks,
  and after scrolls the browser reports (mouse wheel, touchpad, keys), vim
  mouse asks the browser to catch up right away, and hints opened before
  that is over wait for it (`scan_flush`, `scan_settle`).
  After clicking with the mouse, wait a moment before opening the hints, or
  press the hint hotkey again to relabel. If labels land in the wrong place
  right after scrolling, set `scan_flush = 0` and tell us (with `log = 1`,
  `vim.log` says how each wait ended and whether it ended ahead of the
  hotkey).
- **The hints take long to appear in a browser right after scrolling.** With
  `log = 1`, `vim.log` says how long each wait for the page took and how it
  ended: "stopped" or "asked" is the browser answering (a few dozen
  milliseconds after a scroll with Windows' animation effects off, up to
  about a third of a second with them on, usually before the hotkey;
  "scanned on the first flush" means the search ran while the second
  answer only confirmed the page), "time limit" means it did not
  (`scan_settle`, 650 ms). "N targets after M ms"
  is the search itself, which runs in the browser: a long, busy page takes
  longer. Send those lines with a report.
- **A panel, menu or page element gets no label.** Set `log = 2`, open the
  hints where the label is missing, press `Esc`, and look in `vim.log` for
  the lines starting with `scan:`. Each element of the active window is
  listed with its position, size and name, and either `kept` or why it was
  left out: `offscreen (UI Automation)` or `disabled (UI Automation)` (the
  app itself hides it from UI Automation; `offscreen per Chromium, taken
  back into its dialog or menu` means vim mouse labels it anyway, see
  above), `hidden by a window` (another
  window is on top of it; the window lines above say which), `under a
  dialog or menu`, `container`, `too small` and so on (`kept, moved off an
  overlay` is a page element partly under a panel, clicked on the part that
  shows; `focusable only: ...` is an element that only says it can take the
  keyboard focus and was judged not worth a click, see `scan_focusable`).
  An element that is not listed at all is not
  exposed by the app: in a web page, a plain `div` or `span` made clickable
  by a click handler on an ancestor alone, with no role, `tabindex` or
  handler of its own (often only `cursor: pointer` shows it), is invisible
  to UI Automation and to every program using it; press `;` for the grid
  there. Set `log` back to 0 or 1 afterwards; the lines are
  many.
- **A web page element gets no label.** With Node and Chrome installed,
  `mingw32-make check` builds `tools\hint_check.exe` and
  `node tools\hint_check.mjs [url ...]` opens each page in a separate Chrome
  profile and lists the clickable elements the scan misses, with the reason.
- **An app's Alt+H/J/K/L/U/D/G/,/. shortcut no longer works.** vim mouse uses
  those for scrolling and back / forward; set `alt_scroll = 0` in `vim.ini` to
  give them back.
- **A letter such as `j` or `f` does not reach an app.** Outside text fields
  vim mouse takes `f h j k l u d g , .` and `Shift+g` without Alt (YouTube's
  `j k l f`, for example). Click into a text field to type them, or set
  `bare_keys = 0`.
- **A letter types instead of scrolling outside a text field.** With
  `log = 1`, `vim.log` says why on the first such key ("bare keys: off, popup
  ... is open" or "bare keys: off, focus text field in ..." with the window
  class and any caret).
- **A letter scrolls or goes back instead of typing.** vim mouse did not
  recognise that text field. With `log = 1`, `vim.log` shows what it decided
  after each focus change ("focus: text field" or "focus: no text field").
  `bare_keys = 0` turns the Alt-less keys off.
- **A text shortcut types nothing.** Its first letter may be an Alt key of vim
  mouse (`f h j k l u d g` by default); the editor warns about this when you
  save. Programs running as administrator cannot receive typed text either
  (see above).
- **The mouse pointer is gone.** Outside mouse mode vim mouse hides it
  (`hide_pointer`); move the real mouse, tap CapsLock, or set
  `hide_pointer = 0`. If it stays blank after vim mouse was killed or
  crashed, start vim mouse again (it puts the cursors back when it quits)
  or sign out and in.
- **A hover menu closes after a hint click.** The pointer goes back to where
  it was after the click, so a menu that stays open only while the pointer
  is over it closes. `Alt` + label moves the pointer there without clicking,
  or set `click_return = 0`.
- **I need the real CapsLock.** vim mouse keeps the CapsLock state off. Set
  `capslock_off = 0`, then `Shift+CapsLock` turns the real CapsLock on and
  off. If Windows is set to turn Caps Lock off with the Shift key (Settings,
  Time & language, Typing, Advanced keyboard settings, Input language hot
  keys), vim mouse cannot turn it off; `vim.log` says so with `log = 1`.
- **The computer did not shut down at the scheduled time.** It was asleep
  then (a balloon says "Shutdown skipped"), vim mouse was quit or updated in
  between (a shutdown chosen in the tray menu is not saved), or an app with
  unsaved work asked to save it: answer it, or set `shutdown_force = 1`. With
  `log = 1`, `vim.log` has a "shutdown:" line for each step.
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
`mingw32-make shutdown-test` builds `tools\shutdown_test.exe`, which checks the
scheduled shutdown's timing against a fake clock and never shuts down.
`mingw32-make uiaccess` builds `vim-uiaccess.exe`, the UIAccess build (see
"Hints on the Start menu and notifications"), which only runs signed and
installed by `tools\uiaccess_setup.ps1`.

Source layout:

- `main.c` - entry point, single instance, DPI awareness, main window, message
  routing, settings watcher, logging
- `config.c` - settings file defaults/parsing, key-name formatting, text
  shortcut table
- `input.c` - low-level keyboard hook, mouse mode, movement, Alt scrolling,
  text shortcut typing, sleep/lock handling
- `hint.c` - click-hint overlay
- `uia.c` - UI Automation: clickable-element scan, Alt scroll targets,
  text-field detection for bare keys
- `tray.c` - tray icon and menu, help window, start with Windows
- `snippet.c` - text shortcut editor window
- `update.c` - update to the latest release from the tray menu
- `shutdown.c` - scheduled shutdown: the schedule, the countdown window,
  turning the computer off
- `common.h` - shared contract between modules
- `version.h` - version numbers and strings, shared by the code and `vim.rc`
- `vim.rc`, `vim.manifest`, `vim.ico` - icon, manifest and version resources;
  `vim.uiaccess.manifest` is the manifest of the UIAccess build
- `tools/uiaccess_setup.ps1` - signs and installs the UIAccess build
