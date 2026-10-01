'use strict';

const $ = id => document.getElementById(id);
let signedIn = false;
let currentLibraryId = null;
let lastLibrary = '';
let polling = false;
let xhr = null;

const clock = n => `${Math.floor(n / 60)}:${String(Math.floor(n % 60)).padStart(2, '0')}`;
const gib = n => `${(n / 1024 ** 3).toFixed(1)} GiB`;

function notice(text) {
  $('notice').textContent = text;
}

function authUI(active) {
  signedIn = active;
  $('login').hidden = active;
  $('studio').hidden = !active;
}

async function api(path, body) {
  const options = body === undefined ? {} : {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', 'X-MTV-Request': '1' },
    body: JSON.stringify(body)
  };
  const response = await fetch(path, options);
  const data = await response.json();
  if (!response.ok) {
    if (response.status === 401) authUI(false);
    throw new Error(data.error || 'Request failed');
  }
  return data;
}

async function rtControl(action, params = {}) {
  try {
    const res = await api('/api/realtime/control', { action, ...params });
    await refresh();
    return res;
  } catch (error) {
    notice(error.message);
  }
}

function tab(name) {
  document.querySelectorAll('.page').forEach(page => page.hidden = page.id !== name);
  document.querySelectorAll('[data-tab]').forEach(button => button.classList.toggle('active', button.dataset.tab === name));
  if (name === 'system') refreshHealth().catch(error => notice(error.message));
}

document.querySelectorAll('[data-tab]').forEach(button => button.addEventListener('click', () => tab(button.dataset.tab)));

// Authentication
$('login-form').addEventListener('submit', async event => {
  event.preventDefault();
  try {
    await api('/api/login', { password: $('password').value });
    $('password').value = '';
    authUI(true);
    notice('Connected to Mechanical TV studio.');
    await refresh();
    await refreshHealth();
  } catch (error) {
    notice(error.message);
  }
});

$('logout').addEventListener('click', async () => {
  try {
    await api('/api/logout', {});
    $('source').pause();
    authUI(false);
  } catch (error) {
    notice(error.message);
  }
});

// Real-time Motor & Blank Controls
$('rt-start').addEventListener('click', () => rtControl('start'));
$('rt-stop').addEventListener('click', () => rtControl('stop'));
$('rt-blank').addEventListener('click', () => rtControl('emergency_blank'));

// Speed Control
$('rt-speed').addEventListener('input', () => {
  const fps = Number($('rt-speed').value);
  $('rt-speed-val').textContent = `${fps.toFixed(1)} FPS (${Math.round(fps * 60)} RPM)`;
});
$('rt-speed').addEventListener('change', () => {
  rtControl('speed', { fps: Number($('rt-speed').value) });
});

// Brightness Control
$('rt-bright').addEventListener('input', () => {
  $('rt-bright-val').textContent = `${$('rt-bright').value}%`;
});
$('rt-bright').addEventListener('change', () => {
  rtControl('calibration', { brightness: Number($('rt-bright').value) / 100 });
});

// Source Switcher
$('video-source').addEventListener('change', () => {
  const val = $('video-source').value;
  if (val === 'hdmi') {
    rtControl('source', { mode: 'hdmi' });
    $('stream-name').textContent = 'Live HDMI Capture';
    $('source-status-text').textContent = 'HDMI /dev/video0';
    $('source-empty').hidden = true;
  } else if (val === 'library') {
    if (!currentLibraryId) {
      notice('Select a video from the Media Library first.');
      tab('library');
      return;
    }
    rtControl('source', { mode: 'library', id: currentLibraryId });
    $('stream-name').textContent = 'Library Video Clip';
    $('source-status-text').textContent = 'MEDIA FILE LOOP';
  } else {
    // Pattern
    rtControl('source', { mode: 'pattern' });
    rtControl('pattern', { pattern: val });
    $('stream-name').textContent = `Test Pattern: ${val}`;
    $('source-status-text').textContent = 'PROCEDURAL PATTERN';
    $('source-empty').hidden = true;
  }
});

// Optical Calibration Controls
$('cal-phase').addEventListener('input', () => {
  const v = Number($('cal-phase').value);
  const deg = Math.round(v * 360 / 800);
  $('cal-phase-val').textContent = `${v} px (${deg}°)`;
});
$('cal-phase').addEventListener('change', () => {
  rtControl('calibration', { phase_offset: Number($('cal-phase').value) });
});

