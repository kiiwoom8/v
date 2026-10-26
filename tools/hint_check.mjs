// hint_check.mjs - drive a separate Chrome, collect the DOM, run hint_check.exe.
// No dependencies: uses Node's global fetch and WebSocket.

import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn, spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const PORT = 9333;
const HERE = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(HERE, '..');
const TOOL = path.join(ROOT, 'tools', 'hint_check.exe');
const CHROME = 'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe';

const DEFAULT_URLS = [
  'https://www.google.com/search?q=windows+ui+automation',
  'https://en.wikipedia.org/wiki/Vim_(text_editor)',
  'https://github.com/microsoft/terminal',
  'https://www.youtube.com/',
  'https://www.reddit.com/r/programming/',
  'https://news.ycombinator.com/',
  'https://stackoverflow.com/questions/tagged/winapi',
  'https://developer.mozilla.org/en-US/docs/Web/HTML/Element/button',
  'https://www.amazon.com/s?k=mechanical+keyboard',
  'https://www.bbc.com/news',
  'https://www.naver.com/',
  'https://www.youtube.com/watch?v=jNQXAC9IVRw',
  'https://gall.dcinside.com/board/lists/?id=programming'
];

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function parseArgs(argv) {
  const opts = { out: path.join(os.tmpdir(), 'hint_check_out'), keep: false, urls: [],
                 scroll: 0, delay: 0, runs: 2, scanFirst: false };
  for (let i = 0; i < argv.length; ++i) {
    const arg = argv[i];
    if (arg === '--out' && i + 1 < argv.length) {
      opts.out = path.resolve(argv[++i]);
    } else if (arg === '--keep') {
      opts.keep = true;
    } else if (arg === '--scroll' && i + 1 < argv.length) {
      opts.scroll = Number(argv[++i]) || 0;
    } else if (arg === '--delay' && i + 1 < argv.length) {
      opts.delay = Number(argv[++i]) || 0;
    } else if (arg === '--runs' && i + 1 < argv.length) {
      opts.runs = Math.max(1, Number(argv[++i]) || 1);
    } else if (arg === '--scan-first') {
      opts.scanFirst = true;
    } else if (arg === '--help' || arg === '-h') {
      opts.help = true;
    } else if (arg.startsWith('-')) {
      throw new Error(`unknown option ${arg}`);
    } else {
      opts.urls.push(arg);
    }
  }
  if (opts.urls.length === 0) opts.urls = DEFAULT_URLS.slice();
  return opts;
}

class CDP {
  constructor(url) {
    this.url = url;
    this.ws = null;
    this.nextId = 1;
    this.pending = new Map();
    this.listeners = new Map();
    this.closed = false;
  }

  connect(timeoutMs = 10000) {
    return new Promise((resolve, reject) => {
      const ws = new WebSocket(this.url);
      this.ws = ws;
      const timer = setTimeout(() => {
        reject(new Error('websocket connect timeout'));
        try { ws.close(); } catch {}
      }, timeoutMs);
      ws.onopen = () => {
        clearTimeout(timer);
        resolve();
      };
      ws.onerror = () => {
        clearTimeout(timer);
        reject(new Error('websocket error'));
      };
      ws.onclose = () => {
        this.closed = true;
        for (const pending of this.pending.values()) {
          clearTimeout(pending.timer);
          pending.reject(new Error('websocket closed'));
        }
        this.pending.clear();
      };
      ws.onmessage = (event) => this.onMessage(event.data);
    });
  }

  onMessage(raw) {
    let msg;
    try {
      msg = JSON.parse(typeof raw === 'string' ? raw : Buffer.from(raw).toString('utf8'));
    } catch {
      return;
    }
    if (msg.id) {
      const pending = this.pending.get(msg.id);
      if (!pending) return;
      this.pending.delete(msg.id);
      clearTimeout(pending.timer);
      if (msg.error) pending.reject(new Error(msg.error.message || 'CDP error'));
      else pending.resolve(msg.result);
      return;
    }
    if (!msg.method) return;
    const set = this.listeners.get(msg.method);
    if (!set) return;
    for (const cb of [...set]) {
      try { cb(msg.params); } catch {}
    }
  }

