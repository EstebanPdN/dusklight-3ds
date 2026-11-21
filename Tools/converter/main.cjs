const { app, BrowserWindow, dialog, ipcMain, Menu, shell } = require('electron')
const { spawn } = require('node:child_process')
const path = require('node:path')
const { createInterface } = require('node:readline')

const ports = require('./ports.json')
app.commandLine.appendSwitch('lang', 'en-US')
let window, running = false, resultPath
const backend = path.join(process.resourcesPath, 'backend')
const python = path.join(backend, 'python', process.platform === 'win32' ? 'python.exe' : 'bin/python3')

function send(update) {
  window.webContents.send('status', update)
}

function readableError(detail, port) {
  if (/already exists|File exists|WinError (80|183)|appeared during/i.test(detail))
    return port.output + ' already exists next to the input. Move it before trying again.'
  if (/Requires GameCube/i.test(detail)) return 'Use a USA or PAL GameCube ISO, revision 0.'
  if (/space left|disk full|WinError 112/i.test(detail)) return 'The drive is out of free space.'
  if (/Permission|Access is denied|read-only/i.test(detail)) return 'Check the folder’s read and write permissions.'
  if (/not found|No such file/i.test(detail)) return 'A required file could not be found.'
  if (/^\[(Errno|WinError)/.test(detail)) return 'The file could not be read or written. Check the drive and folder.'
  return detail.slice(0, 180)
}

function convert(id, source) {
  if (running || !Object.hasOwn(ports, id)) return
  const port = ports[id]
  resultPath = undefined
  if (typeof source !== 'string' || !path.isAbsolute(source) ||
      !port.extensions.includes(path.extname(source).slice(1).toLowerCase())) {
    send({error:'Choose one file (' + port.extensions.map(ext => '.' + ext).join(', ') + ').'})
    return
  }
  running = true
  const output = path.join(path.dirname(source), port.output)
  send({phase:'Checking the input file', percent:0, name:path.basename(source)})
  const args = id === 'dusklight'
    ? [path.join(backend, 'twilight_pack.py'), '--input', source, '--output', output]
    : [path.join(backend, 'cartridge.py'), id, source, output]
  const child = spawn(python, ['-IB', ...args, '--progress-json'],
    {windowsHide:true, stdio:['ignore','pipe','pipe']})
  let failure = '', complete, packed = false, lastPercent = -1, lastLabel
  const phases = {
    inspect:[0,0,'Checking the disc index'], pack:[20,35,'Packing game data'],
    verify:[55,20,'Verifying the archive'], readback:[75,15,'Comparing the original files']
  }
  child.stderr.on('data', data => { failure = (failure + data).slice(-512) })
  child.once('error', error => { failure = error.message })
  createInterface({input:child.stdout}).on('line', line => {
    let event
    try { event = JSON.parse(line) } catch { return }
    if (event.event === 'complete') { complete = event; return }
    if (event.event === 'error') { failure = event.message; return }
    if (event.phase === 'pack') packed = true
    if (event.phase === 'cartridge') { send({phase:event.message, percent:event.completed}); return }
    const phase = event.phase === 'source-hash'
      ? (packed ? [90,9,'Checking the final ISO checksum'] : [0,20,'Reading the ISO'])
      : phases[event.phase]
    if (!phase) return
    const [base, width, label] = phase
    const percent = base + Math.floor(width * event.completed / Math.max(1,event.total))
    if (lastPercent === percent && lastLabel === label) return
    lastPercent = percent
    lastLabel = label
    send({phase:label, percent})
  })
  child.once('close', code => {
    running = false
    if (code === 0 && complete?.output === output) {
      resultPath = output
      send({done:true, bytes:complete.bytes})
    } else send({error:readableError(failure || 'The conversion process stopped unexpectedly.', port)})
  })
}

app.whenReady().then(() => {
  Menu.setApplicationMenu(null)
  window = new BrowserWindow({width:530,height:480,useContentSize:true,resizable:false,
    maximizable:false,fullscreenable:false,backgroundColor:'#181915',title:'Forge Store · 3DS ROM Converter',
    icon:path.join(__dirname,'ui','icon.png'),
    webPreferences:{preload:path.join(__dirname,'preload.cjs'),sandbox:true,contextIsolation:true}})
  window.on('close', event => {
    if (running) { event.preventDefault(); send({closing:true}) }
  })
  ipcMain.handle('ports', () => ports)
  ipcMain.handle('choose', async (_, id) => {
    if (running || !Object.hasOwn(ports, id)) return
    const port = ports[id]
    const choice = await dialog.showOpenDialog(window,{properties:['openFile'],
      title:'Choose ' + port.name + ' game data', buttonLabel:'Open',
      filters:[{name:port.system + ' game image',extensions:port.extensions}]})
    if (!choice.canceled) convert(id, choice.filePaths[0])
  })
  ipcMain.on('drop', (_, id, file) => convert(id, file))
  ipcMain.on('reveal', () => { if (resultPath) shell.showItemInFolder(resultPath) })
  window.webContents.on('will-navigate', event => event.preventDefault())
  window.webContents.setWindowOpenHandler(() => ({action:'deny'}))
  window.loadFile(path.join(__dirname,'ui','index.html'))
})
app.on('window-all-closed', () => app.quit())
