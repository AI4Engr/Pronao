// app-cling frontend bridge. The shim runs this before any page script (webview "init" script).
//   cling.invoke(command, args)  ->  Promise
// The promise resolves with the command's JSON result, or rejects with {code, message}.
(function () {
  if (window.cling) return;
  window.cling = {
    invoke: function (command, args) {
      return window.__cling_invoke(String(command), args === undefined ? null : args);
    }
  };
})();
