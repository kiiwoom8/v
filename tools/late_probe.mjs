// late_probe.mjs - content a page shows late, in a temporary Chrome, timed
// by late_probe.exe: when the element reaches UI Automation (as a scan sees
// it) and what the browser reported, in ms from the page's own change.
// usage: node tools/late_probe.mjs [scenario,...] [--flush=ms,ms]
// Scenarios: insert (a nav added to the DOM), display (display: none to
// block), visibility (hidden to visible), aria (aria-hidden removed), slide
// (a fixed nav moved in by transform, no transition), slideanim (the same
// over 200 ms), left (moved in by its left), details (a <details> opened),
// offcanvas (shown and slid in over 300 ms, as Bootstrap's offcanvas does),
// jsanim (slid in by a script's transform every frame for 300 ms).
// Each runs once per flush setting (default 0 and 30: none, and an MSAA hit
// test every 30 ms). The page changes itself through the DevTools protocol;
// no input is injected and the window need not be in front. Chrome starts
// with its own profile and occlusion off (a covered page reports nothing).
// --open is the live test of the hints (scan_watch): no probe; Chrome is
// brought to the front (tools/vim_post.exe), the running vim mouse's hints
// open by message, the page changes 700 ms later and the hints close 2.5 s
// after that; with log = 1, vim.log says how soon the new labels came. The
// overlay takes the keyboard meanwhile, so say so to whoever is at the
// machine. Chrome is maximized then, as a browser usually is (a small one
// leaves the window behind it so many labels that the room for late ones
// can run out); the window that was in front gets the foreground back.
// --lead=ms changes the page that long before the open instead (a menu
// button clicked, then the hint key). With log = 2 (milliseconds in vim.log)
// the run ends with vim.log's lines of each open, in ms from the change.
import { spawn, spawnSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync, statSync } from "node:fs";
import { createServer } from "node:http";
import { tmpdir } from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
const all = "insert,display,visibility,aria,slide,slideanim,left,details,offcanvas,jsanim";
const scenarios = (args.find((a) => !a.startsWith("--")) || all).split(",");
const open = args.includes("--open");
const lead = Number((args.find((a) => a.startsWith("--lead=")) || "--lead=-1").slice(7));
const logPath = path.join(process.env.APPDATA || "", "vim mouse", "vim.log");
const logSize = () => { try { return statSync(logPath).size; } catch { return 0; } };
const flushes = ((args.find((a) => a.startsWith("--flush=")) || "--flush=0,30").slice(8)).split(",").map(Number);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const links = Array.from({ length: 20 }, (_, i) => `<a href="#s${i}">static ${i}</a>`).join(" ");
const nav = '<a href="#t">late target</a> <a href="#u">late other</a>';
const html = `<!doctype html><title>late</title>
<style>body{margin:8px;font:16px sans-serif} a{display:inline-block;padding:6px}
#nav{background:#ddf;padding:6px}
.slide{position:fixed;left:0;top:120px;transform:translateX(-150%)} .slide.on{transform:none}
.anim{transition:transform .2s} .left{position:fixed;left:-700px;top:120px}
.oc{position:fixed;left:0;top:120px;visibility:hidden;transform:translateX(-100%);transition:transform .3s ease-in-out,visibility .3s}
.oc.on{visibility:visible;transform:none}</style>
<h1>late</h1><p>${links}</p><div id=box></div>
<script>
const nav = ${JSON.stringify(nav)};
const box = document.getElementById("box");
const setup = {
  insert() {},
  display() { box.innerHTML = '<nav id=nav style="display:none">' + nav + '</nav>'; },
  visibility() { box.innerHTML = '<nav id=nav style="visibility:hidden">' + nav + '</nav>'; },
  aria() { box.innerHTML = '<nav id=nav aria-hidden="true">' + nav + '</nav>'; },
  slide() { box.innerHTML = '<nav id=nav class=slide>' + nav + '</nav>'; },
  slideanim() { box.innerHTML = '<nav id=nav class="slide anim">' + nav + '</nav>'; },
  left() { box.innerHTML = '<nav id=nav class=left>' + nav + '</nav>'; },
  details() { box.innerHTML = '<details id=d><summary>menu</summary><nav id=nav>' + nav + '</nav></details>'; },
  offcanvas() { box.innerHTML = '<nav id=nav class=oc>' + nav + '</nav>'; },
  jsanim() { box.innerHTML = '<nav id=nav class=slide>' + nav + '</nav>'; },
};
const change = {
  insert() { box.innerHTML = '<nav id=nav>' + nav + '</nav>'; },
  display() { document.getElementById("nav").style.display = "block"; },
  visibility() { document.getElementById("nav").style.visibility = "visible"; },
  aria() { document.getElementById("nav").removeAttribute("aria-hidden"); },
  slide() { document.getElementById("nav").classList.add("on"); },
  slideanim() { document.getElementById("nav").classList.add("on"); },
  left() { document.getElementById("nav").style.left = "0px"; },
  details() { document.getElementById("d").open = true; },
  offcanvas() { document.getElementById("nav").classList.add("on"); },
  jsanim() {
    const n = document.getElementById("nav"), t0 = performance.now();
    const step = (t) => {
      const k = Math.min(1, (t - t0) / 300);
      n.style.transform = k < 1 ? "translateX(" + (-150 * (1 - k)) + "%)" : "none";
      if (k < 1) requestAnimationFrame(step);
    };
    requestAnimationFrame(step);
  },
};
const s = new URLSearchParams(location.search).get("s");
setup[s]();
window.go = () => { change[s](); return Date.now(); };
</script>`;

