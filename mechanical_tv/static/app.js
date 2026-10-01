'use strict';
const $ = id => document.getElementById(id);
let signedIn = false, currentId = null, lastLibrary = '', polling = false, xhr = null;
const clock = n => `${Math.floor(n / 60)}:${String(Math.floor(n % 60)).padStart(2, '0')}`;
const gib = n => `${(n / 1024 ** 3).toFixed(1)} GiB`;
function notice(text) { $('notice').textContent = text; }
function authUI(active) { signedIn = active; $('login').hidden = active; $('studio').hidden = !active; }
async function api(path, body) {
  const options = body === undefined ? {} : {method:'POST', headers:{'Content-Type':'application/json','X-MTV-Request':'1'}, body:JSON.stringify(body)};
  const response = await fetch(path, options);
  const data = await response.json();
  if (!response.ok) {
    if (response.status === 401) authUI(false);
    throw new Error(data.error || 'Request failed');
  }
  return data;
}
function tab(name) {
  document.querySelectorAll('.page').forEach(page => page.hidden = page.id !== name);
  document.querySelectorAll('[data-tab]').forEach(button => button.classList.toggle('active', button.dataset.tab === name));
  if (name === 'system') refreshHealth().catch(error => notice(error.message));
}
document.querySelectorAll('[data-tab]').forEach(button => button.addEventListener('click', () => tab(button.dataset.tab)));
$('login-form').addEventListener('submit', async event => {
  event.preventDefault();
  try { await api('/api/login', {password:$('password').value}); $('password').value = ''; authUI(true); notice('Connected. Hardware output is disabled.'); await refresh(); await refreshHealth(); }
  catch(error) { notice(error.message); }
});
$('logout').addEventListener('click', async () => { try { await api('/api/logout', {}); $('source').pause(); authUI(false); } catch(error) { notice(error.message); } });
async function command(action, value) {
  try { await api('/api/player', {action, value}); await refresh(); }
  catch(error) { notice(error.message); }
}
['play','pause','stop'].forEach(action => $(action).addEventListener('click', () => command(action)));
$('loop').addEventListener('change', () => command('loop', $('loop').checked));
$('seek').addEventListener('change', () => command('seek', Number($('seek').value)));
$('brightness').addEventListener('input', () => $('brightness-value').textContent = `${$('brightness').value}%`);
$('brightness').addEventListener('change', () => command('brightness', Number($('brightness').value) / 100));
function paint(player) {
  const ctx = $('pixels').getContext('2d');
  const frame = ctx.createImageData(32, 25);
  for (let i = 0; i < 800; i++) {
    const v = Math.round((player.pixels[i] || 0) * player.brightness);
    frame.data.set([v,v,v,255], i * 4);
  }
  ctx.putImageData(frame, 0, 0);
}
function renderLibrary(items) {
  const key = JSON.stringify(items);
  if (key === lastLibrary) return;
  lastLibrary = key;
  $('library-list').replaceChildren();
  if (!items.length) { const p = document.createElement('p'); p.textContent = 'Your library is empty. Upload a clip to get started.'; $('library-list').append(p); }
  for (const item of items) {
    const row = document.createElement('article'); row.className = 'library-item';
    const info = document.createElement('div');
    const title = document.createElement('h3'); title.textContent = item.name;
    const detail = document.createElement('p'); detail.textContent = `${item.status.toUpperCase()} · ${item.fit} ${item.duration ? '· ' + clock(item.duration) : ''}${item.error ? ' — ' + item.error : ''}`;
    info.append(title, detail);
    const buttons = document.createElement('div'); buttons.className = 'buttons';
    const select = document.createElement('button'); select.textContent = 'Select'; select.disabled = item.status !== 'ready';
    select.addEventListener('click', async () => { await command('select', item.id); tab('player'); });
    const remove = document.createElement('button'); remove.textContent = 'Delete'; remove.disabled = ['uploading','queued','preparing'].includes(item.status);
    remove.addEventListener('click', async () => {
      if (!confirm(`Delete “${item.name}” and its prepared files?`)) return;
      try { await api('/api/delete', {id:item.id}); await refresh(); } catch(error) { notice(error.message); }
    });
    buttons.append(select, remove); row.append(info, buttons); $('library-list').append(row);
  }
}
async function refresh() {
  if (polling) return;
  polling = true;
  try {
    const data = await api('/api/state');
    authUI(true);
    const p = data.player;
    $('connection').textContent = '● Connected';
    $('play-state').textContent = p.state.toUpperCase();
    $('selected-name').textContent = data.library.find(item => item.id === p.id)?.name || 'Select a video to begin.';
    $('position').textContent = clock(p.position); $('duration').textContent = clock(p.duration);
    if (document.activeElement !== $('seek')) { $('seek').max = Math.max(0, p.duration - .1); $('seek').value = p.position; }
    if (document.activeElement !== $('brightness')) { $('brightness').value = Math.round(p.brightness * 100); $('brightness-value').textContent = `${Math.round(p.brightness * 100)}%`; }
    $('loop').checked = p.loop;
    $('play').disabled = !p.id; $('pause').disabled = p.state !== 'playing'; $('seek').disabled = !p.id;
    if (currentId !== p.id) { currentId = p.id; if (p.id) $('source').src = `/api/preview/${p.id}`; else { $('source').removeAttribute('src'); $('source').load(); } }
    $('source-empty').hidden = Boolean(p.id);
    if (p.id && $('source').readyState >= 1) {
      if (Math.abs($('source').currentTime - p.position) > .4) $('source').currentTime = p.position;
      if (p.state === 'playing') $('source').play().catch(() => {}); else $('source').pause();
    }
    paint(p); renderLibrary(data.library);
  } catch(error) {
    $('connection').textContent = '○ Reconnecting…';
    $('source').pause();
    if (signedIn) notice(`Connection unavailable: ${error.message}. The Pi may still be playing.`);
  } finally { polling = false; }
}
async function refreshHealth() {
  const health = await api('/api/health');
  $('storage').textContent = `${gib(health.disk_free_bytes)} free`;
  const metrics = [['Output','Simulation'],['Version',health.version],['Conversion worker',health.worker_alive ? 'Running' : 'Unavailable'],['Free storage',gib(health.disk_free_bytes)],['Temperature',health.temperature_c === null ? 'Unavailable' : `${health.temperature_c.toFixed(1)} °C`],['Uptime',clock(health.uptime_seconds)],['FFmpeg',health.ffmpeg ? 'Available' : 'Missing'],['FFprobe',health.ffprobe ? 'Available' : 'Missing'],['Hardware telemetry','Not connected']];
  $('health').replaceChildren();
  for (const [label,value] of metrics) { const card=document.createElement('article'); card.className='card metric'; const title=document.createElement('h3');title.textContent=label;const p=document.createElement('p');p.textContent=value;card.append(title,p);$('health').append(card); }
}
$('upload-form').addEventListener('submit', event => {
  event.preventDefault();
  const file=$('file').files[0]; if (!file) return;
  if (file.size > 256 * 1024 ** 2) { notice('Maximum upload size is 256 MiB.'); return; }
  xhr=new XMLHttpRequest(); xhr.open('POST', `/api/upload?name=${encodeURIComponent(file.name)}&fit=${$('fit').value}`); xhr.setRequestHeader('X-MTV-Request','1');
  $('upload-button').disabled=true; $('cancel-upload').hidden=false; $('upload-progress').hidden=false; $('upload-progress').value=0;
  xhr.upload.onprogress=event => { if(event.lengthComputable) { const percent=Math.round(event.loaded/event.total*100);$('upload-progress').value=percent;$('upload-status').textContent=percent < 100 ? `Uploading ${percent}%…` : 'Upload sent. Waiting for the Pi…'; } };
  xhr.onload=() => { try { const data=JSON.parse(xhr.responseText); if(xhr.status >= 400) throw new Error(data.error);$('upload-status').textContent='Upload complete. Preparation is queued; watch the library status below.';$('file').value='';refresh(); } catch(error) { notice(error.message);$('upload-status').textContent='Upload failed. Please try again.'; } };
  xhr.onerror=() => { notice('Upload connection failed. Check the connection and try again.');$('upload-status').textContent='Upload failed.'; };
  xhr.onabort=() => $('upload-status').textContent='Upload canceled. Any incomplete entry can be deleted after cleanup.';
  xhr.onloadend=() => { $('upload-button').disabled=false;$('cancel-upload').hidden=true; xhr=null; };
  xhr.send(file);
});
$('cancel-upload').addEventListener('click', () => xhr?.abort());
refresh().then(() => { if(signedIn) refreshHealth().catch(() => {}); });
setInterval(() => { if(signedIn) refresh(); }, 150);
setInterval(() => { if(signedIn) refreshHealth().catch(() => {}); }, 10000);
