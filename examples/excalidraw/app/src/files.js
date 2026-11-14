// Everything the page does with files, in one place and free of React, so that it can be tested with a fake `pronao` (../test_files.mjs). Every function takes the
// `pronao` object it should use. The page never opens a file by itself: a file is read or written only after the user picked it in one of Pronao's dialogs, and the
// program lets the page reach exactly that file (a pick is a grant).

export const SCENE_FILTERS = [{ name: 'Excalidraw drawings', extensions: ['excalidraw', 'json'] }, { name: 'Everything' }];
export const PNG_FILTERS = [{ name: 'PNG image', extensions: ['png'] }];
export const SVG_FILTERS = [{ name: 'SVG image', extensions: ['svg'] }];

export function baseName(path) {
  const i = Math.max(path.lastIndexOf('/'), path.lastIndexOf(String.fromCharCode(92)));
  return path.slice(i + 1);
}

export function stripExtension(name) {
  const dot = name.lastIndexOf('.');
  return dot > 0 ? name.slice(0, dot) : name;
}

// The bytes of a Blob as base64 text (fs.writeBytes carries a binary file through JSON).
export async function base64Of(blob) {
  const bytes = new Uint8Array(await blob.arrayBuffer());
  let text = '';
  for (let i = 0; i < bytes.length; i += 0x8000) text += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
  return btoa(text);
}

// Asks the user for a drawing. null when the dialog is cancelled; otherwise {path, text}. A file that is not text (or too large) rejects with the program's own error.
export async function openText(pronao, title) {
  const picked = await pronao.dialog.open({ title, filters: SCENE_FILTERS });
  if (!picked) return null;
  const { text } = await pronao.fs.read({ path: picked.path });
  return { path: picked.path, text };
}

// Asks where to save. null when cancelled; otherwise the path (the file may not exist yet).
export async function chooseSavePath(pronao, title, defaultName, filters) {
  const picked = await pronao.dialog.save({ title, defaultName, filters });
  return picked ? picked.path : null;
}

export async function writeText(pronao, path, text) {
  await pronao.fs.write({ path, text });
}

export async function writeBlob(pronao, path, blob) {
  await pronao.invoke('fs.writeBytes', { path, base64: await base64Of(blob) });
}

// "Save": to the file the drawing came from or was last saved to, else ask. Returns the path written, or null if the user cancelled.
export async function saveTo(pronao, currentPath, text, defaultName) {
  const path = currentPath || (await chooseSavePath(pronao, 'Save drawing', defaultName, SCENE_FILTERS));
  if (!path) return null;
  await writeText(pronao, path, text);
  return path;
}

export function windowTitle(path, dirty) {
  return (path ? baseName(path) : 'Untitled') + (dirty ? ' •' : '') + ' - Excalidraw';
}