  send(method, params = {}, timeoutMs = 30000) {
    if (this.closed || !this.ws) return Promise.reject(new Error('websocket closed'));
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(`${method} timed out`));
      }, timeoutMs);
      this.pending.set(id, { resolve, reject, timer });
      try {
        this.ws.send(JSON.stringify({ id, method, params }));
      } catch (err) {
        this.pending.delete(id);
        clearTimeout(timer);
        reject(err);
      }
    });
  }

  on(method, cb) {
    let set = this.listeners.get(method);
    if (!set) {
      set = new Set();
      this.listeners.set(method, set);
    }
    set.add(cb);
  }

  off(method, cb) {
    const set = this.listeners.get(method);
    if (set) set.delete(cb);
  }

  waitFor(method, timeoutMs) {
    return new Promise((resolve) => {
      let done = false;
      const timer = setTimeout(() => finish(false), timeoutMs);
      const cb = () => finish(true);
      const finish = (value) => {
        if (done) return;
        done = true;
        clearTimeout(timer);
        this.off(method, cb);
        resolve(value);
      };
      this.on(method, cb);
    });
  }

  close() {
    try { if (this.ws) this.ws.close(); } catch {}
  }
}

async function getJson(url, timeoutMs = 2000) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeoutMs);
  try {
    const res = await fetch(url, { signal: controller.signal, cache: 'no-store' });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    return await res.json();
  } finally {
    clearTimeout(timer);
  }
}

async function portInUse(port) {
  try {
    await getJson(`http://127.0.0.1:${port}/json/version`, 500);
    return true;
  } catch {
    return false;
  }
}

async function waitForVersion(timeoutMs = 25000) {
  const deadline = Date.now() + timeoutMs;
  let last = '';
  while (Date.now() < deadline) {
    try {
      return await getJson(`http://127.0.0.1:${PORT}/json/version`, 1500);
    } catch (err) {
      last = String(err && err.message ? err.message : err);
    }
    await sleep(250);
  }
  throw new Error(`remote debugging endpoint did not start (${last})`);
}