$('cal-gamma').addEventListener('input', () => {
  $('cal-gamma-val').textContent = $('cal-gamma').value;
});
$('cal-gamma').addEventListener('change', () => {
  rtControl('calibration', { gamma: Number($('cal-gamma').value) });
});

$('cal-contrast').addEventListener('input', () => {
  $('cal-contrast-val').textContent = $('cal-contrast').value;
});
$('cal-contrast').addEventListener('change', () => {
  rtControl('calibration', { contrast: Number($('cal-contrast').value) });
});

$('cal-inv-x').addEventListener('change', () => {
  rtControl('calibration', { invert_x: $('cal-inv-x').checked });
});
$('cal-inv-y').addEventListener('change', () => {
  rtControl('calibration', { invert_y: $('cal-inv-y').checked });
});

// Canvas Painting for Mechanical TV Output
function paint(pixels, brightness = 1.0) {
  const canvas = $('pixels');
  const ctx = canvas.getContext('2d');
  const frame = ctx.createImageData(32, 25);
  const data = frame.data;
  const count = pixels ? pixels.length : 0;

  for (let i = 0; i < 800; i++) {
    const raw = i < count ? pixels[i] : 0;
    const v = Math.max(0, min(255, Math.round(raw * brightness)));
    const idx = i * 4;
    // Phosphor green/amber tint for vintage optical feel
    data[idx] = v;          // R
    data[idx + 1] = v;      // G
    data[idx + 2] = v;      // B
    data[idx + 3] = 255;    // Alpha
  }
  ctx.putImageData(frame, 0, 0);

  // Also draw to source canvas if in pattern mode
  const srcCanvas = $('source-canvas');
  if (srcCanvas && !$('video-source').value.startsWith('hdmi') && $('video-source').value !== 'library') {
    srcCanvas.style.display = 'block';
    $('source').style.display = 'none';
    const sCtx = srcCanvas.getContext('2d');
    sCtx.putImageData(frame, 0, 0);
  } else {
    if (srcCanvas) srcCanvas.style.display = 'none';
    $('source').style.display = 'block';
  }
}

function min(a, b) {
  return a < b ? a : b;
}

// Media Library Rendering
function renderLibrary(items) {
  const key = JSON.stringify(items);
  if (key === lastLibrary) return;
  lastLibrary = key;
  $('library-list').replaceChildren();

  if (!items.length) {
    const p = document.createElement('p');
    p.textContent = 'Your library is empty. Upload a clip to get started.';
    $('library-list').append(p);
  }

  for (const item of items) {
    const row = document.createElement('article');
    row.className = 'library-item';
    const info = document.createElement('div');
    const title = document.createElement('h3');
    title.textContent = item.name;
    const detail = document.createElement('p');
    detail.textContent = `${item.status.toUpperCase()} · ${item.fit} ${item.duration ? '· ' + clock(item.duration) : ''}${item.error ? ' — ' + item.error : ''}`;
    info.append(title, detail);

    const buttons = document.createElement('div');
    buttons.className = 'buttons';

    const select = document.createElement('button');
    select.textContent = 'Select';
    select.disabled = item.status !== 'ready';
    select.addEventListener('click', async () => {
      currentLibraryId = item.id;
      $('video-source').value = 'library';
      await command('select', item.id);
      await rtControl('source', { mode: 'library', id: item.id });
      $('stream-name').textContent = item.name;
      $('source').src = `/api/preview/${item.id}`;
      $('source-empty').hidden = true;
      tab('realtime');
    });

    const remove = document.createElement('button');
    remove.textContent = 'Delete';
    remove.disabled = ['uploading', 'queued', 'preparing'].includes(item.status);
    remove.addEventListener('click', async () => {
      if (!confirm(`Delete “${item.name}” and its prepared files?`)) return;
      try {
        await api('/api/delete', { id: item.id });
        if (currentLibraryId === item.id) currentLibraryId = null;
        await refresh();
      } catch (error) {
        notice(error.message);
      }
    });

    buttons.append(select, remove);
    row.append(info, buttons);
    $('library-list').append(row);
  }
}

