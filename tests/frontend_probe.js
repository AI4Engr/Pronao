// Injected into every page of the frontend smoke test: records script errors from the start, and a few seconds after the page has loaded
// reports what the page looks like to the program.
(function () {
  window.__errors = [];
  window.addEventListener('error', function (e) { window.__errors.push(String(e.message) + ' @' + String(e.filename).slice(-60)); });
  window.addEventListener('unhandledrejection', function (e) { window.__errors.push('rejection: ' + String(e.reason)); });
  window.addEventListener('load', function () {
    setTimeout(function () {
      window.pronao.invoke('report', {
        path: location.pathname, origin: location.origin, tauri: window.isTauri === true, internals: typeof window.__TAURI_INTERNALS__,
        inputs: document.querySelectorAll('input').length, buttons: document.querySelectorAll('button').length,
        text: document.body.innerText.replace(/\s+/g, ' ').slice(0, 300), errors: window.__errors.slice(0, 10), scripts: document.scripts.length
      });
    }, 5000);
  });
})();
