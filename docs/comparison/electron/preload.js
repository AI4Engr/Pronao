const { contextBridge, ipcRenderer } = require('electron');
contextBridge.exposeInMainWorld('app', { invoke: (name, ...args) => ipcRenderer.invoke(name, ...args) });