const server = createServer((req, res) => {
  res.writeHead(200, { "Content-Type": "text/html; charset=utf-8", "Cache-Control": "no-store" });
  res.end(html);
});
await new Promise((r) => server.listen(8766, "127.0.0.1", r));
const base = "http://127.0.0.1:8766";

const vimPost = (...a) => spawnSync(path.join(here, "vim_post.exe"), a, { encoding: "utf8" }).stdout.trim();
// The window in front before the test Chrome takes it, given back at the end.
const before = open ? (vimPost("fg").match(/^was (\d+)/m) || [])[1] || "" : "";

const profile = mkdtempSync(path.join(tmpdir(), "lateprobe-"));
const chrome = spawn("C:/Program Files/Google/Chrome/Application/chrome.exe", [
  `--user-data-dir=${profile}`, "--remote-debugging-port=9335", "--force-renderer-accessibility",
  "--disable-features=CalculateNativeWinOcclusion", "--disable-backgrounding-occluded-windows",
  "--disable-renderer-backgrounding", "--no-first-run", "--no-default-browser-check",
  ...(open ? ["--start-maximized"] : ["--window-size=800,600", "--window-position=60,60"]), `${base}/?s=insert`,
], { stdio: "ignore" });

let ws;
for (let i = 0; i < 40 && !ws; ++i) {
  await sleep(250);
  try {
    const t = (await (await fetch("http://127.0.0.1:9335/json")).json()).find((x) => x.type === "page");
    if (t) ws = new WebSocket(t.webSocketDebuggerUrl);
  } catch {}
}
await new Promise((r) => ws.addEventListener("open", r));
let id = 0;
const waiting = new Map();
ws.addEventListener("message", (m) => {
  const j = JSON.parse(m.data);
  if (j.id && waiting.has(j.id)) { waiting.get(j.id)(j); waiting.delete(j.id); }
});
const send = (method, params = {}) => new Promise((r) => { waiting.set(++id, r); ws.send(JSON.stringify({ id, method, params })); });
await send("Page.enable");
await sleep(2500);

