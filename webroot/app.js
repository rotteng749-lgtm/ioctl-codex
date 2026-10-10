/* ioctl-codex — WebUI target selector.
 *
 * Runs inside the KernelSU-Next (or KernelSU) WebView. Everything here uses the
 * manager's root bridge, so we can list *installed* packages and *running*
 * processes with pids — something a normal app cannot do.
 *
 * Bridge compatibility
 * --------------------
 * KernelSU has shipped two shapes of the bridge over time:
 *   old: ksu.exec(cmd, options, callbackName)  -> callback(errno, stdout, stderr)
 *   new: ksu.exec(cmd) -> Promise<{errno, stdout, stderr}>
 * We detect which one is present and normalise both to a single async run().
 * If neither exists (page opened in a plain browser) we fall back to a tiny
 * built-in demo list so the UI is still inspectable.
 *
 * The only file we ever write is /data/adb/ioctl-codex/targets.txt, one package
 * name per line. We always write it atomically (tmp file + mv) so the Zygisk
 * module never reads a half-written list.
 */

'use strict';

const DATA_DIR   = '/data/adb/ioctl-codex';
const TARGETS    = DATA_DIR + '/targets.txt';

/* ---------------------------------------------------------------------------
 * Root bridge
 * ------------------------------------------------------------------------- */
const hasKsu = typeof ksu !== 'undefined' && ksu && typeof ksu.exec === 'function';

let cbSeq = 0;
const pending = Object.create(null);

// KernelSU calls the named global function when the command finishes.
function __codex_cb(id, errno, stdout, stderr) {
  const p = pending[id];
  if (!p) return;
  delete pending[id];
  p.resolve({ errno: Number(errno), stdout: stdout || '', stderr: stderr || '' });
}

async function run(cmd) {
  if (!hasKsu) {
    log('$ ' + cmd + '\n(demo mode: no root bridge)');
    return { errno: 0, stdout: '', stderr: '' };
  }
  // Old-style: exec(cmd, options, callbackName) with a global callback.
  try {
    const res = ksu.exec(cmd, '{}', '__codex_cb_' + (++cbSeq));
    // If exec returned a thenable (new API), await it.
    if (res && typeof res.then === 'function') {
      const r = await res;
      return { errno: Number(r.errno || 0), stdout: r.stdout || '', stderr: r.stderr || '' };
    }
    // Old API: register the pending slot keyed by the callback name we used.
    const cbName = '__codex_cb_' + cbSeq;
    window[cbName] = (errno, stdout, stderr) => __codex_cb(cbName, errno, stdout, stderr);
    return await new Promise((resolve) => { pending[cbName] = { resolve }; });
  } catch (e) {
    return { errno: -1, stdout: '', stderr: String(e) };
  }
}

function toast(msg) {
  try { if (hasKsu && ksu.toast) { ksu.toast(msg); return; } } catch (_) {}
  log('[toast] ' + msg);
}

/* ---------------------------------------------------------------------------
 * Console + tiny helpers
 * ------------------------------------------------------------------------- */
