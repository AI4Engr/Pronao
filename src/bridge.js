// app-cling frontend bridge. The shim runs this before any page script (webview "init" script).
//   cling.invoke(command, args)  ->  Promise
//       resolves with the command's JSON result, or rejects with {code, message}.
//   cling.on(name, fn)  ->  function that removes the listener
//   cling.off(name, fn)
//   cling.once(name)    ->  Promise for the next event called `name`
//       events are pushed from CeeLing (app.emit / emitEvent); fn receives the event's JSON payload.
//       An event sent before the page has loaded and registered its listener is dropped.
(function () {
  if (window.cling) return;
  var listeners = {};
  var tags = 0;
  window.cling = {
    // invoke(command, args, {signal}): with an AbortSignal, aborting it rejects the promise at once with {code: "cancelled"} and tells the
    // program to stop the work (a program the command started is killed). Only async commands can be stopped; others simply finish.
    invoke: function (command, args, options) {
      var a = args === undefined ? null : args;
      var signal = options && options.signal;
      if (!signal) return window.__cling_invoke(String(command), a);
      if (signal.aborted) return Promise.reject({ code: 'cancelled', message: 'aborted before it started' });
      var tag = 't' + (++tags) + '-' + Math.random().toString(36).slice(2, 10);
      var call = window.__cling_invoke(String(command), a, tag);
      return new Promise(function (resolve, reject) {
        var onAbort = function () {
          reject({ code: 'cancelled', message: 'aborted' });
          window.__cling_invoke('$cancel', tag);
        };
        signal.addEventListener('abort', onAbort, { once: true });
        call.then(function (v) { signal.removeEventListener('abort', onAbort); resolve(v); },
                  function (e) { signal.removeEventListener('abort', onAbort); reject(e); });
      });
    },
    on: function (name, fn) {
      name = String(name);
      (listeners[name] = listeners[name] || []).push(fn);
      return function () { window.cling.off(name, fn); };
    },
    off: function (name, fn) {
      var list = listeners[String(name)];
      if (!list) return;
      var i = list.indexOf(fn);
      if (i >= 0) list.splice(i, 1);
    },
    once: function (name) {
      return new Promise(function (resolve) {
        var off = window.cling.on(name, function (payload) { off(); resolve(payload); });
      });
    }
  };
  // Thin wrappers over the commands the application enables (app.enableFileCommands / enableDialogCommands). A
  // wrapper for a command that was not enabled rejects with {code: "unknown_command"} like any other call.
  var call = function (name) { return function (args) { return window.cling.invoke(name, args || {}); }; };
  window.cling.fs = {
    read: call('fs.read'), write: call('fs.write'), list: call('fs.list'), stat: call('fs.stat'),
    exists: call('fs.exists'), mkdir: call('fs.mkdir'), remove: call('fs.remove'),
    watch: call('fs.watch'), unwatch: call('fs.unwatch'),
    // fn receives {watch, kind, path, from?}; returns the function that removes the listener
    onChange: function (fn) { return window.cling.on('fs.change', fn); }
  };
  window.cling.dialog = {
    open: call('dialog.open'), save: call('dialog.save'), folder: call('dialog.folder'), message: call('dialog.message')
  };
  // a later launch of the program (single instance, App.claimInstance) hands over {cwd, args}; the window has already come to the front
  window.cling.onSecondInstance = function (fn) { return window.cling.on('app.secondInstance', fn); };
  // menu.popup({name}) shows a menu the application registered under that name at the mouse and resolves with the id of the item chosen, or null
  window.cling.menu = { popup: function (name) { return window.cling.invoke('menu.popup', { name: name }); } };
  // notify(title, text, {kind: 'info' | 'warning' | 'error', id}) shows a system notification if the application allowed it (notification.show); onNotificationClick(fn) hears of
  // a click on one this page showed, with the id it gave
  window.cling.notify = function (title, text, options) {
    var o = options || {};
    return window.cling.invoke('notification.show', { title: title, text: text === undefined ? '' : text, kind: o.kind || 'info', id: o.id || '' });
  };
  window.cling.onNotificationClick = function (fn) { return window.cling.on('notification.click', function (e) { fn(e.id); }); };
  // theme: get() -> {mode: 'system' | 'light' | 'dark', system: 'light' | 'dark' | 'unknown'}, set(mode) if the application allowed it; onChange(fn) hears {mode, system} when the
  // choice or the system's colour scheme changes (the CSS media query prefers-color-scheme follows the engine by itself)
  window.cling.theme = {
    get: call('theme.get'),
    set: function (mode) { return window.cling.invoke('theme.set', { mode: mode }); },
    onChange: function (fn) { return window.cling.on('theme.changed', fn); }
  };
  // files dragged from the file manager onto the window, when the application called enableFileDrop: onDrop(fn) gets {x, y, files: [{path, kind}], skipped} and the files can then be read
  // with cling.fs under those paths. While a drag of files is over the window, onDragEnter(fn) and onDragOver(fn) (about ten a second) get {x, y} (CSS pixels) and onDragLeave(fn) hears that
  // the drag left or ended. A page that handles drops itself (it calls preventDefault in its own dragover / drop handler) is left alone: nothing is sent for those.
  window.cling.onDrop = function (fn) { return window.cling.on('drop.files', fn); };
  window.cling.onDragEnter = function (fn) { return window.cling.on('drop.enter', fn); };
  window.cling.onDragOver = function (fn) { return window.cling.on('drop.over', fn); };
  window.cling.onDragLeave = function (fn) { return window.cling.on('drop.leave', fn); };
  (function () {
    var depth = 0, lastOver = 0;
    var files = function (e) { var t = e.dataTransfer && e.dataTransfer.types; return window.__cling_drop === true && !e.defaultPrevented && !!t && Array.prototype.indexOf.call(t, 'Files') >= 0; };
    var at = function (e) { return { x: Math.round(e.clientX), y: Math.round(e.clientY) }; };
    var say = function (name, payload) { window.__cling_emit([name, payload]); };
    window.addEventListener('dragenter', function (e) {
      if (files(e) && depth++ === 0) say('drop.enter', at(e));
    });
    window.addEventListener('dragover', function (e) {
      if (!files(e)) return;
      e.preventDefault();                                     // the engine then takes the drop instead of trying to open the file in the window
      e.dataTransfer.dropEffect = 'copy';
      var now = Date.now();
      if (now - lastOver >= 100) { lastOver = now; say('drop.over', at(e)); }
    });
    window.addEventListener('dragleave', function (e) {
      if (files(e) && depth > 0 && --depth === 0) say('drop.leave', {});
    });
    window.addEventListener('drop', function (e) {
      if (!files(e)) return;
      e.preventDefault();
      if (depth > 0) { depth = 0; say('drop.leave', {}); }
      var list = e.dataTransfer.files;
      var p = at(e);
      // the engine turns the File objects into paths for the program (the page never sees them); the program decides what becomes of them
      if (list && list.length && window.chrome && window.chrome.webview && window.chrome.webview.postMessageWithAdditionalObjects) {
        window.chrome.webview.postMessageWithAdditionalObjects('cling.drop ' + p.x + ' ' + p.y, list);
      }
    });
  })();
  window.cling.proc = { run: call('proc.run') };
  // shell: openUrl({url}) opens an address in the default browser, openFile({path}) a file in its default program, reveal({path}) shows a file in the file
  // manager. Only what the application allowed (schemes, file types, the folders of the file commands).
  window.cling.shell = { openUrl: call('shell.openUrl'), openFile: call('shell.openFile'), reveal: call('shell.reveal') };
  // settings: only the keys the application declared. get(key) -> value, getAll() -> {key: value}, set(key, value) -> null; the event
  // "settings.changed" {key, value} reaches every window after a set.
  window.cling.settings = {
    get: function (key) { return window.cling.invoke('settings.get', { key: key }); },
    getAll: call('settings.getAll'),
    set: function (key, value) { return window.cling.invoke('settings.set', { key: key, value: value }); }
  };
  // layout, when the application registered window.getBounds / setBounds / startDrag / startResize (enableWindowLayoutCommands): for a window with no title bar the page calls
  // startDrag() in a mousedown handler on its own title bar and startResize('bottomright') on the edges; setFullscreen / setAlwaysOnTop exist only if the application asked for them
  window.cling.layout = {
    getBounds: call('window.getBounds'), setBounds: call('window.setBounds'), startDrag: call('window.startDrag'),
    startResize: function (edge) { return window.cling.invoke('window.startResize', { edge: edge }); },
    setFullscreen: function (on) { return window.cling.invoke('window.setFullscreen', { on: on !== false }); },
    setAlwaysOnTop: function (on) { return window.cling.invoke('window.setAlwaysOnTop', { on: on !== false }); }
  };
  window.cling.window = {
    setTitle: call('window.setTitle'), setSize: call('window.setSize'), minimize: call('window.minimize'),
    maximize: call('window.maximize'), restore: call('window.restore'), focus: call('window.focus'),
    close: call('window.close'), setDirty: call('window.setDirty'),
    // fires when the user tried to close the window with unsaved work and chose to stay (see app.confirmClose)
    onCloseDeclined: function (fn) { return window.cling.on('window.closeDeclined', fn); },
    // fires with {url} when the window refused to load an address (a link, a redirect, a frame or window.open pointing elsewhere)
    onNavigationBlocked: function (fn) { return window.cling.on('window.navigationBlocked', fn); }
  };
  // Called by the shim (app_eval) with ONE argument: the JSON array [name, payload] that CeeLing's JSON printer built.
  // The page never needs to call this itself.
  window.__cling_emit = function (message) {
    var list = listeners[String(message[0])];
    if (!list) return;
    list.slice().forEach(function (fn) {
      try { fn(message[1]); } catch (e) { console.error(e); }
    });
  };
})();