function collectInPage() {
  const SEMANTIC = [
    'a[href]',
    'area[href]',
    'button',
    'input:not([type="hidden"])',
    'select',
    'textarea',
    'summary',
    '[role="button"]',
    '[role="link"]',
    '[role="checkbox"]',
    '[role="radio"]',
    '[role="tab"]',
    '[role="menuitem"]',
    '[role="menuitemcheckbox"]',
    '[role="menuitemradio"]',
    '[role="option"]',
    '[role="switch"]',
    '[role="combobox"]',
    '[role="treeitem"]',
    '[onclick]',
    '[tabindex]:not([tabindex="-1"])',
    '[contenteditable=""]',
    '[contenteditable="true"]'
  ].join(',');

  const VW = window.innerWidth;
  const VH = window.innerHeight;
  const DPR = window.devicePixelRatio || 1;

  function composedParent(node) {
    if (!node) return null;
    if (node.parentElement) return node.parentElement;
    const root = node.getRootNode ? node.getRootNode() : null;
    if (root && root.host) return root.host;
    return null;
  }

  function composedContains(ancestor, node) {
    let n = node;
    while (n) {
      if (n === ancestor) return true;
      n = composedParent(n);
    }
    return false;
  }

  function hasSemanticAncestor(el) {
    let p = composedParent(el);
    while (p) {
      try {
        if (p.matches && p.matches(SEMANTIC)) return true;
      } catch {}
      p = composedParent(p);
    }
    return false;
  }

  function deepElementFromPoint(x, y) {
    let el = document.elementFromPoint(x, y);
    let guard = 0;
    while (el && el.shadowRoot && guard < 20) {
      let inner = null;
      try { inner = el.shadowRoot.elementFromPoint(x, y); } catch {}
      if (!inner || inner === el) break;
      el = inner;
      ++guard;
    }
    return el;
  }

  function visibleHit(el, r) {
    const w = r.right - r.left;
    const h = r.bottom - r.top;
    const pts = [
      [r.left + w / 2, r.top + h / 2],
      [r.left + w / 4, r.top + h / 4],
      [r.left + (3 * w) / 4, r.top + h / 4],
      [r.left + w / 4, r.top + (3 * h) / 4],
      [r.left + (3 * w) / 4, r.top + (3 * h) / 4]
    ];
    for (const [x, y] of pts) {
      const hit = deepElementFromPoint(x, y);
      if (hit && composedContains(el, hit)) return true;
    }
    return false;
  }

  function attr(el, name) {
    try { const v = el.getAttribute(name); return v == null ? '' : String(v); } catch { return ''; }
  }

  function textOf(el) {
    const parts = [];
    try { if (el.innerText) parts.push(el.innerText); } catch {}
    for (const name of ['aria-label', 'title', 'alt']) {
      const v = attr(el, name);
      if (v) parts.push(v);
    }
    let value = '';
    try { if (typeof el.value === 'string') value = el.value; } catch {}
    if (!value) value = attr(el, 'value');
    if (value) parts.push(value);
    const s = parts.join(' ').replace(/\s+/g, ' ').trim();
    return Array.from(s).slice(0, 40).join('');
  }

  function roleOf(el) {
    return attr(el, 'role').replace(/\s+/g, ' ').trim();
  }

  function tagOf(el) {
    return String(el.tagName || '').toLowerCase();
  }

  function iou(a, b) {
    const iw = Math.min(a.right, b.right) - Math.max(a.left, b.left);
    const ih = Math.min(a.bottom, b.bottom) - Math.max(a.top, b.top);
    if (iw <= 0 || ih <= 0) return 0;
    const inter = iw * ih;
    const areaA = Math.max(0, a.right - a.left) * Math.max(0, a.bottom - a.top);
    const areaB = Math.max(0, b.right - b.left) * Math.max(0, b.bottom - b.top);
    const uni = areaA + areaB - inter;
    return uni > 0 ? inter / uni : 0;
  }

  const all = [];
  function walk(root) {
    let els;
    try { els = root.querySelectorAll('*'); } catch { return; }
    for (const el of els) {
      all.push(el);
      if (el.shadowRoot) walk(el.shadowRoot);
    }
  }
  walk(document);

  const raw = [];
  for (const el of all) {
    let semantic = false;
    try { semantic = el.matches(SEMANTIC); } catch {}
    let pointer = false;
    if (!semantic && !hasSemanticAncestor(el)) {
      let cs = null;
      try { cs = window.getComputedStyle(el); } catch {}
      if (cs && cs.cursor === 'pointer') {
        const parent = composedParent(el);
        let parentCursor = '';
        if (parent) {
          try { parentCursor = window.getComputedStyle(parent).cursor; } catch {}
        }
        pointer = parentCursor !== 'pointer';
      }
    }
    if (!semantic && !pointer) continue;

    let cs = null;
    try { cs = window.getComputedStyle(el); } catch {}
    if (!cs || cs.visibility === 'hidden' || cs.visibility === 'collapse') continue;

    let rect;
    try { rect = el.getBoundingClientRect(); } catch { continue; }
    const left = Math.max(rect.left, 0);
    const top = Math.max(rect.top, 0);
    const right = Math.min(rect.right, VW);
    const bottom = Math.min(rect.bottom, VH);
    if (right - left < 4 || bottom - top < 4) continue;
    const clipped = { left, top, right, bottom };
    if (!visibleHit(el, clipped)) continue;

    raw.push({
      el,
      rect: clipped,
      kind: semantic ? 'semantic' : 'pointer',
      tag: tagOf(el),
      role: roleOf(el),
      text: textOf(el)
    });
  }

  const items = [];
  for (let i = 0; i < raw.length; ++i) {
    let drop = false;
    for (let j = 0; j < raw.length; ++j) {
      if (i === j) continue;
      const a = raw[i];
      const b = raw[j];
      if (!composedContains(a.el, b.el)) continue;
      if (iou(a.rect, b.rect) >= 0.8) {
        drop = true;
        break;
      }
    }
    if (!drop) items.push(raw[i]);
  }

  return {
    innerWidth: VW,
    innerHeight: VH,
    devicePixelRatio: DPR,
    items: items.map((item, index) => ({
      id: index,
      x: item.rect.left,
      y: item.rect.top,
      w: item.rect.right - item.rect.left,
      h: item.rect.bottom - item.rect.top,
      kind: item.kind,
      tag: item.tag,
      role: item.role,
      text: item.text
    }))
  };
}

function num(n) {
  return (Math.round(n * 100) / 100).toFixed(2);
}

function writeDomTsv(file, data) {
  const lines = [`viewport\t${data.innerWidth}\t${data.innerHeight}\t${data.devicePixelRatio}`];
  for (const item of data.items) {
    const tag = String(item.tag || '').replace(/\s+/g, ' ').trim();
    const role = String(item.role || '').replace(/\s+/g, ' ').trim();
    const text = String(item.text || '').replace(/[\t\r\n]+/g, ' ').trim();
    lines.push([
      item.id, num(item.x), num(item.y), num(item.w), num(item.h),
      item.kind, tag, role, text
    ].join('\t'));
  }
  const tmp = `${file}.tmp`;
  fs.writeFileSync(tmp, lines.join('\n') + '\n', 'utf8');
  fs.rmSync(file, { force: true });
  fs.renameSync(tmp, file);
}

