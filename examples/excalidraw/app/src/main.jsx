import './asset-path.js';
import React, { useCallback, useEffect, useRef, useState } from 'react';
import { createRoot } from 'react-dom/client';
import { Excalidraw, MainMenu, exportToBlob, exportToSvg, getSceneVersion, loadFromBlob, serializeAsJSON } from '@excalidraw/excalidraw';
import '@excalidraw/excalidraw/index.css';
import * as files from './files.js';

// No default way of reaching a file is left on: the component's own "open" and "save" would use the browser's file picker, which the program does not give this
// page; its image tool would open a file chooser too. Opening, saving and exporting are the three commands below, and each goes through Pronao.
const UI_OPTIONS = {
  canvasActions: { loadScene: false, saveToActiveFile: false, saveAsImage: false, export: false, clearCanvas: true, toggleTheme: true, changeViewBackgroundColor: true },
  tools: { image: false },
};

function App() {
  const apiRef = useRef(null);
  const pathRef = useRef(null);             // the file the drawing was opened from or last saved to
  const savedVersion = useRef(0);           // getSceneVersion at the last open or save
  const dirtyRef = useRef(false);
  const [message, setMessage] = useState('');

  const show = (text) => { setMessage(text); setTimeout(() => setMessage((m) => (m === text ? '' : m)), 4000); };
  const fail = (e) => show((e && e.code ? e.code + ': ' : '') + ((e && e.message) || String(e)));

  const refreshTitle = useCallback(() => {
    pronao.invoke('window.setTitle', { title: files.windowTitle(pathRef.current, dirtyRef.current) }).catch(() => {});
  }, []);

  const setDirty = useCallback((dirty) => {
    if (dirtyRef.current === dirty) return;
    dirtyRef.current = dirty;
    pronao.invoke('window.setDirty', { dirty }).catch(() => {});      // the program asks before the window closes with unsaved work
    refreshTitle();
  }, [refreshTitle]);

  const onChange = useCallback((elements) => {
    setDirty(getSceneVersion(elements) !== savedVersion.current);
  }, [setDirty]);

  const sceneText = () => {
    const api = apiRef.current;
    return serializeAsJSON(api.getSceneElements(), api.getAppState(), api.getFiles(), 'local');
  };

  const markSaved = (path) => {
    pathRef.current = path;
    // the same list onChange is given: it keeps the deleted elements, which count (deleting is an edit); getSceneElements() would leave them out and never match
    savedVersion.current = getSceneVersion(apiRef.current.getSceneElementsIncludingDeleted());
    dirtyRef.current = true;               // forces the update below to run
    setDirty(false);
    refreshTitle();
  };

  const openDrawing = useCallback(async () => {
    try {
      const got = await files.openText(pronao, 'Open drawing');
      if (!got) return;
      const loaded = await loadFromBlob(new Blob([got.text], { type: 'application/json' }), null, null);
      const api = apiRef.current;
      api.updateScene({ elements: loaded.elements, appState: { ...loaded.appState, viewModeEnabled: false }, captureUpdate: 'IMMEDIATELY' });
      api.addFiles(Object.values(loaded.files || {}));
      api.history.clear();
      api.scrollToContent(loaded.elements, { fitToContent: true });
      markSaved(got.path);
      show('Opened ' + files.baseName(got.path));
    } catch (e) { fail(e); }
  }, []);

  const saveDrawing = useCallback(async (asNew) => {
    try {
      const name = files.stripExtension(pathRef.current ? files.baseName(pathRef.current) : 'Untitled') + '.excalidraw';
      const path = await files.saveTo(pronao, asNew ? null : pathRef.current, sceneText(), name);
      if (!path) return;
      markSaved(path);
      show('Saved ' + files.baseName(path));
    } catch (e) { fail(e); }
  }, []);

  const exportImage = useCallback(async (kind) => {
    try {
      const api = apiRef.current;
      const elements = api.getSceneElements();
      if (!elements.length) { show('Nothing to export'); return; }
      const base = files.stripExtension(pathRef.current ? files.baseName(pathRef.current) : 'Untitled');
      const appState = { ...api.getAppState(), exportBackground: true };
      if (kind === 'png') {
        const path = await files.chooseSavePath(pronao, 'Export PNG', base + '.png', files.PNG_FILTERS);
        if (!path) return;
        await files.writeBlob(pronao, path, await exportToBlob({ elements, appState, files: api.getFiles(), mimeType: 'image/png' }));
        show('Exported ' + files.baseName(path));
      } else {
        const path = await files.chooseSavePath(pronao, 'Export SVG', base + '.svg', files.SVG_FILTERS);
        if (!path) return;
        const svg = await exportToSvg({ elements, appState, files: api.getFiles() });
        await files.writeText(pronao, path, new XMLSerializer().serializeToString(svg));
        show('Exported ' + files.baseName(path));
      }
    } catch (e) { fail(e); }
  }, []);

  useEffect(() => {
    const onKey = (e) => {
      if (!(e.ctrlKey || e.metaKey) || e.altKey) return;
      const k = e.key.toLowerCase();
      if (k === 's') { e.preventDefault(); saveDrawing(e.shiftKey); }
      else if (k === 'o') { e.preventDefault(); openDrawing(); }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [openDrawing, saveDrawing]);

  useEffect(() => { refreshTitle(); }, [refreshTitle]);

  return (
    <div style={{ height: '100%' }}>
      <Excalidraw excalidrawAPI={(api) => { apiRef.current = api; }} onChange={onChange} UIOptions={UI_OPTIONS} handleKeyboardGlobally>
        <MainMenu>
          <MainMenu.Item onSelect={openDrawing} shortcut="Ctrl+O">Open...</MainMenu.Item>
          <MainMenu.Item onSelect={() => saveDrawing(false)} shortcut="Ctrl+S">Save</MainMenu.Item>
          <MainMenu.Item onSelect={() => saveDrawing(true)} shortcut="Ctrl+Shift+S">Save as...</MainMenu.Item>
          <MainMenu.Separator />
          <MainMenu.Item onSelect={() => exportImage('png')}>Export PNG...</MainMenu.Item>
          <MainMenu.Item onSelect={() => exportImage('svg')}>Export SVG...</MainMenu.Item>
          <MainMenu.Separator />
          <MainMenu.DefaultItems.ClearCanvas />
          <MainMenu.DefaultItems.ToggleTheme />
          <MainMenu.DefaultItems.ChangeCanvasBackground />
          <MainMenu.DefaultItems.Help />
        </MainMenu>
      </Excalidraw>
      {message && (
        <div style={{ position: 'fixed', left: '50%', bottom: 18, transform: 'translateX(-50%)', background: 'rgba(30,30,40,.92)', color: '#fff', padding: '8px 16px', borderRadius: 10, font: '14px system-ui, sans-serif', zIndex: 10 }}>{message}</div>
      )}
    </div>
  );
}

createRoot(document.getElementById('root')).render(<App />);
