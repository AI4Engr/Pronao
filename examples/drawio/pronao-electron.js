// Gives the draw.io editor the `electron` object it expects, built on Pronao's commands. Written from what the editor calls (js/diagramly/ElectronApp.js,
// Apache-2.0); no code of drawio-desktop's shell (GPL-3.0) is used. Loaded before bootstrap.js, which decides desktop mode from the browser's user agent.
(function () {
  'use strict';
  var ua = navigator.userAgent;
  Object.defineProperty(navigator, 'userAgent', { get: function () { return ua + ' electron/32.0.0 draw.io/32.6.10'; } });

  // EditorUi.isElectronApp reads window.process.versions.electron. It is what turns off the browser's own file picker (nativeFileSupport) and the
  // second, online load of the script bundles, so the editor goes through the bridge below for every file.
  window.process = window.process || { versions: { electron: '32.0.0' }, platform: 'win32' };

  var listeners = {}, once = {};
  function emit(type, data) {
    (listeners[type] || []).forEach(function (fn) { fn(data); });
    var waiting = once[type] || []; once[type] = [];
    waiting.forEach(function (fn) { fn(data); });
  }
  var BACKSLASH = String.fromCharCode(92);

  function dirname(p) {
    var i = Math.max(p.lastIndexOf('/'), p.lastIndexOf(BACKSLASH));
    return i < 0 ? '' : p.slice(0, i);
  }
  function basename(p) {
    var i = Math.max(p.lastIndexOf('/'), p.lastIndexOf(BACKSLASH));
    return p.slice(i + 1);
  }
  // what the editor keeps to notice that a file changed on disk
  function statOf(path) {
    return pronao.invoke('fs.stat', { path: path }).then(function (e) { return { mtimeMs: e.modified, size: e.size }; });
  }
  function filtersOf(list) {
    return (list || []).map(function (f) {
      var ext = (f.extensions || []).filter(function (e) { return e !== '*'; });
      return ext.length ? { name: f.name, extensions: ext } : { name: f.name };
    });
  }

  var actions = {
    getDocumentsFolder: function () { return ''; },
    dirname: function (m) { return dirname(m.path); },
    isFullscreen: function () { return false; },
    getUpdateState: function () { return {}; },
    getLocalFonts: function () { return []; },
    getFileDrafts: function () { return []; },
    getBkpFile: function () { return null; },
    saveDraft: function () { return null; },
    watchFile: function () { return null; },
    unwatchFile: function () { return null; },
    deleteFile: function () { return null; },               // only ever used for drafts, which this program does not keep
    exit: function () { return pronao.invoke('window.close'); },
    openExternal: function (m) { return pronao.invoke('shell.openUrl', { url: m.url }); },
    clipboardAction: function (m) {
      if (m.method === 'writeText') return navigator.clipboard.writeText(m.data);
      if (m.method === 'readText') return navigator.clipboard.readText();
      return null;
    },
    showOpenDialog: function (m) {
      return pronao.dialog.open({ title: 'Open', filters: filtersOf(m.filters) }).then(function (r) { return r ? [r.path] : undefined; });
    },
    showSaveDialog: function (m) {
      return pronao.dialog.save({ title: 'Save', defaultName: basename(m.defaultPath || 'Untitled.drawio'), filters: filtersOf(m.filters) })
        .then(function (r) { return r ? r.path : null; });
    },
    fileStat: function (m) { return statOf(m.file); },
    isFileWritable: function () { return true; },
    readFile: function (m) {
      if (m.encoding === 'base64') return pronao.invoke('fs.readBytes', { path: m.filename }).then(function (r) { return r.base64; });
      return pronao.fs.read({ path: m.filename }).then(function (r) { return r.text; });
    },
    writeFile: function (m) {
      if (m.enc === 'base64') return pronao.invoke('fs.writeBytes', { path: m.path, base64: m.data });
      return pronao.fs.write({ path: m.path, text: m.data });
    },
    saveFile: function (m) {
      var path = m.fileObject.path;
      if (m.data == null || m.data === '') return Promise.reject(['empty data']);
      var check = (!m.overwrite && m.origStat) ? statOf(path).then(function (now) {
        if (now.mtimeMs !== m.origStat.mtimeMs) return Promise.reject(['conflict']);
      }, function () {}) : Promise.resolve();
      return check.then(function () {
        var enc = m.fileObject.type || m.defEnc || 'utf-8';
        return enc === 'base64' ? pronao.invoke('fs.writeBytes', { path: path, base64: m.data })
                                : pronao.fs.write({ path: path, text: m.data });
      }).then(function () { return statOf(path); });
    }
  };

  // Unsaved work: the editor tells the desktop shell through 'isModified' messages, which also stop any in-place text editing, so they cannot be polled.
  // Instead the file object's own setModified is wrapped once the editor has loaded, and the flag is handed to the window (app.confirmClose asks the user).
  var lastDirty = false, lastUi = null;
  function reportDirty() {
    var file = lastUi && lastUi.getCurrentFile && lastUi.getCurrentFile();
    var dirty = !!(file && file.isModified && file.isModified());
    if (dirty !== lastDirty) { lastDirty = dirty; pronao.invoke('window.setDirty', { dirty: dirty }).catch(function () {}); }
  }
  function watchModified() {
    if (!window.DrawioFile || !DrawioFile.prototype.setModified || !window.EditorUi) { setTimeout(watchModified, 300); return; }
    var originalInit = EditorUi.prototype.init;
    EditorUi.prototype.init = function () { lastUi = this; return originalInit.apply(this, arguments); };
    var original = DrawioFile.prototype.setModified;
    DrawioFile.prototype.setModified = function () {
      var result = original.apply(this, arguments);
      if (this.ui) { lastUi = this.ui; reportDirty(); }
      return result;
    };
    setInterval(reportDirty, 1000);                          // a file being switched or closed changes the answer without a setModified
  }
  window.addEventListener('load', watchModified);
  window.pronaoDraw = { ui: function () { return lastUi; } };     // read-only handle for the tests

  window.electron = {
    request: function (msg, ok, fail) {
      if (typeof msg === 'string') msg = { action: msg };
      var fn = actions[msg.action];
      var p;
      try { p = fn ? Promise.resolve(fn(msg)) : Promise.reject(['unsupported action: ' + msg.action]); } catch (e) { p = Promise.reject([String(e)]); }
      p.then(function (v) { if (ok) ok(v); }, function (e) {
        if (!fail) return;
        if (Array.isArray(e)) fail(e[0], e[1]);
        else fail(e && e.message ? e.message : String(e), e);
      });
    },
    // what the desktop shell tells the editor, and when: after the editor says it has loaded it is told to start with a new, unsaved document
    sendMessage: function (type) {
      if (type === 'app-load-finished') setTimeout(function () { emit('args-obj', { create: true }); }, 0);
    },
    registerMsgListener: function (type, fn) { (listeners[type] = listeners[type] || []).push(fn); },
    listenOnce: function (type, fn) { (once[type] = once[type] || []).push(fn); }
  };
})();