function parseSummary(text) {
  const line = text.split(/\r?\n/).find((row) => row.startsWith('SUMMARY '));
  if (!line) return null;
  const out = {};
  for (const field of line.trim().split(/\s+/).slice(1)) {
    const eq = field.indexOf('=');
    if (eq < 0) continue;
    const key = field.slice(0, eq);
    const value = field.slice(eq + 1);
    out[key] = /^-?\d+$/.test(value) ? Number(value) : value;
  }
  return out;
}

function pageLabel(url) {
  try {
    const u = new URL(url);
    const host = u.hostname.replace(/^www\./, '');
    const p = u.pathname === '/' ? '' : u.pathname;
    return (host + p).slice(0, 44);
  } catch {
    return String(url).slice(0, 44);
  }
}

function fileHost(url) {
  try {
    return new URL(url).hostname.replace(/[^A-Za-z0-9.-]+/g, '_') || 'page';
  } catch {
    return 'page';
  }
}

async function enablePageAccessibility(cdp) {
  try { await cdp.send('Accessibility.enable', {}, 10000); } catch {}
  try { await cdp.send('Accessibility.getFullAXTree', {}, 30000); } catch {}
}

function startTool(browserPid, tsvFile, opts, scanFirst) {  const args = [String(browserPid), tsvFile, '-n', String(opts.runs)];
  if (scanFirst) args.push('-s');
  const child = spawn(TOOL, args, {
    stdio: ['ignore', 'pipe', 'pipe'],
    windowsHide: true
  });
  let stdout = '';
  let stderr = '';
  if (child.stdout) {
    child.stdout.setEncoding('utf8');
    child.stdout.on('data', (data) => { stdout += data; });
  }
  if (child.stderr) {
    child.stderr.setEncoding('utf8');
    child.stderr.on('data', (data) => { stderr += data; });
  }
  const timer = setTimeout(() => {
    try { child.kill(); } catch {}
  }, 600000);
  const done = new Promise((resolve) => {
    let settled = false;
    const finish = (result) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      resolve(result);
    };
    child.on('close', (code, signal) => {
      finish({ code, signal, stdout, stderr });
    });
    child.on('error', (err) => {
      finish({ code: null, signal: null, stdout, stderr, error: err });
    });
  });
  return { child, done };
}

async function runPage(cdp, url, browserPid, outDir, index, opts) {
  const label = pageLabel(url);
  const host = fileHost(url);
  const base = `${index + 1}-${host}`;
  const tsvFile = path.join(outDir, `${base}.tsv`);
  const txtFile = path.join(outDir, `${base}.txt`);

  const loaded = cdp.waitFor('Page.loadEventFired', 15000);
  let navError = '';
  try {
    await cdp.send('Page.navigate', { url }, 20000);
  } catch (err) {
    navError = err.message;
  }
  const didLoad = await loaded;
  // Keep the spawned Chrome in front: when the user's own Chrome covers it,
  // Chromium does not expose the page's accessibility tree to UIA.
  try { await cdp.send('Page.bringToFront', {}, 5000); } catch {}
  await sleep(3000);
  // Scanning right after a scroll measures how stale Chrome's tree is then.
  if (opts.scroll) {
    await cdp.send('Runtime.evaluate', {
      expression: `window.scrollBy({ top: ${opts.scroll}, behavior: 'instant' })`
    }, 10000);
    if (opts.delay) await sleep(opts.delay);
  }
  try { await cdp.send('Page.bringToFront', {}, 5000); } catch {}

  let tool = null;
  if (opts.scanFirst) {
    // The tool scans first and waits for this exact file. Make sure a stale
    // copy cannot satisfy it before this run writes the new one.
    fs.rmSync(tsvFile, { force: true });
    fs.rmSync(`${tsvFile}.tmp`, { force: true });
    tool = startTool(browserPid, tsvFile, opts, true);
  }

  let dom;
  try {
    const result = await cdp.send('Runtime.evaluate', {
      expression: `(${collectInPage.toString()})()`,
      returnByValue: true,
      awaitPromise: true,
      userGesture: false
    }, 90000);
    if (result.exceptionDetails) {
      throw new Error(result.exceptionDetails.text || 'page script exception');
    }
    dom = result.result ? result.result.value : null;
    if (!dom || !Array.isArray(dom.items)) throw new Error('collector returned no items');
  } catch (err) {
    if (tool) {
      try { tool.child.kill(); } catch {}
      await tool.done;
    }
    const text = `PAGE ERROR ${url}\n${err.message}\n`;
    fs.writeFileSync(txtFile, text, 'utf8');
    return { url, label, failed: true, error: err.message, navError, didLoad, text };
  }

  let text = '';
  let status = null;
  if (tool) await enablePageAccessibility(cdp);
  writeDomTsv(tsvFile, dom);
  if (!tool) {
    try { await cdp.send('Page.bringToFront', {}, 5000); } catch {}
    await enablePageAccessibility(cdp);
    tool = startTool(browserPid, tsvFile, opts, false);
  }
  const result = await tool.done;
  status = result.code;
  text = result.stdout || '';
  if (result.stderr)
    text += (text && !text.endsWith('\n') ? '\n' : '') + result.stderr;
  if (result.error)
    text += `\nTOOL ERROR ${result.error.message}\n`;
  fs.writeFileSync(txtFile, text, 'utf8');

  const summary = parseSummary(text);
  return {
    url,
    label,
    failed: !summary || (summary.failed !== undefined && summary.failed !== 0) || status !== 0,
    summary,
    text,
    tsvFile,
    txtFile,
    navError,
    didLoad
  };
}