const $ = (id) => document.getElementById(id);
function log(line) {
  const el = $('console');
  el.textContent += (el.textContent ? '\n' : '') + line;
  el.scrollTop = el.scrollHeight;
}
function setStatus(s) { $('status').textContent = s; }
function esc(s) {
  return String(s).replace(/[&<>"']/g, (c) =>
    ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}

/* ---------------------------------------------------------------------------
 * State
 * ------------------------------------------------------------------------- */
let targets = new Set();     // package names currently in targets.txt
let apps    = [];            // { pkg, label, running, pid, system }
const runningPids = Object.create(null); // pkg -> pid (best effort)

/* ---------------------------------------------------------------------------
 * Parsing helpers
 * ------------------------------------------------------------------------- */

// Parse `pm list packages [-3]` output: lines of "package:com.foo.bar".
function parsePackages(stdout) {
  const out = [];
  for (const line of stdout.split('\n')) {
    const m = line.match(/^package:(.+)$/);
    if (m) out.push(m[1].trim());
  }
  return out;
}

// Parse `ps -A -o PID,NAME` / `ps -eo pid,comm`. We only care about names that
// look like package names (contain a dot) so we can match them to packages.
function parseRunning(stdout) {
  const pids = Object.create(null);
  const lines = stdout.split('\n');
  for (const line of lines) {
    const m = line.trim().match(/^(\d+)\s+(\S+)$/);
    if (!m) continue;
    const pid = m[1];
    let name = m[2];
    if (name.indexOf(':') !== -1) name = name.split(':')[0]; // strip :remote
    if (name.indexOf('.') === -1) continue;                  // not a package
    if (!pids[name]) pids[name] = pid;                       // keep first (main)
  }
  return pids;
}

// Best-effort human label. `pm dump` is slow, so we only call it for apps we
// actually render and cache the result.
const labelCache = Object.create(null);
async function labelFor(pkg) {
  if (labelCache[pkg]) return labelCache[pkg];
  // `cmd package resolve-activity` gives the launcher label quickly.
  const r = await run(`dumpsys package ${pkg} | grep -m1 -A0 'applicationInfo' 2>/dev/null; ` +
                      `cmd package dump ${pkg} 2>/dev/null | grep -m1 'labelRes'`);
  let label = pkg;
  const m = r.stdout.match(/nonLocalizedLabel=([^\s]+)/) || r.stdout.match(/label=([^\s]+)/);
  if (m) label = m[1];
  labelCache[pkg] = label;
  return label;
}

/* ---------------------------------------------------------------------------
 * Load
 * ------------------------------------------------------------------------- */
async function loadTargets() {
  const r = await run(`cat ${TARGETS} 2>/dev/null`);
  targets = new Set(
    r.stdout.split('\n')
      .map((s) => s.trim())
      .filter((s) => s && s[0] !== '#')
  );
  renderTargets();
}

async function loadRunning() {
  // `ps -A` is Toybox on modern Android; fall back to `ps -e`.
  let r = await run('ps -A -o PID,NAME 2>/dev/null');
  if (!r.stdout.trim()) r = await run('ps -e -o PID,NAME 2>/dev/null');
  if (!r.stdout.trim()) r = await run('ps 2>/dev/null');
  Object.assign(runningPids, parseRunning(r.stdout));
}

async function loadInstalled() {
  const inclSystem = $('onlySystem').checked;
  const cmd = inclSystem ? 'pm list packages' : 'pm list packages -3';
  const r = await run(cmd);
  let pkgs = parsePackages(r.stdout);

  apps = pkgs.map((pkg) => ({
    pkg,
    label: pkg,
    system: inclSystem ? !targets.has(pkg) && isSystemGuess(pkg) : false,
    running: !!runningPids[pkg],
    pid: runningPids[pkg] || null,
  }));

  // Resolve a nicer label for the first N visible apps (avoid 300 dumpsys).
  for (const a of apps.slice(0, 40)) a.label = await labelFor(a.pkg);

  renderApps();
}

// Heuristic: packages without a launcher are usually system/overlay packages.
function isSystemGuess(pkg) {
  return /^(com\.android\.|android|com\.google\.android)/.test(pkg);
}

/* ---------------------------------------------------------------------------
 * Render
 * ------------------------------------------------------------------------- */
function renderTargets() {
  const box = $('targets');
  const list = [...targets].sort();
  $('targetCount').textContent = String(list.length);
  if (!list.length) {
    box.innerHTML = '<div class="empty">No targets yet — toggle an app below.</div>';
    return;
  }
  box.innerHTML = list.map((pkg) => `
    <div class="item">
      <div class="meta">
        <div class="name">${esc(pkg)}</div>
        <div class="pkg">${esc(pkg)}</div>
      </div>
      <div class="badges">
        ${runningPids[pkg] ? `<span class="badge run">running</span>` : ''}
      </div>
      <div class="toggle on" data-pkg="${esc(pkg)}" role="switch" aria-checked="true"></div>
    </div>`).join('');
  bindToggles(box);
}

function renderApps() {
  const q = $('search').value.trim().toLowerCase();
  const onlyRunning = $('onlyRunning').checked;
  const onlyTargets = $('onlyTargets').checked;

  let view = apps.filter((a) => {
    if (q && !(a.pkg.toLowerCase().includes(q) || a.label.toLowerCase().includes(q))) return false;
    if (onlyRunning && !a.running) return false;
    if (onlyTargets && !targets.has(a.pkg)) return false;
    return true;
  });
  // Running + already-targeted first, then alphabetical.
  view.sort((a, b) =>
    (b.running - a.running) || (targets.has(b.pkg) - targets.has(a.pkg)) ||
    a.pkg.localeCompare(b.pkg));

  $('appCount').textContent = String(view.length);
  const box = $('apps');
  if (!view.length) {
    box.innerHTML = '<div class="empty">No apps match the filter.</div>';
    return;
  }
  box.innerHTML = view.map((a) => `
    <div class="item">
      <div class="meta">
        <div class="name">${esc(a.label)}</div>
        <div class="pkg">${esc(a.pkg)}</div>
      </div>
      <div class="badges">
        ${a.running ? `<span class="badge run">run</span>` : ''}
        ${a.pid ? `<span class="badge pid">${esc(a.pid)}</span>` : ''}
        ${a.system ? `<span class="badge sys">sys</span>` : ''}
      </div>
      <div class="toggle ${targets.has(a.pkg) ? 'on' : ''}" data-pkg="${esc(a.pkg)}"
           role="switch" aria-checked="${targets.has(a.pkg)}"></div>
    </div>`).join('');
  bindToggles(box);
}

function bindToggles(root) {
  root.querySelectorAll('.toggle').forEach((el) => {
    el.onclick = () => {
      const pkg = el.getAttribute('data-pkg');
      if (targets.has(pkg)) targets.delete(pkg);
      else targets.add(pkg);
      renderTargets();
      renderApps();
    };
  });
}

/* ---------------------------------------------------------------------------
 * Save (atomic write)
 * ------------------------------------------------------------------------- */
async function saveTargets() {
  const list = [...targets].sort();
  // Build the file content. We keep a header comment so a user editing by hand
  // knows the format.
  const body = '# ioctl-codex targets — one package per line, # = comment\n' +
               list.join('\n') + (list.length ? '\n' : '');
  // Base64-encode to avoid any shell-quoting hazards with the package list.
  const b64 = btoa(unescape(encodeURIComponent(body)));
  const cmd =
    `mkdir -p ${DATA_DIR} && ` +
    `echo ${b64} | base64 -d > ${TARGETS}.tmp && ` +
    `chmod 644 ${TARGETS}.tmp && mv -f ${TARGETS}.tmp ${TARGETS}`;
  const r = await run(cmd);
  if (r.errno === 0) {
    toast('Saved ' + list.length + ' target(s)');
    log('saved ' + list.length + ' target(s) to ' + TARGETS);
  } else {
    toast('Save failed');
    log('ERROR saving targets: ' + (r.stderr || r.errno));
  }
}

/* ---------------------------------------------------------------------------
 * Wiring
 * ------------------------------------------------------------------------- */
function debounce(fn, ms) {
  let t;
  return (...a) => { clearTimeout(t); t = setTimeout(() => fn(...a), ms); };
}

async function refreshAll() {
  setStatus('loading…');
  await loadTargets();
  await loadRunning();
  await loadInstalled();
  setStatus(`${apps.length} apps · ${Object.keys(runningPids).length} running · ` +
            `${targets.size} targeted`);
}

window.addEventListener('DOMContentLoaded', async () => {
  if (!hasKsu) {
    setStatus('demo mode (open from the root manager for live data)');
  }
  $('refresh').onclick = refreshAll;
  $('saveTargets').onclick = saveTargets;
  $('clearTargets').onclick = () => { targets.clear(); renderTargets(); renderApps(); };
  $('clearConsole').onclick = () => { $('console').textContent = ''; };
  $('search').addEventListener('input', debounce(renderApps, 120));
  $('onlyRunning').onchange = renderApps;
  $('onlyTargets').onchange = renderApps;
  $('onlySystem').onchange = loadInstalled;

  await refreshAll();
});
