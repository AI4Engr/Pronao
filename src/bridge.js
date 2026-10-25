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