function table(rows) {
  const headers = [
    'page', 'dom', 'sem hit/total', 'ptr hit/total', 'extra', 'targets',
    'hidden', 'hit_hidden', 'outside', 'scan ms first/last',
    'filt', 'notctl', 'drop', 'shad', 'other'
  ];
  const data = rows.map((row) => {
    const s = row.summary;
    if (!s) {
      return [row.label, 'FAILED', '-', '-', '-', '-', '-', '-', '-', '-',
              '-', '-', '-', '-', '-'];
    }
    return [
      row.label,
      String(s.dom || 0),
      `${s.hit_semantic || 0}/${s.semantic || 0}`,
      `${s.hit_pointer || 0}/${s.pointer || 0}`,
      String(s.extra || 0),
      String(s.targets || 0),
      String(s.labels_hidden || 0),
      String(s.hit_hidden || 0),
      String(s.outside_doc || 0),
      `${s.scan_ms_first || 0}/${s.scan_ms_last || 0}`,
      String(s.class_filtered || 0),
      String(s.class_not_control || 0),
      String(s.class_dropped || 0),
      String(s.class_shadowed || 0),
      String(s.class_other || 0)
    ];
  });

  const widths = headers.map((h, i) =>
    Math.max(h.length, ...data.map((row) => String(row[i]).length)));
  const line = (row) => row.map((cell, i) => {
    const value = String(cell);
    return i === 0 ? value.padEnd(widths[i]) : value.padStart(widths[i]);
  }).join('  ');
  return [line(headers), line(widths.map((w) => '-'.repeat(w)))].concat(data.map(line)).join('\n');
}

async function killSpawned(child) {
  if (!child || child.exitCode !== null) return;
  spawnSync('taskkill', ['/PID', String(child.pid), '/T', '/F'], {
    stdio: 'ignore',
    windowsHide: true,
    timeout: 15000
  });
}

