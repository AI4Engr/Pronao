import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

// One bundle for the page; the fonts of the Excalidraw package are copied next to it by ../fetch.sh (the component looks for them under EXCALIDRAW_ASSET_PATH).
export default defineConfig({
  plugins: [react()],
  define: { 'process.env.IS_PREACT': JSON.stringify('false') },
  build: { outDir: 'dist', emptyOutDir: true, sourcemap: false, chunkSizeWarningLimit: 5000 },
});
