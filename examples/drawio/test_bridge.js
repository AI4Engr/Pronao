// Tests pronao-electron.js without a window: a fake `pronao` stands in for the commands and a fake editor for draw.io.
//   node examples/drawio/test_bridge.js
'use strict';
const assert = require('assert');
const fs = require('fs');
const path = require('path');

let passed = 0, failed = 0;
async function check(name, fn) {
  try { await fn(); passed++; console.log('PASS ' + name); }
  catch (e) { failed++; console.log('FAIL ' + name + ': ' + (e && e.message || e)); }
}

function setup() {
  const calls = [];
  const files = new Map();           // path -> {text, base64, modified}
  let clock = 1000;
  const refuse = (p) => { if (!p.startsWith('/ok/')) throw { code: 'forbidden', message: 'cannot access ' + p }; };
  const invoke = async (name, args) => {
    calls.push([name, args]);
    if (name === 'fs.stat') { refuse(args.path); const f = files.get(args.path); if (!f) throw { code: 'not_found', message: 'no ' + args.path }; return { name: 'x', kind: 'file', size: (f.text || '').length, modified: f.modified }; }
    if (name === 'fs.write') { refuse(args.path); files.set(args.path, { text: args.text, modified: ++clock }); return null; }
    if (name === 'fs.read') { refuse(args.path); const f = files.get(args.path); if (!f) throw { code: 'not_found', message: 'no ' + args.path }; return { text: f.text }; }
    if (name === 'fs.writeBytes') { refuse(args.path); files.set(args.path, { base64: args.base64, modified: ++clock }); return null; }
    if (name === 'fs.readBytes') { refuse(args.path); return { base64: files.get(args.path).base64 }; }
    if (name === 'shell.openUrl') return null;
    if (name === 'window.close') return null;
    if (name === 'window.setDirty') return null;
    throw { code: 'unknown_command', message: name };
  };
  const picks = { open: { path: '/ok/a.drawio' }, save: { path: '/ok/b.drawio' } };
  const pronao = {
    invoke,
    fs: { read: (a) => invoke('fs.read', a), write: (a) => invoke('fs.write', a) },
    dialog: { open: async (a) => { calls.push(['dialog.open', a]); return picks.open; }, save: async (a) => { calls.push(['dialog.save', a]); return picks.save; } }
  };
  const win = { navigator: { userAgent: 'Mozilla/5.0 Edg/130' }, pronao, setTimeout, setInterval: () => 0, addEventListener: () => {} };
  win.window = win;
  const code = fs.readFileSync(path.join(__dirname, 'pronao-electron.js'), 'utf8');
  new Function('window', 'navigator', 'pronao', 'setTimeout', 'setInterval', code)(win, win.navigator, pronao, setTimeout, win.setInterval);
  const request = (msg) => new Promise((ok, fail) => win.electron.request(msg, ok, (m, e) => fail(new Error(m))));
  return { win, calls, files, picks, request };
}