async function main() {
  let opts;
  try {
    opts = parseArgs(process.argv.slice(2));
  } catch (err) {
    console.error(err.message);
    process.exit(2);
  }
  if (opts.help) {
    console.log('usage: node tools/hint_check.mjs [--out <dir>] [--keep] ' +
                '[--scroll <px>] [--delay <ms>] [--runs <n>] [--scan-first] ' +
                '[url ...]');
    return;
  }
  if (!fs.existsSync(TOOL)) {
    console.error(`missing ${TOOL}; run "mingw32-make check" first`);
    process.exit(1);
  }
  if (!fs.existsSync(CHROME)) {
    console.error(`missing Chrome at ${CHROME}`);
    process.exit(1);
  }
  fs.mkdirSync(opts.out, { recursive: true });

  if (await portInUse(PORT)) {
    console.error(`port ${PORT} is already in use; refusing to attach to an existing Chrome`);
    process.exit(1);
  }

  const profileDir = path.join(os.tmpdir(), 'hint_check_profile');
  try { fs.rmSync(profileDir, { recursive: true, force: true }); } catch {}

  const args = [
    `--user-data-dir=${profileDir}`,
    `--remote-debugging-port=${PORT}`,
    '--force-renderer-accessibility',
    '--no-first-run',
    '--no-default-browser-check',
    '--window-position=40,40',
    '--window-size=1280,900',
    'about:blank'
  ];
  const child = spawn(CHROME, args, { stdio: 'ignore', windowsHide: false });
  let spawnError = '';
  child.on('error', (err) => { spawnError = err.message; });

  let browserCdp = null;
  let pageCdp = null;
  const rows = [];
  let browserPid = child.pid;
  let fatal = false;

  try {
    const version = await waitForVersion();
    if (!version.webSocketDebuggerUrl) throw new Error('no browser websocket url');
    browserCdp = new CDP(version.webSocketDebuggerUrl);
    await browserCdp.connect();

    try {
      const info = await browserCdp.send('SystemInfo.getProcessInfo', {}, 5000);
      const procs = Array.isArray(info.processInfo) ? info.processInfo : [];
      const browser = procs.find((p) => p.type === 'browser');
      if (browser && Number.isInteger(browser.id)) browserPid = browser.id;
    } catch {}

    const list = await getJson(`http://127.0.0.1:${PORT}/json/list`, 5000);
    const target = (Array.isArray(list) ? list : []).find((t) => t.type === 'page');
    if (!target || !target.webSocketDebuggerUrl) throw new Error('no page target');
    pageCdp = new CDP(target.webSocketDebuggerUrl);
    await pageCdp.connect();
    await pageCdp.send('Page.enable', {}, 10000);

    console.log(`Chrome pid ${browserPid} (spawned ${child.pid}), port ${PORT}`);
    console.log(`output: ${opts.out}`);
    console.log(`pages:  ${opts.urls.length}`);

    for (let i = 0; i < opts.urls.length; ++i) {
      const url = opts.urls[i];
      process.stdout.write(`[${i + 1}/${opts.urls.length}] ${url} ... `);
      if (spawnError) throw new Error(`Chrome failed to start: ${spawnError}`);
      const row = await runPage(pageCdp, url, browserPid, opts.out, i, opts);
      rows.push(row);
      if (row.failed) {
        console.log(`FAILED${row.error ? ` (${row.error})` : ''}`);
      } else {
        const s = row.summary;
        console.log(`dom ${s.dom} sem ${s.hit_semantic}/${s.semantic} ` +
                    `ptr ${s.hit_pointer}/${s.pointer} extra ${s.extra} ` +
                    `targets ${s.targets} hidden ${s.labels_hidden || 0} ` +
                    `hit_hidden ${s.hit_hidden || 0} ` +
                    `outside ${s.outside_doc || 0} ` +
                    `${s.scan_ms_first}/${s.scan_ms_last} ms`);
      }
    }
  } catch (err) {
    fatal = true;
    console.error(`error: ${err.message}`);
  } finally {
    if (!opts.keep) {
      if (browserCdp && !browserCdp.closed) {
        try { await browserCdp.send('Browser.close', {}, 5000); } catch {}
        await sleep(500);
      }
      await killSpawned(child);
    }
    if (pageCdp) pageCdp.close();
    if (browserCdp) browserCdp.close();
  }

  const text = table(rows);
  const reasons = [];
  for (const row of rows) {
    if (!row.summary) continue;
    const entries = Object.entries(row.summary)
      .filter(([key, value]) => key.startsWith('class_drop_') && Number(value) > 0);
    if (entries.length) {
      reasons.push(`  ${row.label}: ` +
        entries.map(([key, value]) => `${key.slice('class_drop_'.length)}=${value}`).join(' '));
    }
  }
  const report = text + '\n' +
    '\nmiss classes: filt=filtered, notctl=not-control, drop=dropped, shad=shadowed\n' +
    (reasons.length ? `\ndrop reasons:\n${reasons.join('\n')}\n` : '');
  console.log('\n' + report);
  fs.writeFileSync(path.join(opts.out, 'summary.txt'), report, 'utf8');
  console.log(`summary: ${path.join(opts.out, 'summary.txt')}`);

  if (opts.keep) {
    console.log(`kept Chrome running (pid ${browserPid}), profile ${profileDir}`);
  }
  if (fatal || rows.length === 0 || rows.every((row) => row.failed)) process.exitCode = 1;
}

main().catch((err) => {
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