// Media clip playback commands
async function command(action, value) {
  try {
    await api('/api/player', { action, value });
    if (action === 'play') await rtControl('start');
    if (action === 'stop') await rtControl('stop');
    await refresh();
  } catch (error) {
    notice(error.message);
  }
}

['play', 'pause', 'stop'].forEach(action => {
  const btn = $(action);
  if (btn) btn.addEventListener('click', () => command(action));
});
$('loop').addEventListener('change', () => command('loop', $('loop').checked));
$('seek').addEventListener('change', () => command('seek', Number($('seek').value)));

// Main Periodic Refresh Loop
async function refresh() {
  if (polling) return;
  polling = true;
  try {
    const data = await api('/api/state');
    authUI(true);

    const p = data.player;
    const rt = data.realtime;

    $('connection').textContent = '● Connected';

    // Update Motor & Sync Status Pills
    const motorState = rt ? rt.state : p.state;
    $('motor-pill').textContent = `MOTOR: ${motorState.toUpperCase()}`;
    $('motor-pill').className = `pill ${motorState}`;

    const isLocked = rt ? rt.sync_locked : false;
    $('sync-pill').textContent = `SYNC: ${isLocked ? 'LOCKED' : 'UNLOCKED'}`;
    $('sync-pill').className = `pill ${isLocked ? 'locked' : (motorState === 'idle' ? 'idle' : 'ramping')}`;

    // Update Player & Timeline state
    if (p) {
      $('play-state').textContent = p.state.toUpperCase();
      $('position').textContent = clock(p.position);
      $('duration').textContent = clock(p.duration);
      if (document.activeElement !== $('seek')) {
        $('seek').max = Math.max(0, p.duration - 0.1);
        $('seek').value = p.position;
      }
      $('loop').checked = Boolean(p.loop);
      $('play').disabled = !p.id;
      $('pause').disabled = p.state !== 'playing';
      $('seek').disabled = !p.id;

      if (p.id && $('source').readyState >= 1) {
        if (Math.abs($('source').currentTime - p.position) > 0.4) {
          $('source').currentTime = p.position;
        }
        if (p.state === 'playing') $('source').play().catch(() => {});
        else $('source').pause();
      }
    }

    // Update Telemetry HUD
    if (rt) {
      $('telem-target-rpm').textContent = `${Math.round(rt.target_rpm)} RPM (${rt.target_fps} FPS)`;
      $('telem-measured-rpm').textContent = `${rt.measured_rpm.toFixed(1)} RPM`;
      $('telem-lock').textContent = isLocked ? 'LOCKED (±0.4%)' : (motorState === 'idle' ? 'STANDBY' : 'ACQUIRING');
      $('telem-jitter').textContent = `${rt.jitter_ms.toFixed(2)} ms`;

      const dutyPct = rt.led && rt.led.average_duty_cycle ? (rt.led.average_duty_cycle * 100).toFixed(1) : '0.0';
      $('telem-duty').textContent = `${dutyPct}%`;

      const watchdogStatus = rt.led && rt.led.safety_tripped ? 'TRIPPED (OFF)' : (motorState === 'idle' ? 'ARMED' : 'ACTIVE');
      $('telem-watchdog').textContent = watchdogStatus;

      // Sync calibration controls if not focused
      if (rt.pipeline) {
        const pipe = rt.pipeline;
        if (document.activeElement !== $('cal-phase')) {
          $('cal-phase').value = pipe.phase_offset || 0;
          $('cal-phase-val').textContent = `${pipe.phase_offset} px (${Math.round(pipe.phase_offset * 360 / 800)}°)`;
        }
        if (document.activeElement !== $('cal-gamma')) {
          $('cal-gamma').value = pipe.gamma || 1.8;
          $('cal-gamma-val').textContent = pipe.gamma || 1.8;
        }
        if (document.activeElement !== $('cal-contrast')) {
          $('cal-contrast').value = pipe.contrast || 1.0;
          $('cal-contrast-val').textContent = pipe.contrast || 1.0;
        }
        if (document.activeElement !== $('cal-inv-x')) $('cal-inv-x').checked = Boolean(pipe.invert_x);
        if (document.activeElement !== $('cal-inv-y')) $('cal-inv-y').checked = Boolean(pipe.invert_y);
        if (document.activeElement !== $('rt-bright')) {
          const bVal = Math.round((pipe.brightness || 0.7) * 100);
          $('rt-bright').value = bVal;
          $('rt-bright-val').textContent = `${bVal}%`;
        }
      }
    }

    // Paint the TV Screen
    paint(p.pixels, p.brightness);
    renderLibrary(data.library);

  } catch (error) {
    $('connection').textContent = '○ Reconnecting…';
    if (signedIn) notice(`Connection unavailable: ${error.message}`);
  } finally {
    polling = false;
  }
}

