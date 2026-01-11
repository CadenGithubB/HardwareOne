#ifndef WEBPAGE_MAPS_H
#define WEBPAGE_MAPS_H

#include "WebServer_Utils.h"
#include "System_GPSMapRenderer.h"

// Streamed inner content for maps page
inline void streamMapsInner(httpd_req_t* req) {
  // Include shared file browser scripts
  String fbScript = getFileBrowserScript();
  httpd_resp_send_chunk(req, fbScript.c_str(), fbScript.length());
  
  httpd_resp_send_chunk(req, R"HTML(
<h2>Map Viewer</h2>
<p>View GPS maps uploaded to the device. Upload .hwmap files using the file browser below.</p>

<div style='display:flex;gap:1rem;margin:1rem 0;flex-wrap:wrap'>
  <div style='flex:1;min-width:300px'>
    <div style='background:var(--panel-bg);padding:1rem;border-radius:8px;border:1px solid var(--border);margin-bottom:1rem'>
      <div style='display:flex;justify-content:space-between;align-items:center;margin-bottom:0.5rem'>
        <h3 style='margin:0;color:var(--panel-fg)'>Maps</h3>
        <button class='btn' onclick='uploadMapFile()'>Upload .hwmap</button>
      </div>
      <div id='maps-file-browser'></div>
    </div>
    <div id='map-info' style='background:var(--panel-bg);padding:1rem;border-radius:8px;border:1px solid var(--border);display:none'>
      <h3 style='margin:0 0 0.5rem 0;color:var(--panel-fg)'>Map Info</h3>
      <div id='map-details'></div>
    </div>
    <div id='map-features' style='background:var(--panel-bg);padding:1rem;border-radius:8px;border:1px solid var(--border);margin-top:1rem;display:none'>
      <h3 style='margin:0 0 0.5rem 0;color:var(--panel-fg)'>Map Features</h3>
      <div id='features-list' style='font-size:0.85rem;max-height:300px;overflow-y:auto'></div>
    </div>
  </div>
  <div style='flex:2;min-width:400px'>
    <div style='background:#1a1a2e;border-radius:8px;border:1px solid var(--border);padding:1rem'>
      <canvas id='map-canvas' width='600' height='400' style='width:100%;background:#0d0d1a;border-radius:4px'></canvas>
      <div style='display:flex;justify-content:space-between;align-items:center;margin-top:0.5rem;color:var(--muted);font-size:0.85rem'>
        <span id='zoom-info'>Zoom: 1x</span>
        <span id='rotation-info'>Rot: 0°</span>
        <span id='gps-info'>GPS: --</span>
      </div>
    </div>
    <div style='margin-top:0.5rem;display:flex;gap:0.5rem;flex-wrap:wrap'>
      <button class='btn' onclick='zoomIn()'>Zoom +</button>
      <button class='btn' onclick='zoomOut()'>Zoom -</button>
      <button class='btn' onclick='rotateLeft()'>↺ Rotate</button>
      <button class='btn' onclick='rotateRight()'>↻ Rotate</button>
      <button class='btn' onclick='resetView()'>Reset</button>
      <button class='btn' onclick='centerOnGPS()'>Center GPS</button>
    </div>
  </div>
</div>

<div style='margin-top:1rem;background:var(--panel-bg);padding:1rem;border-radius:8px;border:1px solid var(--border)'>
  <h3 style='margin:0 0 0.5rem 0;color:var(--panel-fg)'>Legend</h3>
  <div style='display:flex;gap:1.5rem;flex-wrap:wrap;font-size:0.85rem'>
    <span><span style='display:inline-block;width:20px;height:3px;background:#ff6b6b;vertical-align:middle'></span> Highways</span>
    <span><span style='display:inline-block;width:20px;height:3px;background:#ffd93d;vertical-align:middle'></span> Major Roads</span>
    <span><span style='display:inline-block;width:20px;height:3px;background:#c8c8c8;vertical-align:middle'></span> Minor Roads</span>
    <span><span style='display:inline-block;width:20px;height:3px;background:#a8a8a8;vertical-align:middle;border-style:dashed'></span> Paths</span>
    <span><span style='display:inline-block;width:20px;height:3px;background:#4dabf7;vertical-align:middle'></span> Water</span>
    <span><span style='display:inline-block;width:20px;height:3px;background:#69db7c;vertical-align:middle'></span> Parks</span>
    <span><span style='display:inline-block;width:20px;height:3px;background:#da77f2;vertical-align:middle'></span> Railways</span>
    <span><span style='display:inline-block;width:12px;height:12px;background:#868e96;vertical-align:middle'></span> Buildings</span>
    <span><span style='display:inline-block;width:10px;height:10px;background:#ff0000;border-radius:50%;vertical-align:middle'></span> GPS Position</span>
    <span><span style='display:inline-block;width:10px;height:10px;background:#ffd93d;border-radius:50%;vertical-align:middle'></span> Waypoint</span>
    <span><span style='display:inline-block;width:10px;height:10px;background:#ff6b6b;border-radius:50%;vertical-align:middle'></span> Target Waypoint</span>
  </div>
</div>

<div style='margin-top:1rem;background:var(--panel-bg);padding:1rem;border-radius:8px;border:1px solid var(--border)'>
  <h3 style='margin:0 0 0.5rem 0;color:var(--panel-fg)'>Waypoints</h3>
  <div id='waypoint-status' style='margin-bottom:0.5rem;padding:8px;background:#0d0d1a;border-radius:4px;font-size:0.85rem'></div>
  <div style='display:grid;grid-template-columns:1fr 1fr 1fr auto;gap:8px;margin-bottom:0.5rem;align-items:center'>
    <input type='text' id='wp-name' placeholder='Name' maxlength='11' style='padding:6px;background:#0d0d1a;border:1px solid var(--border);border-radius:4px;color:var(--panel-fg)' />
    <input type='number' id='wp-lat' placeholder='Latitude' step='0.000001' style='padding:6px;background:#0d0d1a;border:1px solid var(--border);border-radius:4px;color:var(--panel-fg)' />
    <input type='number' id='wp-lon' placeholder='Longitude' step='0.000001' style='padding:6px;background:#0d0d1a;border:1px solid var(--border);border-radius:4px;color:var(--panel-fg)' />
    <button class='btn' onclick='addWaypoint()' style='padding:6px 12px'>Add</button>
  </div>
  <div id='waypoint-list' style='max-height:200px;overflow-y:auto'></div>
</div>
)HTML", HTTPD_RESP_USE_STRLEN);

  // JavaScript for map rendering
  httpd_resp_send_chunk(req, R"JS(
<script>
// Map state
let currentMap = null;
let zoom = 1;
let panX = 0, panY = 0;
let rotation = 0;  // Rotation in degrees
let isDragging = false;
let lastMouseX = 0, lastMouseY = 0;
let gpsLat = null, gpsLon = null;

// Feature colors
const COLORS = {
  0x00: { stroke: '#ff6b6b', width: 3 },    // Highway
  0x01: { stroke: '#ffd93d', width: 2 },    // Major road
  0x02: { stroke: '#c8c8c8', width: 1.5 },  // Minor road
  0x03: { stroke: '#a8a8a8', width: 1, dash: [4,4] }, // Path
  0x10: { stroke: '#4dabf7', width: 2 },    // Water
  0x11: { stroke: '#69db7c', width: 1.5 },  // Park
  0x20: { stroke: '#da77f2', width: 2, dash: [8,4] }, // Railway
  0x30: { stroke: '#868e96', width: 1, fill: '#495057' } // Building
};

// File explorer instance
let mapsExplorer = null;

// Initialize the shared file explorer for /maps folder
function initMapsFileBrowser() {
  if (typeof window.createFileExplorer !== 'function') {
    console.error('[MAPS] createFileExplorer not available');
    document.getElementById('maps-file-browser').innerHTML = '<p style="color:#ff6b6b">File browser not available</p>';
    return;
  }
  
  mapsExplorer = window.createFileExplorer({
    containerId: 'maps-file-browser',
    path: '/maps',
    height: '180px',
    mode: 'select',
    onSelect: function(filePath) {
      // When user clicks a .hwmap file, load it into the viewer
      if (filePath.endsWith('.hwmap')) {
        loadMap(filePath);
      } else {
        alert('Please select a .hwmap file');
      }
    },
    onNavigate: function(path) {
      // Stay in /maps - don't allow navigation elsewhere
      if (!path.startsWith('/maps')) {
        mapsExplorer.navigate('/maps');
      }
    }
  });
}

// Upload handler for maps
function uploadMapFile() {
  var input = document.createElement('input');
  input.type = 'file';
  input.accept = '.hwmap';
  input.onchange = function(e) {
    var file = e.target.files[0];
    if (!file) return;
    if (!file.name.endsWith('.hwmap')) {
      alert('Please select a .hwmap file');
      return;
    }
    if (file.size > 500 * 1024) {
      alert('File too large (max 500KB)');
      return;
    }
    
    var reader = new FileReader();
    reader.onload = function(evt) {
      var base64 = evt.target.result.split(',')[1];
      var targetPath = '/maps/' + file.name;
      
      fetch('/api/files/upload', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body: 'path=' + encodeURIComponent(targetPath) + '&binary=1&content=' + encodeURIComponent(base64)
      })
      .then(r => r.json())
      .then(data => {
        if (data.success) {
          initMapsFileBrowser(); // Refresh
          loadMap(targetPath);   // Auto-load the uploaded map
        } else {
          alert('Upload failed: ' + (data.error || 'Unknown error'));
        }
      })
      .catch(err => alert('Upload error: ' + err.message));
    };
    reader.readAsDataURL(file);
  };
  input.click();
}

// Load and parse a map file
async function loadMap(path) {
  try {
    const resp = await fetch('/api/files/view?name=' + encodeURIComponent(path));
    if (!resp.ok) throw new Error('Failed to load map');
    
    const buffer = await resp.arrayBuffer();
    currentMap = parseHWMap(buffer, path);
    
    if (currentMap) {
      document.getElementById('map-info').style.display = 'block';
      const minLat = (currentMap.minLat/1e6).toFixed(4);
      const maxLat = (currentMap.maxLat/1e6).toFixed(4);
      const minLon = (currentMap.minLon/1e6).toFixed(4);
      const maxLon = (currentMap.maxLon/1e6).toFixed(4);
      document.getElementById('map-details').innerHTML = `
        <div style="margin-bottom:4px"><strong>File:</strong> ${currentMap.filename}</div>
        <div style="margin-bottom:4px"><strong>Region:</strong> ${currentMap.regionName}</div>
        <div style="margin-bottom:4px"><strong>Features:</strong> ${currentMap.features.length}</div>
        <div style="margin-bottom:4px"><strong>Lat:</strong> ${minLat}° to ${maxLat}°</div>
        <div><strong>Lon:</strong> ${minLon}° to ${maxLon}°</div>
      `;
      resetView();
      renderMap();
      loadMapFeatures();
    }
  } catch (e) {
    console.error('Error loading map:', e);
    alert('Error loading map: ' + e.message);
  }
}

// Parse HWMAP binary format
function parseHWMap(buffer, filename) {
  const view = new DataView(buffer);
  let offset = 0;
  
  // Check magic "HWMP"
  const magic = String.fromCharCode(view.getUint8(0), view.getUint8(1), view.getUint8(2), view.getUint8(3));
  if (magic !== 'HWMP') {
    throw new Error('Invalid map file (bad magic)');
  }
  offset = 4;
  
  const version = view.getUint16(offset, true); offset += 2;
  const flags = view.getUint16(offset, true); offset += 2;
  
  const minLat = view.getInt32(offset, true); offset += 4;
  const minLon = view.getInt32(offset, true); offset += 4;
  const maxLat = view.getInt32(offset, true); offset += 4;
  const maxLon = view.getInt32(offset, true); offset += 4;
  
  const featureCount = view.getUint32(offset, true); offset += 4;
  
  let regionName = '';
  for (let i = 0; i < 8; i++) {
    const c = view.getUint8(offset++);
    if (c !== 0) regionName += String.fromCharCode(c);
  }
  
  // Parse features
  const features = [];
  for (let i = 0; i < featureCount && offset < buffer.byteLength; i++) {
    const type = view.getUint8(offset++);
    const pointCount = view.getUint8(offset++);
    
    if (pointCount < 2) continue;
    
    const points = [];
    
    // First point (absolute)
    let lat = view.getInt32(offset, true); offset += 4;
    let lon = view.getInt32(offset, true); offset += 4;
    points.push({ lat, lon });
    
    // Remaining points (delta encoded)
    for (let j = 1; j < pointCount; j++) {
      const dLat = view.getInt16(offset, true); offset += 2;
      const dLon = view.getInt16(offset, true); offset += 2;
      lat += dLat;
      lon += dLon;
      points.push({ lat, lon });
    }
    
    features.push({ type, points });
  }
  
  return {
    filename: filename.split('/').pop(),
    version, flags, minLat, minLon, maxLat, maxLon,
    regionName, features
  };
}

// Render map to canvas
function renderMap() {
  const canvas = document.getElementById('map-canvas');
  const ctx = canvas.getContext('2d');
  
  // Enable anti-aliasing
  ctx.imageSmoothingEnabled = true;
  ctx.imageSmoothingQuality = 'high';
  
  // Clear with gradient background
  const grad = ctx.createLinearGradient(0, 0, 0, canvas.height);
  grad.addColorStop(0, '#0a0a14');
  grad.addColorStop(1, '#12121f');
  ctx.fillStyle = grad;
  ctx.fillRect(0, 0, canvas.width, canvas.height);
  
  // Draw subtle grid
  ctx.strokeStyle = 'rgba(255,255,255,0.03)';
  ctx.lineWidth = 1;
  for (let x = 0; x < canvas.width; x += 40) {
    ctx.beginPath();
    ctx.moveTo(x, 0);
    ctx.lineTo(x, canvas.height);
    ctx.stroke();
  }
  for (let y = 0; y < canvas.height; y += 40) {
    ctx.beginPath();
    ctx.moveTo(0, y);
    ctx.lineTo(canvas.width, y);
    ctx.stroke();
  }
  
  if (!currentMap) {
    ctx.fillStyle = '#555';
    ctx.font = '16px system-ui, sans-serif';
    ctx.textAlign = 'center';
    ctx.fillText('Select a map to view', canvas.width/2, canvas.height/2);
    return;
  }
  
  const m = currentMap;
  const mapWidth = m.maxLon - m.minLon;
  const mapHeight = m.maxLat - m.minLat;
  
  // Scale to fit canvas with padding, center the map
  const padding = 30;
  const scaleX = (canvas.width - padding*2) / mapWidth;
  const scaleY = (canvas.height - padding*2) / mapHeight;
  const baseScale = Math.min(scaleX, scaleY);
  const scale = baseScale * zoom;
  
  // Center offset when not panned
  const mapRenderWidth = mapWidth * scale;
  const mapRenderHeight = mapHeight * scale;
  const centerOffsetX = (canvas.width - mapRenderWidth) / 2;
  const centerOffsetY = (canvas.height - mapRenderHeight) / 2;
  
  // Transform function with rotation
  const toCanvas = (lat, lon) => {
    // Calculate position relative to map center
    const mapCenterX = (m.minLon + m.maxLon) / 2;
    const mapCenterY = (m.minLat + m.maxLat) / 2;
    
    let x = (lon - mapCenterX) * scale;
    let y = -((lat - mapCenterY) * scale);  // Y inverted
    
    // Apply rotation around center
    if (rotation !== 0) {
      const rad = rotation * Math.PI / 180;
      const cos = Math.cos(rad);
      const sin = Math.sin(rad);
      const rx = x * cos - y * sin;
      const ry = x * sin + y * cos;
      x = rx;
      y = ry;
    }
    
    // Translate to canvas coordinates
    x += canvas.width / 2 + panX;
    y += canvas.height / 2 + panY;
    
    return { x, y };
  };
  
  // Set line style for smooth rendering
  ctx.lineCap = 'round';
  ctx.lineJoin = 'round';
  
  // Render features by type (back to front: buildings, parks, water, paths, roads)
  const renderOrder = [0x30, 0x11, 0x10, 0x03, 0x02, 0x01, 0x00, 0x20];
  
  for (const targetType of renderOrder) {
    for (const feature of m.features) {
      if (feature.type !== targetType) continue;
      if (feature.points.length < 2) continue;
      
      const style = COLORS[feature.type] || { stroke: '#888', width: 1 };
      
      // Draw shadow/glow for major features
      if (targetType === 0x00 || targetType === 0x10) {
        ctx.beginPath();
        ctx.strokeStyle = 'rgba(0,0,0,0.4)';
        ctx.lineWidth = style.width + 3;
        ctx.setLineDash([]);
        const first = toCanvas(feature.points[0].lat, feature.points[0].lon);
        ctx.moveTo(first.x + 1, first.y + 1);
        for (let i = 1; i < feature.points.length; i++) {
          const p = toCanvas(feature.points[i].lat, feature.points[i].lon);
          ctx.lineTo(p.x + 1, p.y + 1);
        }
        ctx.stroke();
      }
      
      ctx.beginPath();
      ctx.strokeStyle = style.stroke;
      ctx.lineWidth = style.width * (zoom > 1 ? 1 + (zoom-1)*0.2 : 1);
      if (style.dash) ctx.setLineDash(style.dash);
      else ctx.setLineDash([]);
      
      const first = toCanvas(feature.points[0].lat, feature.points[0].lon);
      ctx.moveTo(first.x, first.y);
      
      for (let i = 1; i < feature.points.length; i++) {
        const p = toCanvas(feature.points[i].lat, feature.points[i].lon);
        ctx.lineTo(p.x, p.y);
      }
      
      if (style.fill && feature.type === 0x30) {
        ctx.fillStyle = style.fill;
        ctx.globalAlpha = 0.6;
        ctx.fill();
        ctx.globalAlpha = 1.0;
      }
      ctx.stroke();
    }
  }
  
  // Draw waypoints
  if (waypoints && waypoints.length > 0) {
    waypoints.forEach((wp, idx) => {
      const wpLatMicro = wp.lat * 1e6;
      const wpLonMicro = wp.lon * 1e6;
      
      if (wpLatMicro >= m.minLat && wpLatMicro <= m.maxLat &&
          wpLonMicro >= m.minLon && wpLonMicro <= m.maxLon) {
        const pos = toCanvas(wpLatMicro, wpLonMicro);
        const isTarget = idx === targetWaypointIndex;
        
        // Draw waypoint marker
        ctx.beginPath();
        ctx.arc(pos.x, pos.y, isTarget ? 8 : 6, 0, Math.PI * 2);
        ctx.fillStyle = isTarget ? '#ff6b6b' : '#ffd93d';
        ctx.fill();
        ctx.strokeStyle = '#fff';
        ctx.lineWidth = 2;
        ctx.stroke();
        
        // Draw name label
        ctx.fillStyle = '#fff';
        ctx.font = '10px system-ui, sans-serif';
        ctx.textAlign = 'center';
        ctx.fillText(wp.name, pos.x, pos.y - 12);
      }
    });
  }
  
  // Draw GPS position
  if (gpsLat !== null && gpsLon !== null) {
    const gpsLatMicro = gpsLat * 1e6;
    const gpsLonMicro = gpsLon * 1e6;
    
    if (gpsLatMicro >= m.minLat && gpsLatMicro <= m.maxLat &&
        gpsLonMicro >= m.minLon && gpsLonMicro <= m.maxLon) {
      const pos = toCanvas(gpsLatMicro, gpsLonMicro);
      
      // Outer glow
      ctx.beginPath();
      ctx.arc(pos.x, pos.y, 12, 0, Math.PI * 2);
      ctx.fillStyle = 'rgba(255, 100, 100, 0.2)';
      ctx.fill();
      
      // Middle ring
      ctx.beginPath();
      ctx.arc(pos.x, pos.y, 8, 0, Math.PI * 2);
      ctx.fillStyle = 'rgba(255, 50, 50, 0.4)';
      ctx.fill();
      
      // Inner dot
      ctx.beginPath();
      ctx.arc(pos.x, pos.y, 5, 0, Math.PI * 2);
      ctx.fillStyle = '#ff3333';
      ctx.fill();
      ctx.strokeStyle = '#fff';
      ctx.lineWidth = 2;
      ctx.stroke();
    }
  }
  
  ctx.setLineDash([]);
  document.getElementById('zoom-info').textContent = 'Zoom: ' + zoom.toFixed(1) + 'x';
  document.getElementById('rotation-info').textContent = 'Rot: ' + rotation + '°';
}

// Controls
function zoomIn() { zoom *= 1.5; renderMap(); }
function zoomOut() { zoom /= 1.5; if (zoom < 0.5) zoom = 0.5; renderMap(); }
function rotateLeft() { rotation = (rotation - 15 + 360) % 360; renderMap(); }
function rotateRight() { rotation = (rotation + 15) % 360; renderMap(); }
function resetView() { zoom = 1; panX = 0; panY = 0; rotation = 0; renderMap(); }
function centerOnGPS() {
  if (!currentMap || gpsLat === null) return;
  const m = currentMap;
  const canvas = document.getElementById('map-canvas');
  const mapWidth = m.maxLon - m.minLon;
  const mapHeight = m.maxLat - m.minLat;
  const scaleX = (canvas.width - 40) / mapWidth;
  const scaleY = (canvas.height - 40) / mapHeight;
  const scale = Math.min(scaleX, scaleY) * zoom;
  
  const gpsLatMicro = gpsLat * 1e6;
  const gpsLonMicro = gpsLon * 1e6;
  panX = canvas.width/2 - ((gpsLonMicro - m.minLon) * scale + 20);
  panY = -canvas.height/2 + ((gpsLatMicro - m.minLat) * scale + 20);
  renderMap();
}

// Mouse controls for panning
const canvas = document.getElementById('map-canvas');
canvas.addEventListener('mousedown', (e) => {
  isDragging = true;
  lastMouseX = e.clientX;
  lastMouseY = e.clientY;
  canvas.style.cursor = 'grabbing';
});
canvas.addEventListener('mousemove', (e) => {
  if (!isDragging) return;
  panX += e.clientX - lastMouseX;
  panY += e.clientY - lastMouseY;
  lastMouseX = e.clientX;
  lastMouseY = e.clientY;
  renderMap();
});
canvas.addEventListener('mouseup', () => { isDragging = false; canvas.style.cursor = 'grab'; });
canvas.addEventListener('mouseleave', () => { isDragging = false; canvas.style.cursor = 'grab'; });
canvas.addEventListener('wheel', (e) => {
  e.preventDefault();
  if (e.deltaY < 0) zoomIn();
  else zoomOut();
});
canvas.style.cursor = 'grab';

// System state
let i2cEnabled = false;
let gpsEnabled = false;
let gpsPollingInterval = null;

// Check system status to see if I2C/GPS are available
async function checkSystemStatus() {
  try {
    const resp = await fetch('/api/system');
    const data = await resp.json();
    i2cEnabled = data.i2c_enabled === true;
    
    // Also check if GPS sensor is specifically enabled
    if (i2cEnabled) {
      const sensorsResp = await fetch('/api/sensors/status');
      const sensorsData = await sensorsResp.json();
      gpsEnabled = sensorsData.gps && sensorsData.gps.enabled;
    } else {
      gpsEnabled = false;
    }
    
    updateGPSStatusDisplay();
    
    // Start/stop GPS polling based on availability
    if (i2cEnabled && gpsEnabled) {
      if (!gpsPollingInterval) {
        gpsPollingInterval = setInterval(updateGPS, 5000);
      }
      updateGPS();
    } else {
      if (gpsPollingInterval) {
        clearInterval(gpsPollingInterval);
        gpsPollingInterval = null;
      }
    }
  } catch (e) {
    console.error('System status check failed:', e);
    i2cEnabled = false;
    gpsEnabled = false;
    updateGPSStatusDisplay();
  }
}

function updateGPSStatusDisplay() {
  const el = document.getElementById('gps-info');
  if (!i2cEnabled) {
    el.textContent = 'GPS: I2C disabled';
    el.style.color = '#868e96';
  } else if (!gpsEnabled) {
    el.textContent = 'GPS: Not enabled';
    el.style.color = '#868e96';
  } else if (gpsLat === null) {
    el.textContent = 'GPS: Waiting...';
    el.style.color = 'var(--muted)';
  }
}

// Fetch GPS position (only called if GPS is enabled)
async function updateGPS() {
  if (!i2cEnabled || !gpsEnabled) return;
  
  try {
    const resp = await fetch('/api/sensors/status');
    const data = await resp.json();
    
    if (!data.gps) {
      document.getElementById('gps-info').textContent = 'GPS: Not available';
      document.getElementById('gps-info').style.color = '#868e96';
      return;
    }
    
    if (!data.gps.enabled) {
      gpsEnabled = false;
      document.getElementById('gps-info').textContent = 'GPS: Disabled';
      document.getElementById('gps-info').style.color = '#868e96';
      return;
    }
    
    if (data.gps.fix) {
      gpsLat = data.gps.lat;
      gpsLon = data.gps.lon;
      document.getElementById('gps-info').textContent = `GPS: ${gpsLat.toFixed(5)}, ${gpsLon.toFixed(5)}`;
      document.getElementById('gps-info').style.color = '#69db7c';
      renderMap();
    } else {
      document.getElementById('gps-info').textContent = `GPS: No fix (${data.gps.satellites || 0} sats)`;
      document.getElementById('gps-info').style.color = '#ffd93d';
    }
  } catch (e) {
    document.getElementById('gps-info').textContent = 'GPS: Error';
    document.getElementById('gps-info').style.color = '#ff6b6b';
  }
}

// Waypoint management
let waypoints = [];
let targetWaypointIndex = -1;

async function loadWaypoints() {
  if (!currentMap) {
    document.getElementById('waypoint-status').textContent = 'No map loaded';
    document.getElementById('waypoint-list').innerHTML = '';
    return;
  }
  
  try {
    const resp = await fetch('/api/waypoints');
    const data = await resp.json();
    
    if (data.success) {
      waypoints = data.waypoints || [];
      targetWaypointIndex = data.target || -1;
      document.getElementById('waypoint-status').textContent = `Map: ${data.mapName} | Waypoints: ${data.count}/${data.max}`;
      
      if (waypoints.length > 0) {
        let html = '<table style="width:100%;font-size:0.85rem;border-collapse:collapse">';
        html += '<tr style="background:#0a0a14"><th style="padding:4px;text-align:left">Name</th><th>Lat</th><th>Lon</th><th>Actions</th></tr>';
        waypoints.forEach((wp, idx) => {
          const isTarget = idx === targetWaypointIndex;
          html += `<tr style="border-bottom:1px solid #333${isTarget ? ';background:#2a1a1a' : ''}">`;
          html += `<td style="padding:4px">${wp.name}${isTarget ? ' ⭐' : ''}</td>`;
          html += `<td style="text-align:center">${wp.lat.toFixed(5)}</td>`;
          html += `<td style="text-align:center">${wp.lon.toFixed(5)}</td>`;
          html += '<td style="text-align:center">';
          if (!isTarget) {
            html += `<button onclick="gotoWaypoint(${idx})" style="padding:2px 6px;margin:0 2px;background:#2196F3;color:#fff;border:none;border-radius:3px;cursor:pointer;font-size:0.75rem">Target</button>`;
          } else {
            html += `<button onclick="clearTarget()" style="padding:2px 6px;margin:0 2px;background:#FF9800;color:#fff;border:none;border-radius:3px;cursor:pointer;font-size:0.75rem">Clear</button>`;
          }
          html += `<button onclick="deleteWaypoint(${idx})" style="padding:2px 6px;margin:0 2px;background:#f44336;color:#fff;border:none;border-radius:3px;cursor:pointer;font-size:0.75rem">Del</button>`;
          html += '</td></tr>';
        });
        html += '</table>';
        document.getElementById('waypoint-list').innerHTML = html;
      } else {
        document.getElementById('waypoint-list').innerHTML = '<p style="color:#666;font-size:0.85rem;margin:0.5rem 0">No waypoints for this map</p>';
      }
      renderMap();
    } else {
      document.getElementById('waypoint-status').textContent = 'Error: ' + (data.error || 'Failed to load');
    }
  } catch (e) {
    document.getElementById('waypoint-status').textContent = 'Error loading waypoints';
  }
}

async function addWaypoint() {
  const name = document.getElementById('wp-name').value.trim();
  const lat = parseFloat(document.getElementById('wp-lat').value);
  const lon = parseFloat(document.getElementById('wp-lon').value);
  
  if (!name || isNaN(lat) || isNaN(lon)) {
    alert('Please fill in all fields');
    return;
  }
  
  const formData = new FormData();
  formData.append('action', 'add');
  formData.append('name', name);
  formData.append('lat', lat);
  formData.append('lon', lon);
  
  try {
    const resp = await fetch('/api/waypoints', {method: 'POST', body: formData});
    const data = await resp.json();
    if (data.success) {
      document.getElementById('wp-name').value = '';
      document.getElementById('wp-lat').value = '';
      document.getElementById('wp-lon').value = '';
      loadWaypoints();
    } else {
      alert('Error: ' + (data.error || 'Failed to add'));
    }
  } catch (e) {
    alert('Error: ' + e.message);
  }
}

async function deleteWaypoint(idx) {
  if (!confirm('Delete this waypoint?')) return;
  
  const formData = new FormData();
  formData.append('action', 'delete');
  formData.append('index', idx);
  
  try {
    const resp = await fetch('/api/waypoints', {method: 'POST', body: formData});
    const data = await resp.json();
    if (data.success) loadWaypoints();
    else alert('Error: ' + (data.error || 'Failed'));
  } catch (e) {
    alert('Error: ' + e.message);
  }
}

async function gotoWaypoint(idx) {
  const formData = new FormData();
  formData.append('action', 'goto');
  formData.append('index', idx);
  
  try {
    const resp = await fetch('/api/waypoints', {method: 'POST', body: formData});
    const data = await resp.json();
    if (data.success) loadWaypoints();
    else alert('Error: ' + (data.error || 'Failed'));
  } catch (e) {
    alert('Error: ' + e.message);
  }
}

async function clearTarget() {
  const formData = new FormData();
  formData.append('action', 'clear');
  
  try {
    const resp = await fetch('/api/waypoints', {method: 'POST', body: formData});
    const data = await resp.json();
    if (data.success) loadWaypoints();
    else alert('Error: ' + (data.error || 'Failed'));
  } catch (e) {
    alert('Error: ' + e.message);
  }
}

// Load map features from API
async function loadMapFeatures() {
  const panel = document.getElementById('map-features');
  const list = document.getElementById('features-list');
  
  try {
    const resp = await fetch('/api/maps/features');
    const data = await resp.json();
    
    if (data.error || !data.hasMetadata || data.metadataCount === 0) {
      panel.style.display = 'none';
      return;
    }
    
    let html = '';
    const catColors = {
      highways: '#ff6b6b',
      roads: '#ffd93d',
      water: '#4dabf7',
      parks: '#69db7c',
      railways: '#da77f2',
      subways: '#da77f2'
    };
    const catNames = {
      highways: 'Highways',
      roads: 'Roads',
      water: 'Water',
      parks: 'Parks',
      railways: 'Railways',
      subways: 'Subways'
    };
    
    for (const [cat, items] of Object.entries(data.categories || {})) {
      if (items && items.length > 0) {
        const color = catColors[cat] || '#888';
        html += `<div style="margin-bottom:8px">`;
        html += `<div style="color:${color};font-weight:bold;margin-bottom:4px">${catNames[cat] || cat} (${items.length})</div>`;
        html += `<div style="color:var(--muted);padding-left:8px">`;
        items.slice(0, 20).forEach(name => {
          html += `<div style="margin:2px 0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap" title="${name}">${name}</div>`;
        });
        if (items.length > 20) {
          html += `<div style="color:#666;font-style:italic">...and ${items.length - 20} more</div>`;
        }
        html += `</div></div>`;
      }
    }
    
    if (html) {
      list.innerHTML = html;
      panel.style.display = 'block';
    } else {
      panel.style.display = 'none';
    }
  } catch (e) {
    console.error('Error loading map features:', e);
    panel.style.display = 'none';
  }
}

// Initialize
initMapsFileBrowser();
checkSystemStatus();
renderMap();
loadWaypoints();
loadMapFeatures();
setInterval(loadWaypoints, 5000);
</script>
)JS", HTTPD_RESP_USE_STRLEN);
}