if (open) {
  const logFrom = logSize();
  const runs = [];
  const goNow = async (s) => {
    const r = await send("Runtime.evaluate", { expression: "go()", returnByValue: true });
    const t = Math.round(r.result.result.value);
    console.log(`${t} GO ${s}`);
    return t;
  };
  console.log(`FRONT ${vimPost("front", String(chrome.pid)).replace(/\r?\n/g, "; ")}`);
  for (const s of scenarios) {
    await send("Page.navigate", { url: `${base}/?s=${s}` });
    await sleep(1500);
    let go, opened;
    if (lead >= 0) {
      go = await goNow(s);
      await sleep(Math.max(0, go + lead - Date.now()));
      opened = Date.now();
      console.log(`${opened} POST open: ${vimPost("open")}`);
    } else {
      opened = Date.now();
      console.log(`${opened} POST open: ${vimPost("open")}`);
      await sleep(700);
      go = await goNow(s);
    }
    await sleep(2500);
    console.log(`${Date.now()} POST close: ${vimPost("close")}`);
    runs.push({ s, go, from: Math.min(go, opened) - 100, to: Date.now() });
    await sleep(600);
  }
  if (before) console.log(`BACK ${vimPost("back", before)}`);
  flushes.length = 0;
  // vim.log's lines of each open (log = 2 stamps them to the ms), in ms
  // from that scenario's change.
  const day = new Date();
  const lines = readFileSync(logPath).subarray(logFrom).toString("utf8").split(/\r?\n/);
  for (const run of runs) {
    console.log(`\n=== ${run.s}${lead >= 0 ? `, page changed ${lead} ms before the open` : ""}`);
    for (const l of lines) {
      const m = l.match(/^\[(\d\d):(\d\d):(\d\d)\.(\d{3})\] (.*)$/);
      if (!m) continue;
      const t = new Date(day.getFullYear(), day.getMonth(), day.getDate(), +m[1], +m[2], +m[3], +m[4]).getTime();
      if (t >= run.from && t <= run.to) console.log(`${String(t - run.go).padStart(6)}  ${m[5]}`);
    }
  }
}
const summary = [];
for (const flush of flushes) {
  for (const s of scenarios) {
    await send("Page.navigate", { url: `${base}/?s=${s}&f=${flush}` });
    await sleep(1500);
    const lines = [];
    const probe = spawn(path.join(here, "late_probe.exe"), [String(chrome.pid), "3", "late target", String(flush)]);
    probe.stdout.setEncoding("utf8");
    let buf = "";
    let ready;
    const isReady = new Promise((r) => (ready = r));
    probe.stdout.on("data", (d) => {
      buf += d;
      for (let i; (i = buf.indexOf("\n")) >= 0; buf = buf.slice(i + 1)) {
        const l = buf.slice(0, i).trim();
        lines.push(l);
        if (/ READY | FAIL /.test(l)) ready();
      }
    });
    await isReady;
    await sleep(400);
    const r = await send("Runtime.evaluate", { expression: "go()", returnByValue: true });
    const t0 = Math.round(r.result.result.value);
    lines.push(`${t0} GO ${s}`);
    await new Promise((res) => probe.on("exit", res));
    console.log(`\n=== ${s}, flush ${flush} ms`);
    let found = null, firstEv = null;
    let evs = 0;
    for (const l of lines.sort((a, b) => Number(a.split(" ")[0]) - Number(b.split(" ")[0]))) {
      const [t, ...rest] = l.split(" ");
      const d = Number(t) - t0;
      const text = rest.join(" ");
      if (/^EV /.test(text) && d >= 0) { ++evs; if (firstEv === null) firstEv = `${d} ${text.split(" ")[1]}`; if (evs > 40) continue; }
      if (/^FOUND on screen/.test(text) && d >= 0 && found === null) found = d;
      console.log(`${String(d).padStart(6)}  ${text}`);
    }
    summary.push(`${s.padEnd(11)} flush ${String(flush).padStart(3)}: on screen at ${found ?? "never"} ms, first event ${firstEv ?? "none"}, ${evs} events`);
  }
}
ws.close();
spawnSync("taskkill", ["/F", "/T", "/PID", String(chrome.pid)]);
server.close();
await sleep(1500);
try { rmSync(profile, { recursive: true, force: true }); } catch {}
console.log("\n" + summary.join("\n"));
process.exit(0);