// Health and System Diagnostics
async function refreshHealth() {
  const health = await api('/api/health');
  $('storage').textContent = `${gib(health.disk_free_bytes)} free`;

  const rt = health.realtime || {};
  const metrics = [
    ['Output Mode', health.mode === 'realtime' ? 'Realtime Hardware' : 'Realtime Simulation'],
    ['Conversion worker', health.worker_alive ? 'Running' : 'Unavailable'],
    ['Target Speed', `${rt.target_rpm || 600} RPM (${rt.target_fps || 10} FPS)`],
    ['Measured Speed', `${(rt.measured_rpm || 0).toFixed(1)} RPM`],
    ['Sync Lock', rt.sync_locked ? 'Locked' : 'Acquiring / Idle'],
    ['Opto Jitter', `${(rt.jitter_ms || 0).toFixed(2)} ms`],
    ['LED Interlock', rt.led && rt.led.safety_tripped ? 'Tripped (Stall)' : 'Armed'],
    ['Free Storage', gib(health.disk_free_bytes)],
    ['Core Temp', health.temperature_c === null ? 'Unavailable' : `${health.temperature_c.toFixed(1)} °C`],
    ['Uptime', clock(health.uptime_seconds)],
    ['FFmpeg', health.ffmpeg ? 'Available' : 'Missing'],
    ['FFprobe', health.ffprobe ? 'Available' : 'Missing'],
  ];

  $('health').replaceChildren();
  for (const [label, value] of metrics) {
    const card = document.createElement('article');
    card.className = 'card metric';
    const title = document.createElement('h3');
    title.textContent = label;
    const p = document.createElement('p');
    p.textContent = value;
    card.append(title, p);
    $('health').append(card);
  }
}

// Upload Handling
$('upload-form').addEventListener('submit', event => {
  event.preventDefault();
  const file = $('file').files[0];
  if (!file) return;
  if (file.size > 256 * 1024 ** 2) {
    notice('Maximum upload size is 256 MiB.');
    return;
  }
  xhr = new XMLHttpRequest();
  xhr.open('POST', `/api/upload?name=${encodeURIComponent(file.name)}&fit=${$('fit').value}`);
  xhr.setRequestHeader('X-MTV-Request', '1');

  $('upload-button').disabled = true;
  $('cancel-upload').hidden = false;
  $('upload-progress').hidden = false;
  $('upload-progress').value = 0;

  xhr.upload.onprogress = event => {
    if (event.lengthComputable) {
      const percent = Math.round(event.loaded / event.total * 100);
      $('upload-progress').value = percent;
      $('upload-status').textContent = percent < 100 ? `Uploading ${percent}%…` : 'Upload sent. Preparing video…';
    }
  };

  xhr.onload = () => {
    try {
      const data = JSON.parse(xhr.responseText);
      if (xhr.status >= 400) throw new Error(data.error);
      $('upload-status').textContent = 'Upload complete. Conversion queued; view in library.';
      $('file').value = '';
      refresh();
    } catch (error) {
      notice(error.message);
      $('upload-status').textContent = 'Upload failed. Please try again.';
    }
  };

  xhr.onerror = () => {
    notice('Upload connection failed. Check Wi-Fi and try again.');
    $('upload-status').textContent = 'Upload failed.';
  };

  xhr.onabort = () => {
    $('upload-status').textContent = 'Upload canceled.';
  };

  xhr.onloadend = () => {
    $('upload-button').disabled = false;
    $('cancel-upload').hidden = true;
    xhr = null;
  };

  xhr.send(file);
});

$('cancel-upload').addEventListener('click', () => xhr?.abort());

// Initialize Polling
refresh().then(() => {
  if (signedIn) refreshHealth().catch(() => {});
});
setInterval(() => {
  if (signedIn) refresh();
}, 150);
setInterval(() => {
  if (signedIn) refreshHealth().catch(() => {});
}, 8000);
