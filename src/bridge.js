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
  window.cling = {
    invoke: function (command, args) {
      return window.__cling_invoke(String(command), args === undefined ? null : args);
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