// Forward declarations for auth functions (defined in WebCore_Server.cpp)
bool isAuthed(httpd_req_t* req, String& outUser);

// API endpoint to get map features/metadata
inline esp_err_t handleMapFeaturesAPI(httpd_req_t* req) {
  String user;
  if (!isAuthed(req, user)) {
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_sendstr(req, "{\"error\":\"Authentication required\"}");
    return ESP_OK;
  }
  
  httpd_resp_set_type(req, "application/json");
  
  if (!MapCore::hasValidMap()) {
    httpd_resp_sendstr(req, "{\"error\":\"No map loaded\"}");
    return ESP_OK;
  }
  
  const LoadedMap& map = MapCore::getCurrentMap();
  
  // Build JSON response
  String json = "{";
  json += "\"mapName\":\"" + String(map.filename) + "\",";
  json += "\"hasMetadata\":" + String(map.hasMetadata ? "true" : "false") + ",";
  json += "\"featureCount\":" + String(map.header.featureCount) + ",";
  
  if (map.hasMetadata && map.metadataCount > 0) {
    json += "\"metadataCount\":" + String(map.metadataCount) + ",";
    json += "\"categories\":{";
    
    // Category names
    const char* catNames[] = {"highways", "roads", "water", "parks", "railways", "subways"};
    bool firstCat = true;
    
    for (int cat = 0; cat <= 5; cat++) {
      // Count items in this category
      int count = 0;
      for (int i = 0; i < map.metadataCount; i++) {
        if (map.metadata[i].category == cat) count++;
      }
      
      if (count > 0) {
        if (!firstCat) json += ",";
        firstCat = false;
        
        json += "\"" + String(catNames[cat]) + "\":[";
        bool first = true;
        for (int i = 0; i < map.metadataCount; i++) {
          if (map.metadata[i].category == cat) {
            if (!first) json += ",";
            first = false;
            
            // Escape quotes in name
            String name = map.metadata[i].name;
            name.replace("\"", "\\\"");
            json += "\"" + name + "\"";
          }
        }
        json += "]";
      }
    }
    json += "}";
  } else {
    json += "\"metadataCount\":0";
  }
  
  json += "}";
  
  httpd_resp_sendstr(req, json.c_str());
  return ESP_OK;
}

#endif
