# dump.py - prints the accessible tree of every web document Chromium shows,
# through AT-SPI: role, name, screen rect, and OFFSCREEN where Chromium marks
# it so. On Linux these come from the same BrowserAccessibility code as UI
# Automation's BoundingRectangle and IsOffscreen on Windows. --all prints
# every role, not just the ones the scan or conv.py care about.
import sys
import gi
gi.require_version('Atspi', '2.0')
from gi.repository import Atspi

EVERY = '--all' in sys.argv
ROLES = {'push button', 'link', 'menu item', 'list item', 'check box',
         'radio button', 'toggle button', 'entry', 'combo box', 'page tab',
         'tree item', 'document web', 'internal frame', 'dialog', 'menu',
         'list', 'section'}


def line(o, depth):
    role = o.get_role_name()
    name = (o.get_name() or '')[:50]
    e = o.get_extents(Atspi.CoordType.SCREEN)
    ss = o.get_state_set()
    off = '' if ss.contains(Atspi.StateType.SHOWING) else 'OFFSCREEN '
    flags = [n for st, n in ((Atspi.StateType.VISIBLE, 'visible'),
                             (Atspi.StateType.FOCUSABLE, 'focusable'),
                             (Atspi.StateType.ENABLED, 'enabled'))
             if ss.contains(st)]
    return '%s%s "%s" %d,%d %dx%d %s%s' % ('  ' * depth, role, name, e.x, e.y,
                                           e.width, e.height, off,
                                           ' '.join(flags))


def walk(o, depth, out):
    try:
        if EVERY or o.get_role_name() in ROLES:
            out.append(line(o, depth))
        n = o.get_child_count()
    except Exception:
        return
    for i in range(min(n, 400)):
        c = o.get_child_at_index(i)
        if c is not None:
            walk(c, depth + 1, out)


def documents(o, found):
    try:
        if o.get_role_name() == 'document web' and o.get_child_count() > 0:
            found.append(o)
            return
        n = o.get_child_count()
    except Exception:
        return
    for i in range(min(n, 400)):
        c = o.get_child_at_index(i)
        if c is not None:
            documents(c, found)


desk = Atspi.get_desktop(0)
for i in range(desk.get_child_count()):
    app = desk.get_child_at_index(i)
    if app is None or 'hrom' not in (app.get_name() or ''):
        continue
    found = []
    documents(app, found)
    for doc in found:
        out = []
        walk(doc, 0, out)
        print('\n'.join(out))
