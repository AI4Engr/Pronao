const { app, BrowserWindow, ipcMain } = require('electron');
const path = require('path');

ipcMain.handle('add', (e, a, b) => a + b);
ipcMain.handle('mean', (e, values) => {
  if (values.length === 0) throw new Error('invalid_argument: give at least one number');
  return values.reduce((x, y) => x + y, 0) / values.length;
});
ipcMain.handle('greet', (e, name) => 'hello, ' + name);
ipcMain.handle('fail', () => { throw new Error('this command always fails'); });

app.whenReady().then(() => {
  const win = new BrowserWindow({
    width: 640, height: 420, title: 'My app',
    webPreferences: { preload: path.join(__dirname, 'preload.js') }
  });
  win.loadFile('index.html');
});
app.on('window-all-closed', () => app.quit());
