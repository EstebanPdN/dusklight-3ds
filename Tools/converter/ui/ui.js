const el = id => document.getElementById(id)
const api = window.converter
let ports, selected, busy = false, done = false

function preview(id) {
  const port = ports[id]
  document.body.dataset.port = id
  el('art').src = port.art
  el('logo').src = port.logo
  el('logo').alt = port.name
  el('route').textContent = port.system + ' → Nintendo 3DS'
  for (const tile of el('games').children) tile.classList.toggle('active', tile.dataset.id === id)
}

function openPort(id) {
  if (busy) return
  selected = id
  done = false
  preview(id)
  const port = ports[id]
  el('library').hidden = true
  el('detail').hidden = false
  el('format').textContent = port.input + ' TO 3DS GAME DATA'
  el('heading').textContent = 'Drop your ' + port.input + ' here'
  el('note').textContent = port.support
  el('choose').textContent = 'Choose ' + port.input
  el('choose').hidden = false
  el('status').textContent = 'Saved beside your input file'
  el('value').textContent = port.output
  el('bar').hidden = true
  el('choose').focus()
}

api.ports().then(data => {
  ports = data
  for (const [id, port] of Object.entries(ports)) {
    const tile = document.createElement('button')
    const icon = document.createElement('img'), name = document.createElement('b'), system = document.createElement('span')
    tile.className = 'tile'
    tile.dataset.id = id
    icon.src = 'art/' + id + '.webp'
    icon.alt = ''
    name.textContent = port.name
    system.textContent = port.system
    tile.append(icon, name, system)
    tile.onpointerenter = tile.onfocus = () => preview(id)
    tile.onclick = () => openPort(id)
    el('games').append(tile)
  }
  preview('dusklight')
})

el('back').onclick = () => {
  if (busy) return
  const id = selected
  selected = undefined
  el('detail').hidden = true
  el('library').hidden = false
  el('games').querySelector('[data-id="' + id + '"]').focus()
}
el('choose').onclick = () => { if (done) api.reveal(); else api.choose(selected) }
for (const type of ['dragenter','dragover','dragleave','drop']) {
  window.addEventListener(type, event => {
    event.preventDefault()
    if (busy || !selected) return
    el('zone').classList.toggle('over', type === 'dragenter' || type === 'dragover')
    if (type === 'drop') api.drop(selected, event.dataTransfer.files.length === 1 ? event.dataTransfer.files[0] : null)
  })
}

api.onStatus(event => {
  if (!selected) return
  if (event.closing) { el('note').textContent = 'You can close this window when conversion finishes.'; return }
  const port = ports[selected]
  busy = !event.error && !event.done
  done = Boolean(event.done)
  el('back').disabled = busy
  el('choose').hidden = busy
  el('bar').hidden = !busy && !done
  if (busy) {
    el('heading').textContent = 'Preparing your game'
    if (event.name) el('note').textContent = event.name
    el('status').textContent = event.phase
    el('value').textContent = event.percent + '%'
    el('bar').value = event.percent
  } else {
    el('heading').textContent = done ? 'Ready for your SD card' : 'Check your input'
    el('note').textContent = done ? 'Copy ' + port.output + ' to SD:' + port.sd + ' ' + (port.extra || '') : event.error
    el('status').textContent = done ? 'Game data verified' : port.support
    el('value').textContent = done ? '100%' : ''
    el('choose').textContent = done ? 'Show in folder' : 'Choose ' + port.input
    el('bar').value = done ? 100 : 0
  }
})
