# vim mouse

Windows C program (MinGW, Win32 API): vim-style keyboard control of the mouse,
click hints through UI Automation, Alt and bare-key scrolling, text shortcuts.

Before changing code, read `HISTORY.md`: the architecture map, the rules
learned from past bugs (section 5), behaviour not to revert (section 6), and
where each kind of change goes (section 8). `common.h` is the module contract;
`README.md` is the user-facing behaviour.

Rules that past bugs keep coming back to:

- Keep the keyboard hook (`kb_proc`) fast; Windows silently drops a slow hook.
  Never block, call UI Automation or `SendMessage` from it.
- Never inject modifier keys (Shift, Alt); act after the physical key-up.
- Never run UI Automation on the UI thread; create every engine through
  `uia_engine_init`.
- A cancelled or partial scan must not change persistent state.
- Shutdown: `input_stop` before `uia_stop`.
- Code must build with both MinGW.org GCC 6.3 and mingw-w64 (CI, `-Werror`,
  32 and 64 bit); guard newer constants with `#ifndef`.
- A key or setting change also updates `config.c` defaults, parsing and
  template, the help window (`tray.c`), `README.md`, and `HISTORY.md`.
