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
