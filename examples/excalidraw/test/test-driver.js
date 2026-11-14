// TEST ONLY (loaded by test/run_window_test.py into a copy of the site, never into the program that is shipped). Drives the page through its own menus and keys, with the
// two dialogs replaced by fixed paths, and records what it saw in <folder>/steps.log through the program's own file commands. The folder is the one the test program allowed.
(function () {
  const dir = window.__TEST_DIR__;                       // written into the copy of the site by the runner
  const log = [];
  const titles = [], dirty = [];
  const realInvoke = pronao.invoke.bind(pronao);
  pronao.invoke = function (name, args, options) {
    if (name === 'window.setTitle') titles.push(args.title);
    if (name === 'window.setDirty') dirty.push(args.dirty);
    return realInvoke(name, args, options);
  };
  // the dialogs answer with files in the allowed folder, as a user's pick would
  const answers = {};
  pronao.dialog.open = async () => ({ path: dir + '/in.excalidraw' });
  pronao.dialog.save = async (a) => ({ path: dir + '/' + (answers[a.title] || 'unexpected-' + a.title.replace(/\W/g, '_')) });
  answers['Export PNG'] = 'out.png';
  answers['Export SVG'] = 'out.svg';
  answers['Save drawing'] = 'unexpected-save.excalidraw';

  const last = () => String(dirty[dirty.length - 1]);          // 'true', 'false', or 'undefined' when it was never changed
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  async function until(what, fn, ms = 15000) {
    const t0 = Date.now();
    for (;;) {
      const v = fn();
      if (v) return v;
      if (Date.now() - t0 > ms) throw new Error('timed out waiting for ' + what);
      await sleep(50);
    }
  }
  function note(s) { log.push(s); }
  async function openMenuAndClick(label) {
    const trigger = await until('the menu button', () => document.querySelector('[data-testid="main-menu-trigger"]'));
    trigger.click();
    const item = await until('the menu item ' + label, () => [...document.querySelectorAll('.dropdown-menu-item')].find((b) => b.textContent.trim().startsWith(label)));
    item.click();
  }
  function key(k, opts) {
    const ev = new KeyboardEvent('keydown', Object.assign({ key: k, bubbles: true, cancelable: true }, opts || {}));
    document.dispatchEvent(ev);
  }

  async function run() {
    await until('the editor', () => document.querySelector('.excalidraw'));
    await sleep(800);
    note('start title=' + titles[titles.length - 1] + ' dirty=' + last());

    await openMenuAndClick('Open');
    await until('the title of the opened file', () => titles.some((t) => t.startsWith('in.excalidraw')));
    await sleep(500);
    note('opened title=' + titles[titles.length - 1] + ' dirty=' + last());
    if (window.__TEST_SHOT__) { await sleep(2500); note('SHOT_READY'); return; }          // for the README picture: stop here, the runner reads the window

    await openMenuAndClick('Export PNG');
    await sleep(2500);
    await openMenuAndClick('Export SVG');
    await sleep(1500);
    note('exported');

    // an edit: select everything and delete it
    key('a', { ctrlKey: true });
    await sleep(200);
    key('Delete');
    await until('the unsaved mark', () => dirty.length && dirty[dirty.length - 1] === true);
    note('edited title=' + titles[titles.length - 1] + ' dirty=' + last());

    key('s', { ctrlKey: true });                         // Save: back to in.excalidraw, the file it came from
    await until('the saved mark', () => dirty[dirty.length - 1] === false);
    await sleep(500);
    note('saved title=' + titles[titles.length - 1] + ' dirty=' + last());

    answers['Save drawing'] = 'copy.excalidraw';
    key('S', { ctrlKey: true, shiftKey: true });         // Save as: asks, answered with copy.excalidraw
    await until('the title of the copy', () => titles.some((t) => t.startsWith('copy.excalidraw')));
    note('saved as title=' + titles[titles.length - 1] + ' dirty=' + last());
    // nothing but the program's own address can be reached: the browser reports each refusal as a policy violation, which tells it from a missing network
    const blocked = [];
    document.addEventListener('securitypolicyviolation', (e) => blocked.push(e.blockedURI));
    try { await fetch('https://example.com/'); note('external fetch ALLOWED'); } catch (e) { /* refused */ }
    const img = new Image(); img.src = 'https://example.com/x.png';
    const script = document.createElement('script'); script.src = 'https://example.com/x.js'; document.head.appendChild(script);
    await sleep(800);
    note('csp blocked ' + blocked.filter((u) => u.indexOf('example.com') >= 0).length + ' of 3 external requests');
    // the page may reach the picked folder and nothing around it
    try { await pronao.fs.read({ path: dir + '/../outside.txt' }); note('outside READ'); } catch (e) { note('outside refused ' + e.code); }
    note('DONE');
  }
  run().catch((e) => note('ERROR ' + e.message)).finally(async () => {
    note('titles=' + JSON.stringify(titles));
    await realInvoke('fs.write', { path: dir + '/steps.log', text: log.join('\n') + '\n' });
  });
})();
