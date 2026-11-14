// The page's file logic (app/src/files.js) against a fake `pronao`, without a window or a browser:   node examples/excalidraw/test/test_files.mjs
import assert from 'node:assert';
import * as f from '../app/src/files.js';

let passed = 0, failed = 0;
async function check(name, fn) {
  try { await fn(); passed++; console.log('PASS ' + name); } catch (e) { failed++; console.log('FAIL ' + name + ': ' + (e && e.message || e)); }
}

function fake(picks = {}) {
  const calls = [];
  const disk = new Map();
  return {
    calls, disk,
    dialog: {
      open: async (a) => { calls.push(['dialog.open', a]); return picks.open === undefined ? { path: '/ok/a.excalidraw' } : picks.open; },
      save: async (a) => { calls.push(['dialog.save', a]); return picks.save === undefined ? { path: '/ok/new.excalidraw' } : picks.save; },
    },
    fs: {
      read: async (a) => { calls.push(['fs.read', a]); if (!disk.has(a.path)) throw { code: 'forbidden', message: 'no ' + a.path }; return { text: disk.get(a.path) }; },
      write: async (a) => { calls.push(['fs.write', a]); disk.set(a.path, a.text); return null; },
    },
    invoke: async (name, args) => { calls.push([name, args]); if (name === 'fs.writeBytes') disk.set(args.path, 'bytes:' + args.base64); return null; },
  };
}

await check('baseName takes the last part of either kind of path', () => {
  assert.strictEqual(f.baseName('C:\\Users\\me\\a.excalidraw'), 'a.excalidraw');
  assert.strictEqual(f.baseName('/home/me/a.excalidraw'), 'a.excalidraw');
  assert.strictEqual(f.baseName('plain'), 'plain');
});
await check('stripExtension', () => {
  assert.strictEqual(f.stripExtension('a.excalidraw'), 'a');
  assert.strictEqual(f.stripExtension('a.b.excalidraw'), 'a.b');
  assert.strictEqual(f.stripExtension('.hidden'), '.hidden');
  assert.strictEqual(f.stripExtension('noext'), 'noext');
});
await check('the window title shows the file and an unsaved mark', () => {
  assert.strictEqual(f.windowTitle(null, false), 'Untitled - Excalidraw');
  assert.strictEqual(f.windowTitle('/x/y.excalidraw', false), 'y.excalidraw - Excalidraw');
  assert.strictEqual(f.windowTitle('/x/y.excalidraw', true), 'y.excalidraw • - Excalidraw');
});
await check('opening asks with the drawing filters and reads exactly the picked file', async () => {
  const p = fake(); p.disk.set('/ok/a.excalidraw', '{"type":"excalidraw"}');
  const got = await f.openText(p, 'Open drawing');
  assert.deepStrictEqual(got, { path: '/ok/a.excalidraw', text: '{"type":"excalidraw"}' });
  assert.deepStrictEqual(p.calls[0], ['dialog.open', { title: 'Open drawing', filters: f.SCENE_FILTERS }]);
  assert.deepStrictEqual(p.calls[1], ['fs.read', { path: '/ok/a.excalidraw' }]);
});
await check('a cancelled open reads nothing', async () => {
  const p = fake({ open: null });
  assert.strictEqual(await f.openText(p, 'Open'), null);
  assert.strictEqual(p.calls.length, 1);
});
await check('a file the program refused is an error for the page to show, not a crash', async () => {
  const p = fake();                                       // nothing on the fake disk: fs.read rejects like a forbidden path
  await assert.rejects(f.openText(p, 'Open'), (e) => e.code === 'forbidden');
});
await check('save goes to the file the drawing came from, without asking', async () => {
  const p = fake();
  const path = await f.saveTo(p, '/ok/current.excalidraw', 'TEXT', 'x.excalidraw');
  assert.strictEqual(path, '/ok/current.excalidraw');
  assert.strictEqual(p.disk.get('/ok/current.excalidraw'), 'TEXT');
  assert.ok(!p.calls.some((c) => c[0] === 'dialog.save'));
});
await check('save with no file asks, proposing a name, and writes the picked path', async () => {
  const p = fake({ save: { path: '/ok/chosen.excalidraw' } });
  const path = await f.saveTo(p, null, 'TEXT', 'Untitled.excalidraw');
  assert.strictEqual(path, '/ok/chosen.excalidraw');
  assert.deepStrictEqual(p.calls[0], ['dialog.save', { title: 'Save drawing', defaultName: 'Untitled.excalidraw', filters: f.SCENE_FILTERS }]);
  assert.strictEqual(p.disk.get('/ok/chosen.excalidraw'), 'TEXT');
});
await check('a cancelled save writes nothing', async () => {
  const p = fake({ save: null });
  assert.strictEqual(await f.saveTo(p, null, 'TEXT', 'x'), null);
  assert.strictEqual(p.disk.size, 0);
});
await check('a blob is written as base64 through fs.writeBytes, bytes intact', async () => {
  const p = fake();
  const bytes = new Uint8Array([0, 255, 1, 137, 80, 78, 71]);
  await f.writeBlob(p, '/ok/out.png', new Blob([bytes], { type: 'image/png' }));
  assert.deepStrictEqual(p.calls[0], ['fs.writeBytes', { path: '/ok/out.png', base64: Buffer.from(bytes).toString('base64') }]);
});
await check('a large blob does not overflow the argument limit of the encoder', async () => {
  const big = new Uint8Array(1 << 20).map((_, i) => i & 255);
  assert.strictEqual(await f.base64Of(new Blob([big])), Buffer.from(big).toString('base64'));
});

console.log(passed + ' passed, ' + failed + ' failed');
process.exit(failed ? 1 : 0);
