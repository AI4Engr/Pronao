// pronao frontend bridge, run by the shim before any page script. pronao.invoke(command, args[, {signal}]) -> Promise (resolves with the command's JSON result, rejects
// with {code, message}); on / off / once for events pushed from CeeLing (App.emit, emitEvent); an event sent before the page registered its listener is dropped.
(function () {
  if (window.pronao) return;
  var listeners = {};
  var tags = 0;
  window.pronao = {
    // {signal}: aborting rejects at once with {code: "cancelled"} and asks CeeLing to stop the work (only async commands can be stopped).
    invoke: function (command, args, options) {
      var a = args === undefined ? null : args;
      var signal = options && options.signal;
      if (!signal) return window.__pronao_invoke(String(command), a);
      if (signal.aborted) return Promise.reject({ code: 'cancelled', message: 'aborted before it started' });
      var tag = 't' + (++tags) + '-' + Math.random().toString(36).slice(2, 10);
      var call = window.__pronao_invoke(String(command), a, tag);
      return new Promise(function (resolve, reject) {
        var onAbort = function () {
          reject({ code: 'cancelled', message: 'aborted' });
          window.__pronao_invoke('$cancel', tag);
        };
        signal.addEventListener('abort', onAbort, { once: true });
        call.then(function (v) { signal.removeEventListener('abort', onAbort); resolve(v); },
                  function (e) { signal.removeEventListener('abort', onAbort); reject(e); });
      });
    },
    on: function (name, fn) {
      name = String(name);
      (listeners[name] = listeners[name] || []).push(fn);
      return function () { window.pronao.off(name, fn); };
    },
    off: function (name, fn) {
      var list = listeners[String(name)];
      if (!list) return;
      var i = list.indexOf(fn);
      if (i >= 0) list.splice(i, 1);
    },
    once: function (name) {
      return new Promise(function (resolve) {
        var off = window.pronao.on(name, function (payload) { off(); resolve(payload); });
      });
    }
  };
  // Thin wrappers over the commands the application enables; one for a command that was not enabled rejects with {code: "unknown_command"} like any call.
  var call = function (name) { return function (args) { return window.pronao.invoke(name, args || {}); }; };
  window.pronao.fs = {
    read: call('fs.read'), write: call('fs.write'), list: call('fs.list'), stat: call('fs.stat'),
    exists: call('fs.exists'), mkdir: call('fs.mkdir'), remove: call('fs.remove'),
    watch: call('fs.watch'), unwatch: call('fs.unwatch'),
    // fn receives {watch, kind, path, from?}
    onChange: function (fn) { return window.pronao.on('fs.change', fn); }
  };
  window.pronao.dialog = {
    open: call('dialog.open'), save: call('dialog.save'), folder: call('dialog.folder'), message: call('dialog.message')
  };
  // a later launch of the program (App.claimInstance) hands over {cwd, args}; the window has already come to the front
  window.pronao.onSecondInstance = function (fn) { return window.pronao.on('app.secondInstance', fn); };
  // menu.popup({name}) resolves with the id of the item chosen, or null
  window.pronao.menu = { popup: function (name) { return window.pronao.invoke('menu.popup', { name: name }); } };
  // notify(title, text, {kind, id}) needs the application's permission; onNotificationClick(fn) gets the id of a notification this page showed when it is clicked
  window.pronao.notify = function (title, text, options) {
    var o = options || {};
    return window.pronao.invoke('notification.show', { title: title, text: text === undefined ? '' : text, kind: o.kind || 'info', id: o.id || '' });
  };
  window.pronao.onNotificationClick = function (fn) { return window.pronao.on('notification.click', function (e) { fn(e.id); }); };
  // theme: set(mode) needs the application's permission; onChange(fn) gets {mode, system} when the choice or the system's scheme changes
  window.pronao.theme = {
    get: call('theme.get'),
    set: function (mode) { return window.pronao.invoke('theme.set', { mode: mode }); },
    onChange: function (fn) { return window.pronao.on('theme.changed', fn); }
  };
  // files dragged from the file manager, when the application called enableFileDrop: onDrop(fn) gets {x, y, files: [{path, kind}], skipped}; onDragEnter / onDragOver
  // (about ten a second) get {x, y} in CSS pixels and onDragLeave when the drag left. A page that handles drops itself (preventDefault) is left alone.
  window.pronao.onDrop = function (fn) { return window.pronao.on('drop.files', fn); };
  window.pronao.onDragEnter = function (fn) { return window.pronao.on('drop.enter', fn); };
  window.pronao.onDragOver = function (fn) { return window.pronao.on('drop.over', fn); };
  window.pronao.onDragLeave = function (fn) { return window.pronao.on('drop.leave', fn); };
  (function () {
    var depth = 0, lastOver = 0;
    // (WebKitGTK lists a drag of files as 'text/uri-list', with no 'Files'; the program, which has the real paths, ignores a list that holds no file on this machine)
    var kind = function (t) { return Array.prototype.indexOf.call(t, 'Files') >= 0 || (typeof window.__pronao_drop_native === 'function' && Array.prototype.indexOf.call(t, 'text/uri-list') >= 0); };
    var files = function (e) { var t = e.dataTransfer && e.dataTransfer.types; return window.__pronao_drop === true && !e.defaultPrevented && !!t && kind(t); };
    var at = function (e) { return { x: Math.round(e.clientX), y: Math.round(e.clientY) }; };
    var say = function (name, payload) { window.__pronao_emit([name, payload]); };
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
      // the engine turns the File objects into paths for the program (the page never sees them)
      if (list && list.length && window.chrome && window.chrome.webview && window.chrome.webview.postMessageWithAdditionalObjects) {
        window.chrome.webview.postMessageWithAdditionalObjects('pronao.drop ' + p.x + ' ' + p.y, list);
      } else if (typeof window.__pronao_drop_native === 'function') {
        window.__pronao_drop_native(p.x, p.y);                 // (Linux: the program has the paths from the desktop's own drag data; this only says that the page saw the drop)
      }
    });
  })();
  window.pronao.proc = { run: call('proc.run') };
  // shell: only what the application allowed (schemes, file types, the folders of the file commands)
  // http.request {url, method?, headers?, body? | bodyBase64?, timeoutMs?, responseType?} -> {status, headers, body | bodyBase64}; sent by the program, not the page (src/http.clg)
  window.pronao.http = { request: call('http.request') };
  window.pronao.shell = { openUrl: call('shell.openUrl'), openFile: call('shell.openFile'), reveal: call('shell.reveal') };
  // settings: only the keys the application declared; "settings.changed" {key, value} reaches every window after a set
  window.pronao.settings = {
    get: function (key) { return window.pronao.invoke('settings.get', { key: key }); },
    getAll: call('settings.getAll'),
    set: function (key, value) { return window.pronao.invoke('settings.set', { key: key, value: value }); }
  };
  // layout (enableWindowLayoutCommands): a window with no title bar calls startDrag() on mousedown in its own title bar and startResize('bottomright') on its edges
  window.pronao.layout = {
    getBounds: call('window.getBounds'), setBounds: call('window.setBounds'), startDrag: call('window.startDrag'),
    startResize: function (edge) { return window.pronao.invoke('window.startResize', { edge: edge }); },
    setFullscreen: function (on) { return window.pronao.invoke('window.setFullscreen', { on: on !== false }); },
    setAlwaysOnTop: function (on) { return window.pronao.invoke('window.setAlwaysOnTop', { on: on !== false }); }
  };
  window.pronao.window = {
    setTitle: call('window.setTitle'), setSize: call('window.setSize'), minimize: call('window.minimize'),
    maximize: call('window.maximize'), restore: call('window.restore'), focus: call('window.focus'),
    close: call('window.close'), setDirty: call('window.setDirty'),
    // fires when the user tried to close the window with unsaved work and chose to stay (see app.confirmClose)
    onCloseDeclined: function (fn) { return window.pronao.on('window.closeDeclined', fn); },
    // fires with {url} when the window refused to load an address (a link, a redirect, a frame or window.open pointing elsewhere)
    onNavigationBlocked: function (fn) { return window.pronao.on('window.navigationBlocked', fn); }
  };
  // Called by the shim (app_eval) with ONE argument: the JSON array [name, payload] built by CeeLing. Pages never call it.
  window.__pronao_emit = function (message) {
    var list = listeners[String(message[0])];
    if (!list) return;
    list.slice().forEach(function (fn) {
      try { fn(message[1]); } catch (e) { console.error(e); }
    });
  };
})();
