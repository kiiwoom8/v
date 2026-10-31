#!/bin/bash
# tools/ax_sim/run.sh <page or url> [steps-json]
#
# Runs uia.c's own element filters (filter_containers, cover_overlays,
# target_add and a copy of scan_window_uia's per-element checks) on the tree
# Chromium really builds for a page, on Linux, where no UI Automation exists.
# Chromium's AT-SPI tree carries the same bounds and offscreen state as its
# UI Automation tree. Prints every element left out with the reason, then the
# labels (rect corner where the label goes, click point).
#
#   tools/ax_sim/run.sh google.html?xs=1 '[{"click":"#avatar"},{"wait":1500}]'
#   OLD=fac3443 tools/ax_sim/run.sh zendesk.html '[{"click":"#views"},{"wait":1200}]'
#
# A bare page name is served from tools/ax_sim/pages on http://localhost:8001;
# the same files on http://127.0.0.1:8002 make a cross-site iframe
# (google.html?xs=1). OLD=<git revision> also runs that revision's uia.c and
# diffs the two. Needs Xvfb, dbus-launch, at-spi2-core, python3-gi with
# gir1.2-atspi-2.0, Node with Playwright and gcc; zendesk.html also npm.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
WORK=${AX_WORK:-${TMPDIR:-/tmp}/ax_sim}
mkdir -p "$WORK"

# Python with the gi module (it is built for the system's own Python).
if [ -z "$AX_PYTHON" ]; then
    for p in python3 /usr/bin/python3 python3.12 python3.11; do
        if $p -c 'import gi' 2>/dev/null; then AX_PYTHON=$p; break; fi
    done
fi
export AX_PYTHON=${AX_PYTHON:-python3}

# Xvfb, a D-Bus session and the AT-SPI bus, once.
if [ ! -f "$WORK/env.sh" ] || ! (. "$WORK/env.sh"; xdpyinfo -display :99 >/dev/null 2>&1 || [ -e /tmp/.X11-unix/X99 ]); then
    Xvfb :99 -screen 0 1600x1000x24 >/dev/null 2>&1 &
    sleep 0.5
    eval "$(dbus-launch --sh-syntax)"
    DISPLAY=:99 /usr/libexec/at-spi-bus-launcher --launch-immediately >/dev/null 2>&1 &
    sleep 1
    cat > "$WORK/env.sh" <<EOT
export DISPLAY=:99
export DBUS_SESSION_BUS_ADDRESS='$DBUS_SESSION_BUS_ADDRESS'
export ACCESSIBILITY_ENABLED=1
EOT
fi
. "$WORK/env.sh"

# The pages, on two origins.
mkdir -p "$WORK/www"
cp "$HERE"/pages/*.html "$WORK/www/"
if [[ "$1" == zendesk* ]] && [ ! -f "$WORK/www/zendesk.js" ]; then
    (cd "$WORK" && [ -d node_modules/@zendeskgarden ] || npm install --no-audit --no-fund \
        react@18.3.1 react-dom@18.3.1 styled-components@5.3.11 \
        @zendeskgarden/react-theming@8 @zendeskgarden/react-dropdowns@8 \
        @zendeskgarden/react-modals@8 @zendeskgarden/react-buttons@8 esbuild@0.20.2 >/dev/null)
    cp "$HERE/pages/zendesk.jsx" "$WORK/zendesk.jsx"
    (cd "$WORK" && npx esbuild zendesk.jsx --bundle --minify \
        --define:process.env.NODE_ENV='"production"' --outfile=www/zendesk.js --log-level=warning)
fi
for port in 8001 8002; do
    curl -s --noproxy '*' -o /dev/null "http://127.0.0.1:$port/" ||
        (cd "$WORK/www" && python3 -m http.server $port --bind 127.0.0.1 >/dev/null 2>&1 &)
done
sleep 0.3

URL=$1
[[ "$URL" == *://* ]] || URL="http://localhost:8001/$URL"
NODE_PATH=${NODE_PATH:-$(npm root -g)} node "$HERE/run.cjs" "$URL" "${2:-[]}" > "$WORK/tree.txt"
python3 "$HERE/conv.py" < "$WORK/tree.txt" > "$WORK/input.txt"

sim() {  # sim <uia.c> <name>: builds and runs the simulation of that uia.c
    python3 "$HERE/mk.py" "$1" "$WORK/$2.c"
    gcc -O1 -w -o "$WORK/$2" "$WORK/$2.c" -lm
    "$WORK/$2" < "$WORK/input.txt"
}
if [ -n "$OLD" ]; then
    git -C "$ROOT" show "$OLD:uia.c" > "$WORK/uia_old.c"
    sim "$WORK/uia_old.c" sim_old > "$WORK/old.txt"
    sim "$ROOT/uia.c" sim_new > "$WORK/new.txt"
    diff "$WORK/old.txt" "$WORK/new.txt" && echo "(no difference)"
else
    sim "$ROOT/uia.c" sim_new
fi
