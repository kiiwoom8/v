// load_probe.mjs - slow pages in a temporary Chrome, watched by load_probe.exe.
// usage: node tools/load_probe.mjs [scenario,...] [--open]
// Scenarios: stream (content in four parts over 4.5 s), slowimg (all content
// at once, one image holds the load event 5 s), ttfb (nothing for 3 s, the
// old page stays). Each starts from a fast page in the same tab, through the
// DevTools protocol (no input is injected). Prints the probe's lines and the
// driver's (NAV, CDP committed / DOMContentLoaded / load) in time order, in
// ms from each navigation. --open also posts WM_APP_HINT_OPEN to the running
// vim mouse 300 ms after each navigation and WM_APP_HINT_CLOSE 6 s later
// (tools/vim_post.exe), for a live test of the overlay with log = 1; the
// overlay captures the keyboard meanwhile, so say so to whoever is at the
// machine. Chrome starts with its own profile at the top-left of the primary
// monitor, with occlusion off (a covered page is hidden and reports nothing).
import { spawn, spawnSync } from "node:child_process";
import { mkdtempSync, rmSync } from "node:fs";
import { createServer } from "node:http";
import { tmpdir } from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
const open = args.includes("--open");
const scenarios = (args.find((a) => !a.startsWith("--")) || "stream,slowimg,ttfb").split(",");
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const lines = [];
const log = (s) => lines.push(`${Date.now()} ${s}`);

const links = (p, n) => Array.from({ length: n }, (_, i) => `<p><a href="#${p}${i}">${p} link ${i}</a></p>`).join("");
const page = (title, body) => `<!doctype html><title>${title}</title><h1>${title}</h1>${body}`;
const server = createServer(async (req, res) => {
  const p = req.url.split("?")[0];
  const html = { "Content-Type": "text/html; charset=utf-8", "Cache-Control": "no-store" };
  if (p === "/fast") { res.writeHead(200, html); res.end(page("fast", links("old", 20))); }
  else if (p === "/stream") {
    res.writeHead(200, html);
    res.write(page("stream", links("a", 10)) + " ".repeat(2048));
    for (const part of "bcd") { await sleep(1500); res.write(links(part, 10)); }
    res.end();
  } else if (p === "/slowimg") {
    res.writeHead(200, html);
    res.end(page("slowimg", '<img src="/img" width=50 height=50>' + links("s", 40)));
  } else if (p === "/ttfb") { await sleep(3000); res.writeHead(200, html); res.end(page("ttfb", links("t", 40))); }
  else if (p === "/img") {
    await sleep(5000);
    res.writeHead(200, { "Content-Type": "image/gif" });
    res.end(Buffer.from("R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7", "base64"));
  } else { res.writeHead(404); res.end(); }
});
await new Promise((r) => server.listen(8765, "127.0.0.1", r));
const base = "http://127.0.0.1:8765";

// The window in front before the test Chrome takes it, given back at the end.
const vimPost = (...a) => spawnSync(path.join(here, "vim_post.exe"), a, { encoding: "utf8" }).stdout.trim();
const before = open ? (vimPost("fg").match(/^was (\d+)/m) || [])[1] || "" : "";

const profile = mkdtempSync(path.join(tmpdir(), "loadprobe-"));
const chrome = spawn("C:/Program Files/Google/Chrome/Application/chrome.exe", [
  `--user-data-dir=${profile}`, "--remote-debugging-port=9334", "--force-renderer-accessibility",
  "--disable-features=CalculateNativeWinOcclusion", "--disable-backgrounding-occluded-windows",
  "--disable-renderer-backgrounding", "--no-first-run", "--no-default-browser-check",
  "--window-size=800,700", "--window-position=60,60", `${base}/fast`,
], { stdio: "ignore" });

const probe = spawn(path.join(here, "load_probe.exe"), [String(chrome.pid), String(6 + scenarios.length * 13)]);
probe.stdout.setEncoding("utf8");
let buf = "";
probe.stdout.on("data", (d) => {
  buf += d;
  for (let i; (i = buf.indexOf("\n")) >= 0; buf = buf.slice(i + 1)) lines.push(buf.slice(0, i).trim());
});

let ws;
for (let i = 0; i < 40 && !ws; ++i) {
  await sleep(250);
  try {
    const t = (await (await fetch("http://127.0.0.1:9334/json")).json()).find((x) => x.type === "page");
    if (t) ws = new WebSocket(t.webSocketDebuggerUrl);
  } catch {}
}
await new Promise((r) => ws.addEventListener("open", r));
let id = 0;
const send = (method, params = {}) => ws.send(JSON.stringify({ id: ++id, method, params }));
ws.addEventListener("message", (m) => {
  const j = JSON.parse(m.data);
  if (j.method === "Page.domContentEventFired") log("CDP DOMContentLoaded");
  else if (j.method === "Page.loadEventFired") log("CDP load");
  else if (j.method === "Page.frameNavigated" && !j.params.frame.parentId) log(`CDP committed ${j.params.frame.url}`);
});
send("Page.enable");
send("Page.bringToFront");
await sleep(4000);

const post = (what) => log(`POST hint ${what}: ${vimPost(what)}`);
if (open) log(`FRONT ${vimPost("front", String(chrome.pid)).replace(/\r?\n/g, "; ")}`);
for (const s of scenarios) {
  send("Page.navigate", { url: `${base}/fast` });
  await sleep(3000);
  log(`NAV ${s}`);
  send("Page.navigate", { url: `${base}/${s}` });
  if (open) { await sleep(300); post("open"); await sleep(6000); post("close"); await sleep(3700); }
  else await sleep(10000);
}
ws.close();
if (open && before) log(`BACK ${vimPost("back", before)}`);
await new Promise((r) => probe.on("exit", r));
spawnSync("taskkill", ["/F", "/T", "/PID", String(chrome.pid)]);
server.close();
await sleep(1500);
try { rmSync(profile, { recursive: true, force: true }); } catch {}

let t0 = 0;
for (const l of lines.sort((a, b) => Number(a.split(" ")[0]) - Number(b.split(" ")[0]))) {
  const [t, ...rest] = l.split(" ");
  if (/^NAV /.test(rest.join(" "))) t0 = Number(t);
  console.log(`${String(t0 ? Number(t) - t0 : 0).padStart(7)}  ${rest.join(" ")}`);
}
process.exit(0);
