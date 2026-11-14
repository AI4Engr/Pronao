// Where the component finds its fonts: the program's own site, never a CDN. This module is imported first (before the component), because the component
// reads the setting when it first loads a font.
window.EXCALIDRAW_ASSET_PATH = window.location.origin + '/';
