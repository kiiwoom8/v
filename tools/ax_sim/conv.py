# conv.py - turns dump.py output into the simulation input: one line per
# element in tree order, "<UIA control type> <actionable> <x> <y> <w> <h> <name>".
# Offscreen elements are left out, as the scan's condition does. List and tree
# items count as not actionable (no pattern), as a plain li or td does.
import sys, re

M = {'push button': (50000, 1), 'link': (50005, 1), 'list item': (50007, 0),
     'menu item': (50011, 1), 'check box': (50002, 1),
     'radio button': (50013, 1), 'entry': (50004, 1), 'combo box': (50003, 1),
     'page tab': (50019, 1), 'tree item': (50024, 0),
     'toggle button': (50000, 1), 'document web': (50030, 0),
     'menu': (50009, 0), 'dialog': (50032, 0)}

for line in sys.stdin:
    m = re.match(r'\s*(.+?) "(.*)" (-?\d+),(-?\d+) (\d+)x(\d+) (OFFSCREEN )?', line)
    if not m: continue
    role, name, x, y, w, h, off = m.groups()
    if role not in M or off: continue      # offscreen ones the UIA condition leaves out
    t, a = M[role]
    print(t, a, x, y, w, h, (name or role)[:36] or '-')
