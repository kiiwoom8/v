# conv.py - turns dump.py output into the simulation input: one line per
# element in tree order,
# "<UIA control type> <actionable> <offscreen> <x> <y> <w> <h> <name>".
# Offscreen elements are flagged 1; the scan leaves them out unless they are
# taken back into a web dialog or menu (rescue_pick). List and tree items
# count as not actionable (no pattern), as a plain li or td does. An element
# of another role that Chromium calls focusable is what the web condition
# lists for its keyboard focus alone (scan_focusable): actionable 2, with
# the UIA control type Chromium gives that role, so the simulation applies
# focusable_left_out to it (not its scroll-area test: AT-SPI has no scroll
# pattern). A first line "#monitor <w> <h>" sets the monitor (default
# 1600x1000, run.sh's Xvfb).
import sys, re

M = {'push button': (50000, 1), 'link': (50005, 1), 'list item': (50007, 0),
     'menu item': (50011, 1), 'check box': (50002, 1),
     'radio button': (50013, 1), 'entry': (50004, 1), 'combo box': (50003, 1),
     'page tab': (50019, 1), 'tree item': (50024, 0),
     'toggle button': (50000, 1), 'document web': (50030, 0),
     'menu': (50009, 0), 'dialog': (50032, 0)}
# Focusable only: role -> UIA control type (Chromium's ax_platform_node_win)
F = {'section': 50026, 'panel': 50033, 'landmark': 50026, 'heading': 50020,
     'slider': 50015, 'image': 50006, 'canvas': 50006, 'status bar': 50020,
     'scroll pane': 50033}

for line in sys.stdin:
    m = re.match(r'\s*(.+?) "(.*)" (-?\d+),(-?\d+) (\d+)x(\d+) (OFFSCREEN )?(.*)$', line)
    if not m: continue
    role, name, x, y, w, h, off, flags = m.groups()
    if role in M:
        t, a = M[role]
    elif role in F and 'focusable' in flags.split():
        t, a = F[role], 2
    else:
        continue
    print(t, a, 1 if off else 0, x, y, w, h, (name or role)[:36] or '-')