(async () => {
  await check('the editor is told it runs in the desktop app', async () => {
    const { win } = setup();
    assert.ok(win.navigator.userAgent.includes(' electron/') && win.navigator.userAgent.includes(' draw.io/'));
    assert.ok(win.navigator.userAgent.startsWith('Mozilla/5.0 Edg/130'));
  });
  await check('the editor finds the desktop app by window.process.versions.electron', async () => {
    const { win } = setup();
    assert.ok(win.process && win.process.versions && win.process.versions.electron);
  });
  await check('dirname', async () => {
    const { request } = setup();
    assert.strictEqual(await request({ action: 'dirname', path: 'C:\\a\\b\\c.drawio' }), 'C:\\a\\b');
    assert.strictEqual(await request({ action: 'dirname', path: '/a/b/c.drawio' }), '/a/b');
  });
  await check('open dialog answers a list with the picked path, and undefined on cancel', async () => {
    const { request, picks, calls } = setup();
    assert.deepStrictEqual(await request({ action: 'showOpenDialog', filters: [{ name: 'Diagrams', extensions: ['drawio', 'xml'] }, { name: 'All', extensions: ['*'] }] }), ['/ok/a.drawio']);
    assert.deepStrictEqual(calls.find((c) => c[0] === 'dialog.open')[1].filters, [{ name: 'Diagrams', extensions: ['drawio', 'xml'] }, { name: 'All' }]);
    picks.open = null;
    assert.strictEqual(await request({ action: 'showOpenDialog' }), undefined);
  });
  await check('save dialog proposes the file name and answers the path', async () => {
    const { request, calls } = setup();
    assert.strictEqual(await request({ action: 'showSaveDialog', defaultPath: 'C:\\docs\\Untitled.drawio' }), '/ok/b.drawio');
    assert.strictEqual(calls.find((c) => c[0] === 'dialog.save')[1].defaultName, 'Untitled.drawio');
  });
  await check('text and binary files round-trip', async () => {
    const { request } = setup();
    await request({ action: 'writeFile', path: '/ok/a.drawio', data: '<mxfile/>', enc: 'utf-8' });
    assert.strictEqual(await request({ action: 'readFile', filename: '/ok/a.drawio', encoding: 'utf-8' }), '<mxfile/>');
    await request({ action: 'writeFile', path: '/ok/p.png', data: 'iVBORw0KGgo=', enc: 'base64' });
    assert.strictEqual(await request({ action: 'readFile', filename: '/ok/p.png', encoding: 'base64' }), 'iVBORw0KGgo=');
  });
  await check('a path the program did not grant is refused, with the reason', async () => {
    const { request } = setup();
    await assert.rejects(request({ action: 'readFile', filename: '/etc/passwd', encoding: 'utf-8' }), /forbidden|cannot access/);
  });
  await check('save returns a new stat and a changed file is a conflict', async () => {
    const { request } = setup();
    await request({ action: 'writeFile', path: '/ok/a.drawio', data: 'v1', enc: 'utf-8' });
    const stat1 = await request({ action: 'fileStat', file: '/ok/a.drawio' });
    const stat2 = await request({ action: 'saveFile', fileObject: { path: '/ok/a.drawio', type: 'utf-8' }, data: 'v2', origStat: stat1, overwrite: false });
    assert.notStrictEqual(stat2.mtimeMs, stat1.mtimeMs);
    await assert.rejects(request({ action: 'saveFile', fileObject: { path: '/ok/a.drawio', type: 'utf-8' }, data: 'v3', origStat: stat1, overwrite: false }), /conflict/);
    await request({ action: 'saveFile', fileObject: { path: '/ok/a.drawio', type: 'utf-8' }, data: 'v3', origStat: stat1, overwrite: true });
    assert.strictEqual(await request({ action: 'readFile', filename: '/ok/a.drawio', encoding: 'utf-8' }), 'v3');
  });
  await check('saving nothing is refused', async () => {
    const { request } = setup();
    await assert.rejects(request({ action: 'saveFile', fileObject: { path: '/ok/a.drawio' }, data: '', overwrite: true }), /empty data/);
  });
  await check('links go to the shell command; exit closes the window', async () => {
    const { request, calls } = setup();
    await request({ action: 'openExternal', url: 'https://example.com' });
    await request({ action: 'exit' });
    assert.deepStrictEqual(calls.filter((c) => c[0] === 'shell.openUrl' || c[0] === 'window.close').map((c) => c[0]), ['shell.openUrl', 'window.close']);
  });
  await check('actions the program does not support answer harmlessly, others are refused', async () => {
    const { request } = setup();
    assert.deepStrictEqual(await request({ action: 'getFileDrafts' }), []);
    assert.strictEqual(await request({ action: 'getBkpFile' }), null);
    await assert.rejects(request({ action: 'launchMissiles' }), /unsupported action/);
  });
  await check('the editor is told to start with a new document once it has loaded', async () => {
    const { win } = setup();
    let got = null;
    win.electron.registerMsgListener('args-obj', (a) => { got = a; });
    win.electron.sendMessage('app-load-finished', null);
    await new Promise((r) => setTimeout(r, 10));
    assert.deepStrictEqual(got, { create: true });
  });
  console.log(passed + ' passed, ' + failed + ' failed');
  process.exit(failed ? 1 : 0);
})();
