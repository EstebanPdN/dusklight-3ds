const { contextBridge, ipcRenderer, webUtils } = require('electron')
contextBridge.exposeInMainWorld('converter', {
  ports: () => ipcRenderer.invoke('ports'),
  choose: id => ipcRenderer.invoke('choose', id),
  drop: (id, file) => ipcRenderer.send('drop', id, file ? webUtils.getPathForFile(file) : ''),
  reveal: () => ipcRenderer.send('reveal'),
  onStatus: callback => ipcRenderer.on('status', (_, update) => callback(update))
})
