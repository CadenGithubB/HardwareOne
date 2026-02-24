#ifndef WEBPAGE_MAPS_H
#define WEBPAGE_MAPS_H

#include "System_BuildConfig.h"
#if ENABLE_HTTP_SERVER
#include <esp_http_server.h>
#endif

#if ENABLE_MAPS

#include "WebServer_Utils.h"
#include "System_Maps.h"
#include <cstring>

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
      <h3 style='margin:0 0 0.5rem 0;color:var(--panel-fg)'>Maps</h3>
      <div style='display:flex;gap:0.5rem;align-items:center;flex-wrap:wrap;margin:0 0 0.5rem 0'>
        <button class='btn' onclick='organizeMaps()' style='padding:6px 10px'>Organize Maps</button>
        <span id='maps-organize-status' style='font-size:0.8rem;color:var(--panel-fg)'></span>
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
    <div style='background:var(--panel-bg);border-radius:8px;border:1px solid var(--border);padding:1rem'>
      <canvas id='map-canvas' width='1200' height='800' style='width:100%;background:var(--bg);border-radius:4px'></canvas>
      <div style='display:flex;justify-content:space-between;align-items:center;margin-top:0.5rem;color:var(--panel-fg);font-size:0.85rem'>
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
      <button class='btn' onclick='showSearchDialog()'>🔍 Search</button>
      <button class='btn' id='btn-add-waypoint' onclick='toggleWaypointMode()'>📍 Add Waypoint</button>
    </div>
    <div id='search-dialog' style='display:none;margin-top:1rem;background:var(--panel-bg);padding:1rem;border-radius:8px;border:1px solid var(--border)'>
      <h4 style='margin:0 0 0.5rem 0'>Search Map Features</h4>
      <input type='text' id='search-input' placeholder='Type to search...' style='width:100%;padding:0.5rem;border:1px solid var(--border);border-radius:4px;background:var(--bg);color:var(--fg);margin-bottom:0.5rem' oninput='searchMapNames()'>
      <div id='search-results' style='max-height:200px;overflow-y:auto;font-size:0.85rem'></div>
      <div style='margin-top:0.5rem;display:flex;gap:0.5rem'>
        <button class='btn' onclick='hideSearchDialog()'>Close</button>
      </div>
    </div>
  </div>
</div>

<div style='margin-top:1rem;background:var(--panel-bg);padding:1rem;border-radius:8px;border:1px solid var(--border)'>
  <h3 style='margin:0 0 0.5rem 0;color:var(--panel-fg)'>Layers</h3>
  <div style='display:grid;grid-template-columns:repeat(auto-fill,minmax(110px,1fr));gap:4px;padding:8px;background:var(--input-bg);border-radius:4px;margin-top:6px;font-size:0.75rem;border:1px solid var(--border)'>
    <label style='display:flex;align-items:center;gap:4px;cursor:pointer;'><input type='checkbox' id='layer-road-motorway' checked onchange='renderMap()' style='width:16px;height:16px;margin:0'><span style='color:#ff6b6b'>Motorway</span></label>
    <label style='display:flex;align-items:center;gap:4px;cursor:pointer;'><input type='checkbox' id='layer-road-trunk' checked onchange='renderMap()' style='width:16px;height:16px;margin:0'><span style='color:#ff8787'>Trunk</span></label>
    <label style='display:flex;align-items:center;gap:4px;cursor:pointer;'><input type='checkbox' id='layer-road-primary' checked onchange='renderMap()' style='width:16px;height:16px;margin:0'><span style='color:#ffd93d'>Primary</span></label>
    <label style='display:flex;align-items:center;gap:4px;cursor:pointer;'><input type='checkbox' id='layer-road-secondary' checked onchange='renderMap()' style='width:16px;height:16px;margin:0'><span style='color:#ffe066'>Secondary</span></label>
    <label style='display:flex;align-items:center;gap:4px;cursor:pointer;'><input type='checkbox' id='layer-road-tertiary' checked onchange='renderMap()' style='width:16px;height:16px;margin:0'><span style='color:#ffffff'>Tertiary</span></label>
    <label style='display:flex;align-items:center;gap:4px;cursor:pointer;'><input type='checkbox' id='layer-road-residential' checked onchange='renderMap()' style='width:16px;height:16px;margin:0'><span style='color:#e9ecef'>Residential</span></label>
    <label style='display:flex;align-items:center;gap:4px;cursor:pointer;'><input type='checkbox' id='layer-road-service' checked onchange='renderMap()' style='width:16px;height:16px;margin:0'><span style='color:#ced4da'>Service</span></label>
    <label style='display:flex;align-items:center;gap:4px;cursor:pointer;'><input type='checkbox' id='layer-path-footway' checked onchange='renderMap()' style='width:16px;height:16px;margin:0'><span style='color:#aaaaaa'>Footway</span></label>
    <label style='display:flex;align-items:center;gap:4px;cursor:pointer;'><input type='checkbox' id='layer-path-cycleway' checked onchange='renderMap()' style='width:16px;height:16px;margin:0'><span style='color:#74c0fc'>Cycleway</span></label>
    <label style='display:flex;align-items:center;gap:4px;cursor:pointer;'><input type='checkbox' id='layer-path-track' checked onchange='renderMap()' style='width:16px;height:16px;margin:0'><span style='color:#8b7355'>Track</span></label>
    <label style='display:flex;align-items:center;gap:4px;cursor:pointer;'><input type='checkbox' id='layer-water-lake' checked onchange='renderMap()' style='width:16px;height:16px;margin:0'><span style='color:#4dabf7'>Lakes</span></label>
    <label style='display:flex;align-items:center;gap:4px;cursor:pointer;'><input type='checkbox' id='layer-water-river' checked onchange='renderMap()' style='width:16px;height:16px;margin:0'><span style='color:#339af0'>Rivers</span></label>
    <label style='display:flex;align-items:center;gap:4px;cursor:pointer;'><input type='checkbox' id='layer-water-coast' checked onchange='renderMap()' style='width:16px;height:16px;margin:0'><span style='color:#1c7ed6'>Coast</span></label>
    <label style='display:flex;align-items:center;gap:4px;cursor:pointer;'><input type='checkbox' id='layer-landmask' checked onchange='renderMap()' style='width:16px;height:16px;margin:0'><span style='color:#c9b896'>Land Mask</span></label>
    <label style='display:flex;align-items:center;gap:4px;cursor:pointer;'><input type='checkbox' id='layer-park-park' checked onchange='renderMap()' style='width:16px;height:16px;margin:0'><span style='color:#69db7c'>Parks</span></label>
    <label style='display:flex;align-items:center;gap:4px;cursor:pointer;'><input type='checkbox' id='layer-park-forest' checked onchange='renderMap()' style='width:16px;height:16px;margin:0'><span style='color:#40c057'>Forests</span></label>
    <label style='display:flex;align-items:center;gap:4px;cursor:pointer;'><input type='checkbox' id='layer-park-grass' checked onchange='renderMap()' style='width:16px;height:16px;margin:0'><span style='color:#a9e34b'>Grass</span></label>
    <label style='display:flex;align-items:center;gap:4px;cursor:pointer;'><input type='checkbox' id='layer-rail-rail' checked onchange='renderMap()' style='width:16px;height:16px;margin:0'><span style='color:#da77f2'>Rail</span></label>
    <label style='display:flex;align-items:center;gap:4px;cursor:pointer;'><input type='checkbox' id='layer-rail-subway' checked onchange='renderMap()' style='width:16px;height:16px;margin:0'><span style='color:#e599f7'>Subway</span></label>
    <label style='display:flex;align-items:center;gap:4px;cursor:pointer;'><input type='checkbox' id='layer-transit-bus' checked onchange='renderMap()' style='width:16px;height:16px;margin:0'><span style='color:var(--accent)'>Bus</span></label>
    <label style='display:flex;align-items:center;gap:4px;cursor:pointer;'><input type='checkbox' id='layer-transit-ferry' checked onchange='renderMap()' style='width:16px;height:16px;margin:0'><span style='color:var(--accent)'>Ferry</span></label>
    <label style='display:flex;align-items:center;gap:4px;cursor:pointer;'><input type='checkbox' id='layer-transit-stations' checked onchange='renderMap()' style='width:16px;height:16px;margin:0'><span style='color:var(--accent)'>Stations</span></label>
    <label style='display:flex;align-items:center;gap:4px;cursor:pointer;'><input type='checkbox' id='layer-building' checked onchange='renderMap()' style='width:16px;height:16px;margin:0'><span style='color:#868e96'>Buildings</span></label>
    <label style='display:flex;align-items:center;gap:4px;cursor:pointer;'><input type='checkbox' id='layer-industrial' checked onchange='renderMap()' style='width:16px;height:16px;margin:0'><span style='color:#fab005'>Industrial</span></label>
    <label style='display:flex;align-items:center;gap:4px;cursor:pointer;'><input type='checkbox' id='layer-commercial' checked onchange='renderMap()' style='width:16px;height:16px;margin:0'><span style='color:#ced4da'>Commercial</span></label>
    <label style='display:flex;align-items:center;gap:4px;cursor:pointer;'><input type='checkbox' id='layer-residential-area' checked onchange='renderMap()' style='width:16px;height:16px;margin:0'><span style='color:#dee2e6'>Res. Areas</span></label>
  </div>
  <div style='margin-top:0.5rem;display:flex;gap:1rem;flex-wrap:wrap;font-size:0.8rem;color:var(--panel-fg)'>
    <span><span style='display:inline-block;width:10px;height:10px;background:#ff0000;border-radius:50%;vertical-align:middle'></span> GPS</span>
    <span><span style='display:inline-block;width:10px;height:10px;background:#ffd93d;border-radius:50%;vertical-align:middle'></span> Waypoint</span>
    <span><span style='display:inline-block;width:20px;height:3px;background:#00d9ff;vertical-align:middle'></span> Track</span>
  </div>
</div>

<div style='margin-top:1rem;background:var(--panel-bg);padding:1rem;border-radius:8px;border:1px solid var(--border)'>
  <h3 style='margin:0 0 0.5rem 0;color:var(--panel-fg)'>GPS Tracks</h3>
  <div style='display:flex;gap:8px;margin-bottom:0.5rem;align-items:center'>
    <select id='track-file' style='flex:1;padding:6px;background:var(--crumb-bg);border:1px solid var(--border);border-radius:4px;color:var(--panel-fg)'>
      <option value=''>Select GPS log file...</option>
    </select>
    <button class='btn' onclick='loadGPSTrack()' style='padding:6px 12px'>Load</button>
    <button class='btn' onclick='clearGPSTrack()' style='padding:6px 12px'>Clear</button>
    <button class='btn' id='btn-live-track' onclick='toggleLiveTrack()' style='padding:6px 12px'>Live</button>
  </div>
  <div style='display:flex;gap:8px;margin-bottom:0.5rem;align-items:center'>
    <span style='font-size:0.85rem;color:var(--panel-fg)'>Track Color:</span>
    <input type='color' id='track-color' value='#00d9ff' onchange='updateTrackColor()' style='width:40px;height:28px;border:none;background:none;cursor:pointer' />
    <select id='track-color-preset' onchange='applyColorPreset()' style='padding:4px;background:var(--crumb-bg);border:1px solid var(--border);border-radius:4px;color:var(--panel-fg);font-size:0.85rem'>
      <option value='#00d9ff'>Cyan</option>
      <option value='#ff6b6b'>Red</option>
      <option value='#ffd93d'>Yellow</option>
      <option value='#69db7c'>Green</option>
      <option value='#da77f2'>Purple</option>
      <option value='#ff922b'>Orange</option>
      <option value='#ffffff'>White</option>
    </select>
  </div>
  <div id='track-info' style='font-size:0.85rem;color:var(--panel-fg)'></div>
</div>

<div style='margin-top:1rem;background:var(--panel-bg);padding:1rem;border-radius:8px;border:1px solid var(--border)'>
  <h3 style='margin:0 0 0.5rem 0;color:var(--panel-fg)'>Waypoints</h3>
  <div id='waypoint-status' style='margin-bottom:0.5rem;padding:8px;background:var(--crumb-bg);border-radius:4px;font-size:0.85rem'></div>
  <div style='display:grid;grid-template-columns:1fr 1fr 1fr auto;gap:8px;margin-bottom:0.5rem;align-items:center'>
    <input type='text' id='wp-name' placeholder='Name' maxlength='11' style='padding:6px;background:var(--crumb-bg);border:1px solid var(--border);border-radius:4px;color:var(--panel-fg)' />
    <input type='number' id='wp-lat' placeholder='Latitude' step='0.000001' style='padding:6px;background:var(--crumb-bg);border:1px solid var(--border);border-radius:4px;color:var(--panel-fg)' />
    <input type='number' id='wp-lon' placeholder='Longitude' step='0.000001' style='padding:6px;background:var(--crumb-bg);border:1px solid var(--border);border-radius:4px;color:var(--panel-fg)' />
    <button class='btn' onclick='addWaypoint()' style='padding:6px 12px'>Add</button>
  </div>
  <input type='text' id='wp-notes' placeholder='Notes (optional)' maxlength='255' style='width:100%;padding:6px;background:var(--crumb-bg);border:1px solid var(--border);border-radius:4px;color:var(--panel-fg);margin-bottom:0.5rem' />
  <div style='display:flex;gap:6px;margin-bottom:0.5rem'>
    <button class='btn' onclick='exportWaypoints()' style='padding:6px 10px'>Export</button>
    <button class='btn' onclick='importWaypoints()' style='padding:6px 10px'>Import</button>
    <button class='btn' onclick='clearAllWaypoints()' style='padding:6px 10px'>Clear</button>
  </div>
  <div id='waypoint-list' style='max-height:200px;overflow-y:auto'></div>
</div>

<div style='margin-top:1rem;background:var(--panel-bg);padding:1rem;border-radius:8px;border:1px solid var(--border)'>
  <h3 style='margin:0 0 0.5rem 0;color:var(--panel-fg)'>Transit Routes</h3>
  <div id='routes-list' style='max-height:250px;overflow-y:auto;font-size:0.85rem'>
    <div style='color:var(--panel-fg)'>Load a map to see routes</div>
  </div>
</div>
)HTML", HTTPD_RESP_USE_STRLEN);

  // JavaScript for map rendering
  httpd_resp_send_chunk(req, R"JS(
<script>
// Map state
let currentMap = null;
let zoom = 10;
let panX = 0, panY = 0;
let rotation = 0;  // Rotation in degrees
let isDragging = false;
let lastMouseX = 0, lastMouseY = 0;
let gpsLat = null, gpsLon = null;
let gpsTrack = null;  // Array of {lat, lon} points
let trackColor = '#00d9ff';  // Default track color
let selectedFeature = null;  // Currently selected feature from search
let searchResults = [];      // All matching features for navigation
let searchResultIndex = 0;   // Current index in search results
let waypointMode = false;    // Add waypoint on click mode
let selectedWaypointIndex = -1;  // Selected waypoint (for click selection)
let isPointerDown = false;
let downMouseX = 0, downMouseY = 0;
let pendingWaypointClickIndex = -1;
let didDrag = false;

// Toggle waypoint add mode
function toggleWaypointMode() {
  waypointMode = !waypointMode;
  const btn = document.getElementById('btn-add-waypoint');
  if (waypointMode) {
    btn.style.background = 'var(--accent)';
    btn.style.color = '#000';
    document.getElementById('map-canvas').style.cursor = 'crosshair';
  } else {
    btn.style.background = '';
    btn.style.color = '';
    document.getElementById('map-canvas').style.cursor = 'grab';
  }
}

// Add waypoint at canvas coordinates
async function addMapWaypoint(canvasX, canvasY) {
  if (!currentMap) return;
  
  // Convert canvas coords to geo coords (inverse of toCanvas in renderMap)
  const canvas = document.getElementById('map-canvas');
  const m = currentMap;
  const mapWidth = m.maxLon - m.minLon;
  const mapHeight = m.maxLat - m.minLat;
  const scaleX = (canvas.width * 0.9) / mapWidth;
  const scaleY = (canvas.height * 0.9) / mapHeight;
  const baseScale = Math.min(scaleX, scaleY);
  const scale = baseScale * zoom;
  const mapCenterX = (m.minLon + m.maxLon) / 2;
  const mapCenterY = (m.minLat + m.maxLat) / 2;
  
  // Inverse transform
  let x = canvasX - canvas.width/2 - panX;
  let y = canvasY - canvas.height/2 - panY;
  
  // Undo rotation
  if (rotation !== 0) {
    const rad = -rotation * Math.PI / 180;
    const cos = Math.cos(rad);
    const sin = Math.sin(rad);
    const rx = x * cos - y * sin;
    const ry = x * sin + y * cos;
    x = rx; y = ry;
  }
  
  const lon = mapCenterX + x / scale;
  const lat = mapCenterY - y / scale;
  
  const name = await hwPrompt('Waypoint name:', 'WP' + (waypoints.length + 1));
  if (name !== null && name.trim()) {
    const notes = (await hwPrompt('Notes (optional, max 255 chars):', '')) || '';
    addWaypointViaAPI(name.trim().substring(0, 11), lat, lon, notes.substring(0, 255)).catch((err) => {
      hwAlert('Error: ' + (err && err.message ? err.message : err));
    });
  }
  
  // Exit waypoint mode after adding
  toggleWaypointMode();
}

// Feature colors (widths doubled for high-DPI canvas) - MUST MATCH preview in map-tool
const COLORS = {
  0x00: { stroke: '#ff6b6b', width: 6 },    // Highway
  0x01: { stroke: '#ffd93d', width: 4 },    // Major road
  0x02: { stroke: '#ffffff', width: 3 },    // Minor road (white like preview)
  0x03: { stroke: '#aaaaaa', width: 2, dash: [8,8] }, // Path
  0x10: { stroke: '#4dabf7', width: 2, fill: '#1864ab' },    // Water (filled polygon)
  0x11: { stroke: '#69db7c', width: 1, fill: '#2b8a3e' },    // Park (filled polygon)
  0x12: { stroke: '#c9b896', width: 0, fill: '#c9b896' },    // Land mask (tan to restore land on top of water)
  0x20: { stroke: '#da77f2', width: 4, dash: [16,8] }, // Railway
  0x21: { stroke: '#fab005', width: 3, dash: [12,6] }, // Bus route (orange)
  0x22: { stroke: '#15aabf', width: 4, dash: [16,8] }, // Ferry (cyan)
  0x30: { stroke: '#868e96', width: 2, fill: '#495057' }, // Building (gray like preview)
  0x40: { stroke: '#f06595', width: 0, radius: 6 }  // Station (pink dot)
};

// V6 subtype-specific colors (when map version >= 6)
const SUBTYPE_COLORS = {
  // Highway subtypes (0x00)
  0x00: {
    0: { stroke: '#ff6b6b', width: 7 },  // Motorway (thicker)
    1: { stroke: '#ff8787', width: 5 }   // Trunk (slightly thinner)
  },
  // Major road subtypes (0x01)
  0x01: {
    0: { stroke: '#ffd93d', width: 4 },  // Primary
    1: { stroke: '#ffe066', width: 3 }   // Secondary (thinner)
  },
  // Minor road subtypes (0x02)
  0x02: {
    0: { stroke: '#ffffff', width: 3 },  // Tertiary
    1: { stroke: '#e9ecef', width: 2 },  // Residential (thinner)
    2: { stroke: '#ced4da', width: 1 }   // Service (thinnest)
  },
  // Path subtypes (0x03)
  0x03: {
    0: { stroke: '#aaaaaa', width: 2, dash: [6,6] },   // Footway
    1: { stroke: '#74c0fc', width: 2, dash: [8,4] },   // Cycleway (blue tint)
    2: { stroke: '#8b7355', width: 2, dash: [10,5] }   // Track (brown)
  },
  // Water subtypes (0x10)
  0x10: {
    0: { stroke: '#4dabf7', width: 2, fill: '#1864ab' },  // Lake
    1: { stroke: '#339af0', width: 3 },                    // River (line only)
    2: { stroke: '#1c7ed6', width: 2, fill: '#1864ab' }   // Coastline
  },
  // Park/nature subtypes (0x11)
  0x11: {
    0: { stroke: '#69db7c', width: 1, fill: '#2b8a3e' },  // Park
    1: { stroke: '#40c057', width: 1, fill: '#1b4332' },  // Forest (darker)
    2: { stroke: '#a9e34b', width: 1, fill: '#5c940d' }   // Grassland (yellow-green)
  },
  // Railway subtypes (0x20)
  0x20: {
    0: { stroke: '#da77f2', width: 4, dash: [16,8] },  // Rail
    1: { stroke: '#e599f7', width: 3, dash: [8,8] }    // Subway (thinner, shorter dash)
  },
  // Building subtypes (0x30)
  0x30: {
    0: { stroke: '#868e96', width: 2, fill: '#495057' },  // Building
    1: { stroke: '#fab005', width: 2, fill: '#5c4a1d' },  // Industrial (brownish)
    2: { stroke: '#4dabf7', width: 2, fill: '#1c4a6e' },  // Commercial (blueish)
    3: { stroke: '#69db7c', width: 2, fill: '#2d4a3a' }   // Residential (greenish)
  }
};

// Get style for feature, using subtype if v6
function getFeatureStyle(type, subtype, version) {
  if (version >= 6 && SUBTYPE_COLORS[type] && SUBTYPE_COLORS[type][subtype]) {
    return SUBTYPE_COLORS[type][subtype];
  }
  return COLORS[type] || { stroke: '#888', width: 1 };
}

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
    showActions: true,
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

async function organizeMaps() {
  const statusEl = document.getElementById('maps-organize-status');
  if (statusEl) statusEl.textContent = 'Organizing...';
  try {
    const resp = await fetch('/api/maps/organize', { method: 'POST' });
    const data = await resp.json();
    if (!data || data.success !== true) {
      if (statusEl) statusEl.textContent = 'Organize failed';
      alert('Organize failed: ' + (data && data.error ? data.error : 'Unknown'));
      return;
    }
    if (statusEl) statusEl.textContent = `Moved ${data.moved}, Failed ${data.failed}`;
    if (mapsExplorer && typeof mapsExplorer.navigate === 'function') mapsExplorer.navigate('/maps');
  } catch (e) {
    if (statusEl) statusEl.textContent = 'Organize error';
    alert('Organize error: ' + e.message);
  }
}

// Load and parse a map file
async function loadMap(path) {
  try {
    const resp = await fetch('/api/files/view?name=' + encodeURIComponent(path));
    if (!resp.ok) throw new Error('Failed to load map');
    
    const buffer = await resp.arrayBuffer();
    currentMap = parseHWMap(buffer, path);
    
    if (currentMap) {
      try {
        const selResp = await fetch('/api/maps/select?file=' + encodeURIComponent(path), { credentials: 'include' });
        const selData = await selResp.json();
        if (!selData || selData.success !== true) {
          console.warn('[MAP] Device map select failed:', selData);
        }
      } catch (e) {
        console.warn('[MAP] Device map select error:', e);
      }
      document.getElementById('map-info').style.display = 'block';
      // Coordinates already in degrees from parser (divided by 10000000)
      const minLat = currentMap.minLat.toFixed(4);
      const maxLat = currentMap.maxLat.toFixed(4);
      const minLon = currentMap.minLon.toFixed(4);
      const maxLon = currentMap.maxLon.toFixed(4);
      
      // Count feature types for debug
      const typeCounts = {};
      const typeNames = {0x00:'Highway',0x01:'Major',0x02:'Minor',0x03:'Path',0x10:'Water',0x11:'Park',0x20:'Rail',0x30:'Building'};
      currentMap.features.forEach(f => { typeCounts[f.type] = (typeCounts[f.type]||0)+1; });
      let typeInfo = Object.entries(typeCounts).map(([t,c]) => `${typeNames[t]||('0x'+Number(t).toString(16))}:${c}`).join(', ');
      console.log('[MAP] Feature types:', typeCounts);
      
      document.getElementById('map-details').innerHTML = `
        <div style="margin-bottom:4px"><strong>File:</strong> ${currentMap.filename}</div>
        <div style="margin-bottom:4px"><strong>Region:</strong> ${currentMap.regionName}</div>
        <div style="margin-bottom:4px"><strong>Features:</strong> ${currentMap.features.length}</div>
        <div style="margin-bottom:4px;font-size:0.8rem;color:var(--panel-fg)"><strong>Types:</strong> ${typeInfo}</div>
        <div style="margin-bottom:4px"><strong>Lat:</strong> ${minLat}° to ${maxLat}°</div>
        <div><strong>Lon:</strong> ${minLon}° to ${maxLon}°</div>
      `;
      resetView();
      renderMap();
      loadWaypoints();
      loadMapFeatures();
      if (typeof updateRoutesList === 'function') updateRoutesList();
    }
  } catch (e) {
    console.error('Error loading map:', e);
    alert('Error loading map: ' + e.message);
  }
}

// Parse HWMAP binary format (v5)
function parseHWMap(buffer, filename) {
  const view = new DataView(buffer);
  const fileLen = buffer.byteLength;
  let offset = 0;

  const requireBytes = (need, what) => {
    if (offset + need > fileLen) {
      throw new Error(`[HWMap] Truncated reading ${what}: need=${need} at ${offset}, len=${fileLen}`);
    }
  };
  const readU8 = () => { requireBytes(1, 'u8'); return view.getUint8(offset++); };
  const readU16 = () => { requireBytes(2, 'u16'); const v = view.getUint16(offset, true); offset += 2; return v; };
  const readI32 = () => { requireBytes(4, 'i32'); const v = view.getInt32(offset, true); offset += 4; return v; };
  const readU32 = () => { requireBytes(4, 'u32'); const v = view.getUint32(offset, true); offset += 4; return v; };

  // Header (40 bytes)
  requireBytes(40, 'header');
  const magic = String.fromCharCode(view.getUint8(0), view.getUint8(1), view.getUint8(2), view.getUint8(3));
  if (magic !== 'HWMP') throw new Error('Invalid map file (bad magic)');
  offset = 4;

  const version = readU16();
  if (version !== 5 && version !== 6) throw new Error(`Unsupported map version: ${version} (need v5 or v6)`);
  const flags = readU16();

  const minLat = readI32();
  const minLon = readI32();
  const maxLat = readI32();
  const maxLon = readI32();

  const featureCount = readU32();
  const nameCount = readU16();

  let regionName = '';
  for (let i = 0; i < 8; i++) {
    const c = readU8();
    if (c !== 0) regionName += String.fromCharCode(c);
  }
  readU16(); // padding

  // Extract v5 tiling params from flags (matches generator encoding)
  // bits 0-1: tileGridCode (0=16, 1=32, 2=64), bits 2-6: haloPct (0-31), bits 7-10: quantBits-10
  const tileGridCode = flags & 0x03;
  const tileGridSize = tileGridCode === 0 ? 16 : tileGridCode === 2 ? 64 : 32;
  const haloPct = ((flags >> 2) & 0x1F) / 100.0;
  const quantBits = Math.min(16, ((flags >> 7) & 0x0F) + 10);
  const qMax = (1 << quantBits) - 1;
  const tileCount = tileGridSize * tileGridSize;

  console.log(`[MAP] v${version} tiled: ${tileGridSize}x${tileGridSize}, halo=${haloPct}, quantBits=${quantBits}`);
  const hdrSize = version >= 6 ? 6 : 5;  // v6 adds subtype byte

  // Name table
  const names = [];
  for (let i = 0; i < nameCount; i++) {
    const len = readU8();
    if (len > 63) throw new Error(`Invalid name length ${len}`);
    requireBytes(len, `name[${i}]`);
    let name = '';
    for (let j = 0; j < len; j++) name += String.fromCharCode(readU8());
    names.push(name);
  }

  // Tile directory: tileCount entries of (uint32 offset, uint32 payloadSize)
  // Note: featureCount is stored at the START of each tile's payload, not in directory
  const tileDir = [];
  for (let t = 0; t < tileCount; t++) {
    const tileOffset = readU32();
    const tilePayloadSize = readU32();
    tileDir.push({ offset: tileOffset, size: tilePayloadSize });
  }

  // Calculate tile geo bounds
  const mapWidth = maxLon - minLon;
  const mapHeight = maxLat - minLat;
  const tileW = mapWidth / tileGridSize;
  const tileH = mapHeight / tileGridSize;
  const haloW = tileW * haloPct;
  const haloH = tileH * haloPct;

  // Parse all tiles and dequantize points to microdegrees
  const features = [];
  for (let ty = 0; ty < tileGridSize; ty++) {
    for (let tx = 0; tx < tileGridSize; tx++) {
      const tileIdx = ty * tileGridSize + tx;
      const tile = tileDir[tileIdx];
      // Match preview parser: treat offset==0 as "no tile" too
      if (tile.size === 0 || tile.offset === 0) continue;

      // Tile halo bounds (for dequantization)
      const tileMinLon = minLon + tx * tileW - haloW;
      const tileMaxLon = minLon + (tx + 1) * tileW + haloW;
      const tileMinLat = minLat + ty * tileH - haloH;
      const tileMaxLat = minLat + (ty + 1) * tileH + haloH;
      const haloLonSpan = tileMaxLon - tileMinLon;
      const haloLatSpan = tileMaxLat - tileMinLat;

      offset = tile.offset;
      const tileEnd = tile.offset + tile.size;
      
      // Feature count is at the START of each tile's payload
      const tileFeatureCount = readU16();
      
      for (let f = 0; f < tileFeatureCount && offset + hdrSize <= tileEnd; f++) {
        const type = readU8();
        const subtype = version >= 6 ? readU8() : 0;  // v6 adds subtype byte
        const nameIndex = readU16();
        const pointCount = readU16();

        // Bounds check: ensure we have enough data for all points
        const bytesNeeded = pointCount * 4;
        if (offset + bytesNeeded > tileEnd) {
          console.warn(`[MAP] Tile ${tileIdx} feature ${f}: truncated (need ${bytesNeeded} bytes, have ${tileEnd - offset})`);
          break;
        }

        const points = [];
        for (let p = 0; p < pointCount; p++) {
          const qLat = readU16();
          const qLon = readU16();
          // Dequantize: match preview renderer (qMax depends on quantBits)
          const latMicro = tileMinLat + (qLat / qMax) * haloLatSpan;
          const lonMicro = tileMinLon + (qLon / qMax) * haloLonSpan;
          points.push({ lat: latMicro / 10000000, lon: lonMicro / 10000000 });
        }

        if (points.length >= 2) {
          const featureName = (nameIndex !== 0xFFFF && nameIndex < names.length) ? names[nameIndex] : null;
          features.push({ type, subtype, points, name: featureName, tileIdx });
        }
      }
    }
  }

  console.log(`[MAP] Parsed ${features.length} features from ${tileCount} tiles`);

  // Build spatial index (matching preview exactly)
  const HWMAP_GRID_SIZE = 8;
  const buildSpatialIndex = () => {
    const gridSize = HWMAP_GRID_SIZE;
    const mapMinLatDeg = minLat / 10000000;
    const mapMinLonDeg = minLon / 10000000;
    const mapMaxLatDeg = maxLat / 10000000;
    const mapMaxLonDeg = maxLon / 10000000;
    const cellWidth = (mapMaxLonDeg - mapMinLonDeg) / gridSize;
    const cellHeight = (mapMaxLatDeg - mapMinLatDeg) / gridSize;
    const cells = Array(gridSize * gridSize).fill(null).map(() => []);

    for (let fi = 0; fi < features.length; fi++) {
      const f = features[fi];
      if (!f || !f.points || f.points.length < 2) continue;

      let fMinLat = Infinity, fMaxLat = -Infinity;
      let fMinLon = Infinity, fMaxLon = -Infinity;
      for (const p of f.points) {
        if (p.lat < fMinLat) fMinLat = p.lat;
        if (p.lat > fMaxLat) fMaxLat = p.lat;
        if (p.lon < fMinLon) fMinLon = p.lon;
        if (p.lon > fMaxLon) fMaxLon = p.lon;
      }

      let minCellX = Math.floor((fMinLon - mapMinLonDeg) / cellWidth);
      let maxCellX = Math.floor((fMaxLon - mapMinLonDeg) / cellWidth);
      let minCellY = Math.floor((fMinLat - mapMinLatDeg) / cellHeight);
      let maxCellY = Math.floor((fMaxLat - mapMinLatDeg) / cellHeight);

      minCellX = Math.max(0, Math.min(gridSize - 1, minCellX));
      maxCellX = Math.max(0, Math.min(gridSize - 1, maxCellX));
      minCellY = Math.max(0, Math.min(gridSize - 1, minCellY));
      maxCellY = Math.max(0, Math.min(gridSize - 1, maxCellY));

      for (let cy = minCellY; cy <= maxCellY; cy++) {
        for (let cx = minCellX; cx <= maxCellX; cx++) {
          cells[cy * gridSize + cx].push(fi);
        }
      }
    }

    return { gridSize, cells };
  };

  // Convert bounds to degrees (matching preview renderer)
  return {
    filename: filename.split('/').pop(),
    version, flags,
    minLat: minLat / 10000000,
    minLon: minLon / 10000000,
    maxLat: maxLat / 10000000,
    maxLon: maxLon / 10000000,
    regionName, features, names, tileGridSize, haloPct, quantBits,
    spatialIndex: buildSpatialIndex()
  };
}

// Render map to canvas
function renderMap() {
  const canvas = document.getElementById('map-canvas');
  const ctx = canvas.getContext('2d');
  
  // Enable anti-aliasing
  ctx.imageSmoothingEnabled = true;
  ctx.imageSmoothingQuality = 'high';
  
  // Clear with theme background color
  const bgColor = getComputedStyle(document.documentElement).getPropertyValue('--bg').trim() || '#1a1a2e';
  ctx.fillStyle = bgColor;
  ctx.fillRect(0, 0, canvas.width, canvas.height);
  
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
  
  // Scale to fit canvas (90% of canvas, matching preview exactly)
  const scaleX = (canvas.width * 0.9) / mapWidth;
  const scaleY = (canvas.height * 0.9) / mapHeight;
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

  const isOn = (id, def = true) => {
    const el = document.getElementById(id);
    return el ? el.checked !== false : def;
  };

  const layerEnabled = (type, subtype) => {
    const v = m.version || 5;
    const st = subtype || 0;

    if (type === 0x12) return isOn('layer-landmask', true);

    if (v < 6) {
      if (type === 0x00) return isOn('layer-road-motorway', true) || isOn('layer-road-trunk', true);
      if (type === 0x01) return isOn('layer-road-primary', true) || isOn('layer-road-secondary', true);
      if (type === 0x02) return isOn('layer-road-tertiary', true) || isOn('layer-road-residential', true) || isOn('layer-road-service', true);
      if (type === 0x03) return isOn('layer-path-footway', true) || isOn('layer-path-cycleway', true) || isOn('layer-path-track', true);
      if (type === 0x10) return isOn('layer-water-lake', true) || isOn('layer-water-river', true) || isOn('layer-water-coast', true);
      if (type === 0x11) return isOn('layer-park-park', true) || isOn('layer-park-forest', true) || isOn('layer-park-grass', true);
      if (type === 0x20) return isOn('layer-rail-rail', true) || isOn('layer-rail-subway', true);
      if (type === 0x21) return isOn('layer-transit-bus', true);
      if (type === 0x22) return isOn('layer-transit-ferry', true);
      if (type === 0x40) return isOn('layer-transit-stations', true);
      if (type === 0x30) return isOn('layer-building', true) || isOn('layer-industrial', true) || isOn('layer-commercial', true) || isOn('layer-residential-area', true);
      return true;
    }

    if (type === 0x00) return st === 0 ? isOn('layer-road-motorway', true) : isOn('layer-road-trunk', true);
    if (type === 0x01) return st === 0 ? isOn('layer-road-primary', true) : isOn('layer-road-secondary', true);
    if (type === 0x02) return st === 0 ? isOn('layer-road-tertiary', true) : (st === 1 ? isOn('layer-road-residential', true) : isOn('layer-road-service', true));
    if (type === 0x03) return st === 0 ? isOn('layer-path-footway', true) : (st === 1 ? isOn('layer-path-cycleway', true) : isOn('layer-path-track', true));
    if (type === 0x10) return st === 0 ? isOn('layer-water-lake', true) : (st === 1 ? isOn('layer-water-river', true) : isOn('layer-water-coast', true));
    if (type === 0x11) return st === 0 ? isOn('layer-park-park', true) : (st === 1 ? isOn('layer-park-forest', true) : isOn('layer-park-grass', true));
    if (type === 0x20) return st === 0 ? isOn('layer-rail-rail', true) : isOn('layer-rail-subway', true);
    if (type === 0x21) return isOn('layer-transit-bus', true);
    if (type === 0x22) return isOn('layer-transit-ferry', true);
    if (type === 0x40) return isOn('layer-transit-stations', true);
    if (type === 0x30) return st === 0 ? isOn('layer-building', true) : (st === 1 ? isOn('layer-industrial', true) : (st === 2 ? isOn('layer-commercial', true) : isOn('layer-residential-area', true)));

    return true;
  };

  const anyWaterEnabled = layerEnabled(0x10, 0) || layerEnabled(0x10, 1) || layerEnabled(0x10, 2);
  const anyParkEnabled = layerEnabled(0x11, 0) || layerEnabled(0x11, 1) || layerEnabled(0x11, 2);
  
  // Check if map has land mask features (coastal map)
  const hasLandMask = m.features.some(f => f.type === 0x12) && layerEnabled(0x12, 0);
  
  // ========== RENDERING PIPELINE ==========
  // Strategy for maps WITH land masks: water polygons carve water, land masks draw land
  // Strategy for maps WITHOUT land masks: fill tan background, water carves out
  // Coordinates are now in DEGREES (converted in parser)
  
  // 1. Fill background with LAND color (tan) - ONLY if no land masks
  // When hasLandMask=true, the land mask polygons themselves draw the land areas.
  // This prevents filling areas outside the selection (e.g., New Jersey when only Staten Island selected)
  if (!hasLandMask) {
    ctx.fillStyle = '#c9b896';
    ctx.beginPath();
    const corners = [
      toCanvas(m.minLat, m.minLon),
      toCanvas(m.minLat, m.maxLon),
      toCanvas(m.maxLat, m.maxLon),
      toCanvas(m.maxLat, m.minLon)
    ];
    ctx.moveTo(corners[0].x, corners[0].y);
    for (let i = 1; i < corners.length; i++) ctx.lineTo(corners[i].x, corners[i].y);
    ctx.closePath();
    ctx.fill();
  }
  
  // Thresholds for artifact detection (in degrees, matching preview)
  const latThreshold = Math.min(mapHeight * 0.3, 0.02);
  const lonThreshold = Math.min(mapWidth * 0.3, 0.02);
  const EDGE_EPS = 0.001;
  const FULL_EPS = Math.min(mapHeight, mapWidth) * 0.005;
  
  // 2. Draw ALL water polygons (carve out water from tan background)
  if (anyWaterEnabled) {
    const waterFeatures = m.features.filter(f => f.type === 0x10);
    for (const feature of waterFeatures) {
      if (!layerEnabled(0x10, feature.subtype || 0)) continue;
      const pts = feature.points;
      if (pts.length < 2) continue;
      
      // Check if closed (0.01 degrees threshold)
      const isClosed = Math.abs(pts[0].lat - pts[pts.length-1].lat) < 0.01 &&
                       Math.abs(pts[0].lon - pts[pts.length-1].lon) < 0.01;

      if (!isClosed) {
        const style = getFeatureStyle(feature.type, feature.subtype || 0, m.version);
        if (!style.width) continue;
        ctx.beginPath();
        ctx.strokeStyle = style.stroke;
        let lineScale = 1;
        if (zoom > 1) {
          lineScale = Math.max(0.25, 1 / Math.sqrt(zoom));
        } else if (zoom < 1) {
          lineScale = Math.max(0.35, zoom);
        }
        ctx.lineWidth = style.width * lineScale;
        if (style.dash) ctx.setLineDash(style.dash);
        else ctx.setLineDash([]);
        const first = toCanvas(pts[0].lat, pts[0].lon);
        ctx.moveTo(first.x, first.y);
        for (let i = 1; i < pts.length; i++) {
          const p = toCanvas(pts[i].lat, pts[i].lon);
          ctx.lineTo(p.x, p.y);
        }
        ctx.stroke();
        ctx.setLineDash([]);
        continue;
      }
      
      // Skip diagonal artifacts
      const isNearBoundary = (pt) => (
        Math.abs(pt.lat - m.minLat) < EDGE_EPS ||
        Math.abs(pt.lat - m.maxLat) < EDGE_EPS ||
        Math.abs(pt.lon - m.minLon) < EDGE_EPS ||
        Math.abs(pt.lon - m.maxLon) < EDGE_EPS
      );
      let hasDiagonal = false;
      for (let i = 1; i < pts.length; i++) {
        const latDiff = Math.abs(pts[i].lat - pts[i-1].lat);
        const lonDiff = Math.abs(pts[i].lon - pts[i-1].lon);
        if (latDiff > latThreshold && lonDiff > lonThreshold && isNearBoundary(pts[i]) && isNearBoundary(pts[i-1])) {
          hasDiagonal = true;
          break;
        }
      }
      if (hasDiagonal) continue;
      
      // Skip full-coverage artifacts (v5)
      let pMinLat = Infinity, pMaxLat = -Infinity, pMinLon = Infinity, pMaxLon = -Infinity;
      for (const pt of pts) {
        if (pt.lat < pMinLat) pMinLat = pt.lat;
        if (pt.lat > pMaxLat) pMaxLat = pt.lat;
        if (pt.lon < pMinLon) pMinLon = pt.lon;
        if (pt.lon > pMaxLon) pMaxLon = pt.lon;
      }
      const polyLatCov = (pMaxLat - pMinLat) / mapHeight;
      const polyLonCov = (pMaxLon - pMinLon) / mapWidth;
      const touchesNorth = pts.some(p => Math.abs(p.lat - m.maxLat) < FULL_EPS);
      const touchesSouth = pts.some(p => Math.abs(p.lat - m.minLat) < FULL_EPS);
      const touchesWest = pts.some(p => Math.abs(p.lon - m.minLon) < FULL_EPS);
      const touchesEast = pts.some(p => Math.abs(p.lon - m.maxLon) < FULL_EPS);
      const touchesAll = touchesNorth && touchesSouth && touchesWest && touchesEast;
      if (touchesAll && polyLatCov > 0.995 && polyLonCov > 0.995) continue;
      
      const style = getFeatureStyle(feature.type, feature.subtype || 0, m.version);
      ctx.beginPath();
      ctx.fillStyle = style.fill || '#1864ab';
      const first = toCanvas(pts[0].lat, pts[0].lon);
      ctx.moveTo(first.x, first.y);
      for (let i = 1; i < pts.length; i++) {
        const p = toCanvas(pts[i].lat, pts[i].lon);
        ctx.lineTo(p.x, p.y);
      }
      ctx.closePath();
      ctx.fill();
      ctx.strokeStyle = style.fill || '#1864ab';
      ctx.lineWidth = 1.25;
      ctx.setLineDash([]);
      ctx.stroke();
    }
  }
  
  // 3. Draw ALL land masks on top (restore land areas)
  // EXACTLY matching preview's drawLandMasks function with debugging
  if (hasLandMask) {
    const landMaskFeatures = m.features.filter(f => f.type === 0x12);
    let skippedDiagonal = 0, skippedFullCoverage = 0, skippedNotClosed = 0, fillCount = 0;
    const validLandMasks = [];
    
    for (const feature of landMaskFeatures) {
      const pts = feature.points;
      if (pts.length < 3) continue;
      
      // Check if closed
      const isClosed = Math.abs(pts[0].lat - pts[pts.length-1].lat) < 0.01 &&
                       Math.abs(pts[0].lon - pts[pts.length-1].lon) < 0.01;
      if (!isClosed) { skippedNotClosed++; continue; }
      
      // Calculate polygon bounds
      let pMinLat = Infinity, pMaxLat = -Infinity, pMinLon = Infinity, pMaxLon = -Infinity;
      for (const pt of pts) {
        if (pt.lat < pMinLat) pMinLat = pt.lat;
        if (pt.lat > pMaxLat) pMaxLat = pt.lat;
        if (pt.lon < pMinLon) pMinLon = pt.lon;
        if (pt.lon > pMaxLon) pMaxLon = pt.lon;
      }
      const polyLatCov = (pMaxLat - pMinLat) / mapHeight;
      const polyLonCov = (pMaxLon - pMinLon) / mapWidth;
      const coversNearlyAll = polyLatCov > 0.98 && polyLonCov > 0.98;
      const coversEssentiallyAll = polyLatCov > 0.995 && polyLonCov > 0.995;
      
      // Check if polygon touches edges
      const touchesNorth = pts.some(p => Math.abs(p.lat - m.maxLat) < FULL_EPS);
      const touchesSouth = pts.some(p => Math.abs(p.lat - m.minLat) < FULL_EPS);
      const touchesWest = pts.some(p => Math.abs(p.lon - m.minLon) < FULL_EPS);
      const touchesEast = pts.some(p => Math.abs(p.lon - m.maxLon) < FULL_EPS);
      const touchesAllEdges = touchesNorth && touchesSouth && touchesWest && touchesEast;
      
      // Check if polygon is mostly boundary points
      const boundaryEps = FULL_EPS * 2;
      let boundaryPtCount = 0;
      for (const p of pts) {
        const d = Math.min(
          Math.abs(p.lat - m.minLat),
          Math.abs(p.lat - m.maxLat),
          Math.abs(p.lon - m.minLon),
          Math.abs(p.lon - m.maxLon)
        );
        if (d < boundaryEps) boundaryPtCount++;
      }
      const boundaryFrac = boundaryPtCount / pts.length;
      const isMostlyBoundary = boundaryFrac > 0.98;
      
      // Calculate actual polygon area for filtering
      let polyArea = 0;
      for (let i = 0; i < pts.length; i++) {
        const j = (i + 1) % pts.length;
        polyArea += pts[i].lon * pts[j].lat;
        polyArea -= pts[j].lon * pts[i].lat;
      }
      polyArea = Math.abs(polyArea / 2);
      const selectionArea = mapHeight * mapWidth;
      const areaRatio = polyArea / selectionArea;
      
      // Skip full-coverage artifacts: >98% bbox AND >40% actual area
      if (touchesAllEdges && coversNearlyAll && areaRatio > 0.40) {
        skippedFullCoverage++;
        console.log(`[LAND] Skipping large land polygon: ${pts.length} pts, area=${(areaRatio*100).toFixed(1)}%`);
        continue;
      }
      
      // Skip diagonal artifacts
      const isNearBoundary = (pt) => (
        Math.abs(pt.lat - m.minLat) < EDGE_EPS ||
        Math.abs(pt.lat - m.maxLat) < EDGE_EPS ||
        Math.abs(pt.lon - m.minLon) < EDGE_EPS ||
        Math.abs(pt.lon - m.maxLon) < EDGE_EPS
      );
      let hasDiagonal = false;
      for (let i = 1; i < pts.length; i++) {
        const latDiff = Math.abs(pts[i].lat - pts[i-1].lat);
        const lonDiff = Math.abs(pts[i].lon - pts[i-1].lon);
        if (latDiff > latThreshold && lonDiff > lonThreshold && isNearBoundary(pts[i]) && isNearBoundary(pts[i-1])) {
          hasDiagonal = true;
          break;
        }
      }
      if (hasDiagonal) { skippedDiagonal++; continue; }
      
      // Collect valid land masks for combined drawing
      validLandMasks.push(pts);
      fillCount++;
    }
    if (skippedDiagonal > 0 || skippedFullCoverage > 0) {
      console.log(`[LAND] Skipped: ${skippedDiagonal} diagonal, ${skippedFullCoverage} full-coverage`);
    }
    
    // Draw ALL land masks as a single combined path to eliminate internal edge artifacts
    if (validLandMasks.length > 0) {
      ctx.beginPath();
      ctx.fillStyle = '#c9b896';
      for (const pts of validLandMasks) {
        const first = toCanvas(pts[0].lat, pts[0].lon);
        ctx.moveTo(first.x, first.y);
        for (let i = 1; i < pts.length; i++) {
          const p = toCanvas(pts[i].lat, pts[i].lon);
          ctx.lineTo(p.x, p.y);
        }
        ctx.closePath();
      }
      ctx.fill();
    }
  }
  
  // 5. Draw parks (green areas on top)
  if (anyParkEnabled) {
    const parkFeatures = m.features.filter(f => f.type === 0x11);
    for (const feature of parkFeatures) {
      if (!layerEnabled(0x11, feature.subtype || 0)) continue;
      const pts = feature.points;
      if (pts.length < 3) continue;
      
      const isClosed = Math.abs(pts[0].lat - pts[pts.length-1].lat) < 0.01 &&
                       Math.abs(pts[0].lon - pts[pts.length-1].lon) < 0.01;
      if (!isClosed) continue;
      
      const style = getFeatureStyle(feature.type, feature.subtype || 0, m.version);
      ctx.beginPath();
      ctx.fillStyle = style.fill || '#2b8a3e';
      const first = toCanvas(pts[0].lat, pts[0].lon);
      ctx.moveTo(first.x, first.y);
      for (let i = 1; i < pts.length; i++) {
        const p = toCanvas(pts[i].lat, pts[i].lon);
        ctx.lineTo(p.x, p.y);
      }
      ctx.closePath();
      ctx.fill();
    }
  }
  
  // Render features by type (back to front matching preview renderer):
  // Water (0x10), Land mask (0x12), Parks (0x11) already rendered in dedicated passes above
  // Now render: buildings, paths, minor, major, ferry, bus, railway, highways, stations
  // Use two-pass for highways to avoid lumpy overlaps at intersections
  const renderOrder = [0x30, 0x03, 0x02, 0x01, 0x22, 0x21, 0x20, 0x00, 0x40];
  
  // First pass: draw all highway casings (outlines) together
  ctx.beginPath();
  ctx.strokeStyle = '#8b0000';  // Dark red casing
  // Scale line width EXACTLY matching generator preview
  let casingLineScale = 1;
  if (zoom > 1) {
    casingLineScale = Math.max(0.3, 1 / Math.sqrt(zoom));
  } else if (zoom < 1) {
    casingLineScale = Math.max(0.4, zoom);
  }
  ctx.lineWidth = (COLORS[0x00].width + 4) * casingLineScale;
  ctx.setLineDash([]);
  // Match preview: iterate ALL highways without culling for casings
  for (const feature of m.features) {
    if (feature.type !== 0x00 || feature.points.length < 2) continue;
    if (!layerEnabled(0x00, feature.subtype || 0)) continue;
    const first = toCanvas(feature.points[0].lat, feature.points[0].lon);
    ctx.moveTo(first.x, first.y);
    for (let i = 1; i < feature.points.length; i++) {
      const p = toCanvas(feature.points[i].lat, feature.points[i].lon);
      ctx.lineTo(p.x, p.y);
    }
  }
  ctx.stroke();
  
  // Draw features matching preview's drawMapFeatures with LOD fade
  // Preview does NOT use viewport culling - iterates all features with layer toggles only
  for (const targetType of renderOrder) {
    for (const feature of m.features) {
      if (feature.type !== targetType) continue;
      if (feature.points.length < 2) continue;
      if (!layerEnabled(feature.type, feature.subtype || 0)) continue;
      
      const style = getFeatureStyle(feature.type, feature.subtype || 0, m.version);
      const pts = feature.points;
      
      // LOD with smooth fade-in transitions (matching preview)
      let fadeOpacity = 1.0;
      let fadeLineScale = 1.0;
      
      // Minor roads (0x02): fade in from 0.2 to 0.4
      if (feature.type === 0x02) {
        if (zoom < 0.2) continue;
        if (zoom < 0.4) {
          const t = (zoom - 0.2) / 0.2;
          fadeOpacity = t * t;
          fadeLineScale = 0.3 + t * 0.7;
        }
      }
      
      // Paths (0x03): fade in from 0.7 to 1.2
      if (feature.type === 0x03) {
        if (zoom < 0.7) continue;
        if (zoom < 1.2) {
          const t = (zoom - 0.7) / 0.5;
          fadeOpacity = t * t;
          fadeLineScale = 0.2 + t * 0.8;
        }
      }
      
      // Buildings (0x30): only show when zoomed in enough to see individual buildings
      // At low zoom, buildings overlap and create ugly blobs
      if (feature.type === 0x30) {
        if (zoom < 2.0) continue;
        if (zoom < 3.0) {
          const t = (zoom - 2.0) / 1.0;
          fadeOpacity = t * t;
          fadeLineScale = 0.3 + t * 0.7;
        }
      }
      
      ctx.beginPath();
      ctx.strokeStyle = style.stroke;
      // Scale line width based on zoom - EXACTLY matching generator preview
      let lineScale = 1;
      // Only apply thinning to highways/trunk (0x00, 0x01) - paths should stay visible
      if (feature.type === 0x00 || feature.type === 0x01) {
        if (zoom > 1) {
          // Reduce line width progressively as zoom increases for highways
          lineScale = Math.max(0.3, 1 / Math.sqrt(zoom));
        } else if (zoom < 1) {
          lineScale = Math.max(0.4, zoom);
        }
      } else if (feature.type === 0x22) {
        if (zoom > 1) {
          lineScale = Math.max(0.25, 1 / Math.sqrt(zoom));
        } else if (zoom < 1) {
          lineScale = Math.max(0.35, zoom);
        }
      } else if (feature.type === 0x03) {
        // Paths: keep full width or slightly thicker when zoomed in
        if (zoom < 1) {
          lineScale = Math.max(0.5, zoom);
        }
      } else if (zoom < 1) {
        // Other features: reduce when zoomed out
        lineScale = Math.max(0.4, zoom);
      }
      ctx.lineWidth = style.width * lineScale * fadeLineScale;
      const alpha = feature.type === 0x22 ? (fadeOpacity * 0.55) : fadeOpacity;
      ctx.globalAlpha = alpha;
      if (style.dash) ctx.setLineDash(style.dash);
      else ctx.setLineDash([]);
      
      const first = toCanvas(pts[0].lat, pts[0].lon);
      ctx.moveTo(first.x, first.y);
      for (let i = 1; i < pts.length; i++) {
        const p = toCanvas(pts[i].lat, pts[i].lon);
        ctx.lineTo(p.x, p.y);
      }
      
      // Fill closed buildings only (water, parks, land masks handled in dedicated passes)
      const isClosed = pts.length >= 3 && (
        Math.abs(pts[0].lat - pts[pts.length-1].lat) < 0.01 &&
        Math.abs(pts[0].lon - pts[pts.length-1].lon) < 0.01
      );
      
      if (style.fill && isClosed && feature.type === 0x30) {
        ctx.closePath();
        ctx.fillStyle = style.fill;
        ctx.globalAlpha = fadeOpacity * 0.6;
        ctx.fill();
      }
      
      if (style.width > 0) ctx.stroke();
      ctx.globalAlpha = 1.0;
    }
  }
  
  // Draw street/feature names with zoom-based visibility
  ctx.textAlign = 'center';
  ctx.textBaseline = 'middle';
  
  const drawnLabels = [];  // Track label positions to avoid overlap
  const drawnNames = new Map();  // Track how many times each name has been drawn (for v5 tiled dedup)
  const maxNameInstances = 2;  // Max times same name can appear (for long roads spanning tiles)
  
  for (const feature of m.features) {
    if (!feature.name || feature.points.length < 2) continue;
    if (!layerEnabled(feature.type, feature.subtype || 0)) continue;

    const displayName = String(feature.name).replace(/\0/g, '').trim();
    if (!displayName) continue;
    const nameKey = displayName.toLowerCase();
    
    // V5 tiled format: same road appears in multiple tiles - limit name repetition
    const nameCount = drawnNames.get(nameKey) || 0;
    if (nameCount >= maxNameInstances) continue;  // Already drawn enough instances of this name
    
    // Zoom-based visibility by feature type
    let minZoom = 20;  // Default: don't show
    let fontSize = 18;
    
    if (feature.type === 0x00) {
      // Highways: visible at 5x+
      minZoom = 5;
      fontSize = zoom >= 10 ? 28 : 22;
    } else if (feature.type === 0x01) {
      // Major roads: visible at 7x+
      minZoom = 7;
      fontSize = zoom >= 12 ? 24 : 20;
    } else if (feature.type === 0x02) {
      // Minor roads: visible at 10x+
      minZoom = 10;
      fontSize = 18;
    } else if (feature.type === 0x03) {
      // Paths: visible at 15x+
      minZoom = 15;
      fontSize = 16;
    } else if (feature.type === 0x10) {
      // Water: visible at 8x+
      minZoom = 8;
      fontSize = zoom >= 12 ? 24 : 20;
    } else if (feature.type === 0x11) {
      // Parks: visible at 8x+
      minZoom = 8;
      fontSize = zoom >= 12 ? 24 : 20;
    } else if (feature.type === 0x30) {
      // Buildings: visible at 8x+
      minZoom = 8;
      fontSize = zoom >= 12 ? 20 : 16;
    }
    
    if (zoom < minZoom) continue;
    
    ctx.font = fontSize + 'px system-ui, sans-serif';
    
    // Get midpoint of feature for label placement
    const midIdx = Math.floor(feature.points.length / 2);
    const midPt = feature.points[midIdx];
    const pos = toCanvas(midPt.lat, midPt.lon);
    
    // Skip if off-screen
    if (pos.x < 0 || pos.x > canvas.width || pos.y < 0 || pos.y > canvas.height) continue;
    
    // Skip if too close to another label
    const labelWidth = ctx.measureText(displayName).width;
    let tooClose = false;
    for (const lbl of drawnLabels) {
      const dx = pos.x - lbl.x;
      const dy = pos.y - lbl.y;
      if (Math.abs(dx) < (labelWidth + lbl.w) / 2 + 20 && Math.abs(dy) < 30) {
        tooClose = true;
        break;
      }
    }
    if (tooClose) continue;
    
    // Draw text with outline for visibility
    ctx.strokeStyle = 'rgba(0,0,0,0.8)';
    ctx.lineWidth = 4;
    ctx.strokeText(displayName, pos.x, pos.y);
    
    // Color based on feature type
    if (feature.type === 0x00) ctx.fillStyle = '#ff6b6b';
    else if (feature.type === 0x01) ctx.fillStyle = '#ffd93d';
    else if (feature.type === 0x10) ctx.fillStyle = '#4dabf7';
    else if (feature.type === 0x11) ctx.fillStyle = '#69db7c';
    else if (feature.type === 0x30) ctx.fillStyle = '#b197fc';
    else ctx.fillStyle = '#ffffff';
    
    ctx.fillText(displayName, pos.x, pos.y);
    drawnLabels.push({ x: pos.x, y: pos.y, w: labelWidth });
    drawnNames.set(nameKey, (drawnNames.get(nameKey) || 0) + 1);  // Track name usage for v5 dedup
  }
  
  // Draw waypoints (from device /api/waypoints)
  if (waypoints && waypoints.length > 0) {
    waypoints.forEach((wp, idx) => {
      if (wp.lat >= m.minLat && wp.lat <= m.maxLat &&
          wp.lon >= m.minLon && wp.lon <= m.maxLon) {
        const pos = toCanvas(wp.lat, wp.lon);
        const isTarget = idx === targetWaypointIndex;
        const isSelected = idx === selectedWaypointIndex;
        
        // Draw waypoint marker
        ctx.beginPath();
        ctx.arc(pos.x, pos.y, isTarget ? 8 : 6, 0, Math.PI * 2);
        ctx.fillStyle = isTarget ? '#ff6b6b' : '#ffd93d';
        ctx.fill();
        ctx.strokeStyle = '#fff';
        ctx.lineWidth = 2;
        ctx.stroke();

        if (isSelected) {
          ctx.beginPath();
          ctx.arc(pos.x, pos.y, (isTarget ? 8 : 6) + 6, 0, Math.PI * 2);
          ctx.strokeStyle = 'var(--accent)';
          ctx.lineWidth = 3;
          ctx.stroke();
        }
        
        // Draw name label
        ctx.fillStyle = '#fff';
        ctx.font = '10px system-ui, sans-serif';
        ctx.textAlign = 'center';
        ctx.fillText(wp.name, pos.x, pos.y - 12);
      }
    });
  }
  
  // Draw GPS track
  if (gpsTrack && gpsTrack.length > 1) {
    ctx.strokeStyle = trackColor;
    ctx.lineWidth = 3;
    ctx.lineCap = 'round';
    ctx.lineJoin = 'round';
    ctx.shadowColor = trackColor;
    ctx.shadowBlur = 4;
    
    ctx.beginPath();
    let firstPoint = true;
    for (const point of gpsTrack) {
      if (point.lat >= m.minLat && point.lat <= m.maxLat &&
          point.lon >= m.minLon && point.lon <= m.maxLon) {
        const pos = toCanvas(point.lat, point.lon);
        if (firstPoint) {
          ctx.moveTo(pos.x, pos.y);
          firstPoint = false;
        } else {
          ctx.lineTo(pos.x, pos.y);
        }
      }
    }
    ctx.stroke();
    ctx.shadowBlur = 0;
    
    // Draw start point (green)
    if (gpsTrack.length > 0) {
      const start = gpsTrack[0];
      const startLatMicro = start.lat * 1e6;
      const startLonMicro = start.lon * 1e6;
      if (startLatMicro >= m.minLat && startLatMicro <= m.maxLat &&
          startLonMicro >= m.minLon && startLonMicro <= m.maxLon) {
        const pos = toCanvas(startLatMicro, startLonMicro);
        ctx.beginPath();
        ctx.arc(pos.x, pos.y, 6, 0, Math.PI * 2);
        ctx.fillStyle = '#69db7c';
        ctx.fill();
        ctx.strokeStyle = '#fff';
        ctx.lineWidth = 2;
        ctx.stroke();
      }
    }
    
    // Draw end point (red)
    if (gpsTrack.length > 1) {
      const end = gpsTrack[gpsTrack.length - 1];
      const endLatMicro = end.lat * 1e6;
      const endLonMicro = end.lon * 1e6;
      if (endLatMicro >= m.minLat && endLatMicro <= m.maxLat &&
          endLonMicro >= m.minLon && endLonMicro <= m.maxLon) {
        const pos = toCanvas(endLatMicro, endLonMicro);
        ctx.beginPath();
        ctx.arc(pos.x, pos.y, 6, 0, Math.PI * 2);
        ctx.fillStyle = '#ff6b6b';
        ctx.fill();
        ctx.strokeStyle = '#fff';
        ctx.lineWidth = 2;
        ctx.stroke();
      }
    }
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
  
  // Draw selected feature pin
  if (selectedFeature) {
    const pos = toCanvas(selectedFeature.pinLat, selectedFeature.pinLon);
    
    // Pin shadow
    ctx.beginPath();
    ctx.ellipse(pos.x, pos.y + 18, 8, 3, 0, 0, Math.PI * 2);
    ctx.fillStyle = 'rgba(0,0,0,0.3)';
    ctx.fill();
    
    // Pin body (teardrop shape)
    ctx.beginPath();
    ctx.moveTo(pos.x, pos.y + 16);
    ctx.bezierCurveTo(pos.x - 12, pos.y - 4, pos.x - 12, pos.y - 20, pos.x, pos.y - 24);
    ctx.bezierCurveTo(pos.x + 12, pos.y - 20, pos.x + 12, pos.y - 4, pos.x, pos.y + 16);
    ctx.fillStyle = '#4dabf7';
    ctx.fill();
    ctx.strokeStyle = '#fff';
    ctx.lineWidth = 2;
    ctx.stroke();
    
    // Pin inner circle
    ctx.beginPath();
    ctx.arc(pos.x, pos.y - 8, 5, 0, Math.PI * 2);
    ctx.fillStyle = '#fff';
    ctx.fill();
  }
  
  ctx.setLineDash([]);
  document.getElementById('zoom-info').textContent = 'Zoom: ' + zoom.toFixed(1) + 'x';
  document.getElementById('rotation-info').textContent = 'Rot: ' + rotation + '°';
}

// Controls - adjust pan to maintain center point when zooming
function zoomIn() {
  const factor = 1.5;
  const newZoom = Math.min(zoom * factor, 30);
  const zoomRatio = newZoom / zoom;
  panX *= zoomRatio;
  panY *= zoomRatio;
  zoom = newZoom;
  renderMap();
}
function zoomOut() {
  const factor = 1.5;
  const newZoom = Math.max(zoom / factor, 0.5);
  const zoomRatio = newZoom / zoom;
  panX *= zoomRatio;
  panY *= zoomRatio;
  zoom = newZoom;
  renderMap();
}
function rotateLeft() { rotation = (rotation - 15 + 360) % 360; renderMap(); }
function rotateRight() { rotation = (rotation + 15) % 360; renderMap(); }
function resetView() { zoom = 10; panX = 0; panY = 0; rotation = 0; renderMap(); }
function centerOnGPS() {
  if (!currentMap || gpsLat === null) return;
  const m = currentMap;
  const canvas = document.getElementById('map-canvas');
  const mapWidth = m.maxLon - m.minLon;
  const mapHeight = m.maxLat - m.minLat;
  const scaleX = (canvas.width * 0.9) / mapWidth;
  const scaleY = (canvas.height * 0.9) / mapHeight;
  const scale = Math.min(scaleX, scaleY) * zoom;
  
  const mapCenterX = (m.minLon + m.maxLon) / 2;
  const mapCenterY = (m.minLat + m.maxLat) / 2;
  
  // Match toCanvas() math (degrees, center-based), including rotation
  let x = (gpsLon - mapCenterX) * scale;
  let y = -((gpsLat - mapCenterY) * scale);
  if (rotation !== 0) {
    const rad = rotation * Math.PI / 180;
    const cos = Math.cos(rad);
    const sin = Math.sin(rad);
    const rx = x * cos - y * sin;
    const ry = x * sin + y * cos;
    x = rx;
    y = ry;
  }
  
  // Center GPS dot on screen
  panX = -x;
  panY = -y;
  renderMap();
}

// Mouse controls for panning
const canvas = document.getElementById('map-canvas');
function getCanvasXYFromMouseEvent(e) {
  const rect = canvas.getBoundingClientRect();
  const scaleX = canvas.width / rect.width;
  const scaleY = canvas.height / rect.height;
  return {
    x: (e.clientX - rect.left) * scaleX,
    y: (e.clientY - rect.top) * scaleY
  };
}

function geoToCanvasForHitTest(lat, lon) {
  if (!currentMap) return { x: 0, y: 0 };
  const m = currentMap;
  const mapWidth = m.maxLon - m.minLon;
  const mapHeight = m.maxLat - m.minLat;
  const scaleX = (canvas.width * 0.9) / mapWidth;
  const scaleY = (canvas.height * 0.9) / mapHeight;
  const scale = Math.min(scaleX, scaleY) * zoom;
  const mapCenterX = (m.minLon + m.maxLon) / 2;
  const mapCenterY = (m.minLat + m.maxLat) / 2;
  let x = (lon - mapCenterX) * scale;
  let y = -((lat - mapCenterY) * scale);
  if (rotation !== 0) {
    const rad = rotation * Math.PI / 180;
    const cos = Math.cos(rad);
    const sin = Math.sin(rad);
    const rx = x * cos - y * sin;
    const ry = x * sin + y * cos;
    x = rx;
    y = ry;
  }
  x += canvas.width / 2 + panX;
  y += canvas.height / 2 + panY;
  return { x, y };
}

function hitTestWaypoint(canvasX, canvasY) {
  if (!currentMap || !waypoints || waypoints.length === 0) return -1;
  let bestIdx = -1;
  let bestD2 = Infinity;
  for (let idx = 0; idx < waypoints.length; idx++) {
    const wp = waypoints[idx];
    const pos = geoToCanvasForHitTest(wp.lat, wp.lon);
    const dx = canvasX - pos.x;
    const dy = canvasY - pos.y;
    const d2 = dx * dx + dy * dy;
    if (d2 < bestD2) {
      bestD2 = d2;
      bestIdx = idx;
    }
  }
  const hitRadius = 14;
  return (bestIdx >= 0 && bestD2 <= hitRadius * hitRadius) ? bestIdx : -1;
}

function selectWaypointOnMap(idx) {
  selectedWaypointIndex = idx;
  renderMap();
  if (typeof loadWaypoints === 'function') loadWaypoints();
  
  // Show popup for selected waypoint
  if (idx >= 0 && waypoints && waypoints[idx]) {
    showWaypointPopup(idx);
  }
}

function showWaypointPopup(idx) {
  const wp = waypoints[idx];
  if (!wp) return;
  
  // Remove any existing popup
  const existingPopup = document.getElementById('waypoint-popup');
  if (existingPopup) existingPopup.remove();
  
  // Create popup
  const popup = document.createElement('div');
  popup.id = 'waypoint-popup';
  popup.style.cssText = 'position:fixed;top:50%;left:50%;transform:translate(-50%,-50%);background:var(--panel-bg);border:2px solid var(--border);border-radius:8px;padding:1rem;min-width:250px;max-width:400px;z-index:10000;box-shadow:0 4px 12px rgba(0,0,0,0.5)';
  
  const hasNotes = wp.notes && String(wp.notes).trim().length > 0;
  const isTarget = idx === targetWaypointIndex;
  
  popup.innerHTML = `
    <div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:0.5rem">
      <h3 style="margin:0;color:var(--panel-fg)">${escapeHtml(wp.name)}${isTarget ? ' ⭐' : ''}</h3>
      <button onclick="closeWaypointPopup()" style="background:none;border:none;color:var(--panel-fg);font-size:1.5rem;cursor:pointer;padding:0;line-height:1">&times;</button>
    </div>
    <div style="color:var(--panel-fg);font-size:0.85rem;margin-bottom:0.5rem">
      <div>Lat: ${wp.lat.toFixed(6)}°</div>
      <div>Lon: ${wp.lon.toFixed(6)}°</div>
    </div>
    ${hasNotes ? `<div style="margin:0.5rem 0;padding:0.5rem;background:var(--input-bg);border-radius:4px;font-size:0.85rem;color:var(--panel-fg)">${escapeHtml(wp.notes)}</div>` : ''}
    <div style="display:flex;gap:0.5rem;margin-top:0.75rem;flex-wrap:wrap">
      ${!isTarget ? `<button onclick="gotoWaypoint(${idx});closeWaypointPopup()" class="btn" style="flex:1;padding:6px 10px;font-size:0.85rem">Set Target</button>` : `<button onclick="clearTarget();closeWaypointPopup()" class="btn" style="flex:1;padding:6px 10px;font-size:0.85rem">Clear Target</button>`}
      <button onclick="centerOnWaypoint(${idx})" class="btn" style="flex:1;padding:6px 10px;font-size:0.85rem">Center</button>
    </div>
    <div style="display:flex;gap:0.5rem;margin-top:0.5rem;flex-wrap:wrap">
      <button onclick="renameWaypoint(${idx});closeWaypointPopup()" class="btn" style="flex:1;padding:6px 10px;font-size:0.85rem">Rename</button>
      <button onclick="editWaypointNotes(${idx});closeWaypointPopup()" class="btn" style="flex:1;padding:6px 10px;font-size:0.85rem">Notes</button>
      <button onclick="deleteWaypoint(${idx});closeWaypointPopup()" class="btn" style="flex:1;padding:6px 10px;font-size:0.85rem;background:#f44336;color:#fff;border:none">Delete</button>
    </div>
  `;
  
  document.body.appendChild(popup);
  
  // Close on click outside
  setTimeout(() => {
    const closeOnClickOutside = (e) => {
      if (!popup.contains(e.target)) {
        closeWaypointPopup();
        document.removeEventListener('click', closeOnClickOutside);
      }
    };
    document.addEventListener('click', closeOnClickOutside);
  }, 100);
}

function closeWaypointPopup() {
  const popup = document.getElementById('waypoint-popup');
  if (popup) popup.remove();
  selectedWaypointIndex = -1;
  renderMap();
  if (typeof loadWaypoints === 'function') loadWaypoints();
}

canvas.addEventListener('mousedown', (e) => {
  if (waypointMode) {
    const pt = getCanvasXYFromMouseEvent(e);
    addMapWaypoint(pt.x, pt.y);
    return;
  }
  isPointerDown = true;
  didDrag = false;
  downMouseX = e.clientX;
  downMouseY = e.clientY;
  lastMouseX = e.clientX;
  lastMouseY = e.clientY;
  const pt = getCanvasXYFromMouseEvent(e);
  pendingWaypointClickIndex = hitTestWaypoint(pt.x, pt.y);
});

canvas.addEventListener('mousemove', (e) => {
  if (!isPointerDown) return;
  const dx = e.clientX - downMouseX;
  const dy = e.clientY - downMouseY;
  const moved = Math.abs(dx) + Math.abs(dy);
  if (!isDragging && moved > 3) {
    isDragging = true;
    didDrag = true;
    pendingWaypointClickIndex = -1;
    canvas.style.cursor = 'grabbing';
  }
  if (!isDragging) return;
  panX += e.clientX - lastMouseX;
  panY += e.clientY - lastMouseY;
  lastMouseX = e.clientX;
  lastMouseY = e.clientY;
  renderMap();
});

canvas.addEventListener('mouseup', (e) => {
  if (!isPointerDown) return;
  isPointerDown = false;
  const clickIdx = pendingWaypointClickIndex;
  pendingWaypointClickIndex = -1;
  const wasDragging = isDragging;
  isDragging = false;
  canvas.style.cursor = waypointMode ? 'crosshair' : 'grab';
  if (wasDragging || didDrag) return;
  if (clickIdx >= 0) {
    selectWaypointOnMap(clickIdx);
  }
});

canvas.addEventListener('mouseleave', () => {
  isPointerDown = false;
  isDragging = false;
  pendingWaypointClickIndex = -1;
  canvas.style.cursor = waypointMode ? 'crosshair' : 'grab';
});
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

function escapeHtml(s) {
  s = (s === null || s === undefined) ? '' : String(s);
  return s.replace(/&/g, '&amp;')
          .replace(/</g, '&lt;')
          .replace(/>/g, '&gt;')
          .replace(/\"/g, '&quot;')
          .replace(/'/g, '&#39;');
}

async function loadWaypoints() {
  if (!currentMap) {
    document.getElementById('waypoint-status').textContent = 'Error: No map loaded';
    document.getElementById('waypoint-list').innerHTML = '<p style="color:var(--panel-fg);font-size:0.85rem;margin:0.5rem 0">Load a map above to manage waypoints</p>';
    return;
  }
  
  try {
    const resp = await fetch('/api/waypoints');
    const data = await resp.json();
    
    if (data.success) {
      waypoints = data.waypoints || [];
      targetWaypointIndex = (typeof data.target === 'number') ? data.target : -1;
      document.getElementById('waypoint-status').textContent = `Map: ${data.mapName} | Waypoints: ${data.count}/${data.max}`;
      
      if (waypoints.length > 0) {
        let html = '<table style="width:100%;font-size:0.85rem;border-collapse:collapse">';
        html += '<tr style="background:#0a0a14"><th style="padding:4px;text-align:left">Name</th><th>Lat</th><th>Lon</th><th>Actions</th></tr>';
        waypoints.forEach((wp, idx) => {
          const isTarget = idx === targetWaypointIndex;
          const isSelected = idx === selectedWaypointIndex;
          const rowBg = isSelected ? '#102030' : (isTarget ? '#2a1a1a' : '');
          html += `<tr style="border-bottom:1px solid #333${rowBg ? ';background:' + rowBg : ''}">`;
          const hasNotes = wp.notes && String(wp.notes).trim().length > 0;
          html += `<td style="padding:4px">${escapeHtml(wp.name)}${hasNotes ? ' 📝' : ''}${isTarget ? ' ⭐' : ''}</td>`;
          html += `<td style="text-align:center">${wp.lat.toFixed(5)}</td>`;
          html += `<td style="text-align:center">${wp.lon.toFixed(5)}</td>`;
          html += '<td style="text-align:center">';
          if (!isTarget) {
            html += `<button onclick="gotoWaypoint(${idx})" class="btn" style="padding:2px 6px;margin:0 2px;font-size:0.75rem">Target</button>`;
          } else {
            html += `<button onclick="clearTarget()" class="btn" style="padding:2px 6px;margin:0 2px;font-size:0.75rem">Clear</button>`;
          }
          html += `<button onclick="renameWaypoint(${idx})" class="btn" style="padding:2px 6px;margin:0 2px;font-size:0.75rem">Rename</button>`;
          html += `<button onclick="editWaypointNotes(${idx})" class="btn" style="padding:2px 6px;margin:0 2px;font-size:0.75rem">Notes</button>`;
          html += `<button onclick="deleteWaypoint(${idx})" class="btn" style="padding:2px 6px;margin:0 2px;background:#f44336;color:#fff;border:none;border-radius:3px;cursor:pointer;font-size:0.75rem">Del</button>`;
          html += '</td></tr>';
        });
        html += '</table>';
        document.getElementById('waypoint-list').innerHTML = html;
      } else {
        document.getElementById('waypoint-list').innerHTML = '<p style="color:#666;font-size:0.85rem;margin:0.5rem 0">No waypoints for this map</p>';
      }
      renderMap();
      if (typeof updateRoutesList === 'function') updateRoutesList();
    } else {
      document.getElementById('waypoint-status').textContent = 'Error: ' + (data.error || 'Failed to load');
    }
  } catch (e) {
    document.getElementById('waypoint-status').textContent = 'Error loading waypoints';
  }
}

function centerOnWaypoint(idx) {
  if (!currentMap || !waypoints || idx < 0 || idx >= waypoints.length) return;
  const wp = waypoints[idx];
  if (!wp) return;
  const m = currentMap;
  const canvas = document.getElementById('map-canvas');
  const mapWidth = m.maxLon - m.minLon;
  const mapHeight = m.maxLat - m.minLat;
  const scaleX = (canvas.width * 0.9) / mapWidth;
  const scaleY = (canvas.height * 0.9) / mapHeight;
  const scale = Math.min(scaleX, scaleY) * zoom;
  const mapCenterX = (m.minLon + m.maxLon) / 2;
  const mapCenterY = (m.minLat + m.maxLat) / 2;
  let x = (wp.lon - mapCenterX) * scale;
  let y = -((wp.lat - mapCenterY) * scale);
  if (rotation !== 0) {
    const rad = rotation * Math.PI / 180;
    const cos = Math.cos(rad);
    const sin = Math.sin(rad);
    const rx = x * cos - y * sin;
    const ry = x * sin + y * cos;
    x = rx;
    y = ry;
  }
  panX = -x;
  panY = -y;
  renderMap();
}

async function addWaypoint() {
  const name = document.getElementById('wp-name').value.trim();
  const lat = parseFloat(document.getElementById('wp-lat').value);
  const lon = parseFloat(document.getElementById('wp-lon').value);
  const notes = (document.getElementById('wp-notes').value || '').trim();
  
  if (!name || isNaN(lat) || isNaN(lon)) {
    alert('Please fill in all fields');
    return;
  }

  await addWaypointViaAPI(name, lat, lon, notes);
  document.getElementById('wp-name').value = '';
  document.getElementById('wp-lat').value = '';
  document.getElementById('wp-lon').value = '';
  document.getElementById('wp-notes').value = '';
}

async function addWaypointViaAPI(name, lat, lon, notes) {
  const body = new URLSearchParams();
  body.set('action', 'add');
  body.set('name', name);
  body.set('lat', String(lat));
  body.set('lon', String(lon));
  if (notes) body.set('notes', notes);
  const resp = await fetch('/api/waypoints', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body
  });
  const data = await resp.json();
  if (!data.success) throw new Error(data.error || 'Failed to add waypoint');
  await loadWaypoints();
}

async function renameWaypoint(idx) {
  const wp = waypoints[idx];
  if (!wp) return;
  const newName = await hwPrompt('Enter waypoint name:', wp.name);
  if (newName === null) return;
  const name = newName.trim().substring(0, 11);
  if (!name) return;
  const body = new URLSearchParams();
  body.set('action', 'rename');
  body.set('index', String(idx));
  body.set('name', name);
  const resp = await fetch('/api/waypoints', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body
  });
  const data = await resp.json();
  if (!data.success) await hwAlert('Error: ' + (data.error || 'Failed'));
  await loadWaypoints();
}

async function editWaypointNotes(idx) {
  const wp = waypoints[idx];
  if (!wp) return;
  const existing = (wp.notes || '').toString();
  const newNotes = await hwPrompt('Enter notes (optional, max 255 chars):', existing);
  if (newNotes === null) return;
  const notes = newNotes.substring(0, 255);
  const body = new URLSearchParams();
  body.set('action', 'set_notes');
  body.set('index', String(idx));
  body.set('notes', notes);
  const resp = await fetch('/api/waypoints', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body
  });
  const data = await resp.json();
  if (!data.success) alert('Error: ' + (data.error || 'Failed'));
  await loadWaypoints();
}

function exportWaypoints() {
  if (!waypoints || waypoints.length === 0) {
    alert('No waypoints to export');
    return;
  }
  const data = {
    waypoints: waypoints.map(wp => ({ name: wp.name, lat: wp.lat, lon: wp.lon, notes: wp.notes || '' })),
    exportedAt: new Date().toISOString(),
    version: 1
  };
  const blob = new Blob([JSON.stringify(data, null, 2)], { type: 'application/json' });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  // Use waypoints_<mapname>.json format to match the system's expected naming
  const mapName = (currentMap && currentMap.filename) ? currentMap.filename.replace(/\.hwmap$/,'') : 'map';
  a.download = `waypoints_${mapName}.json`;
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
  URL.revokeObjectURL(url);
}

function importWaypoints() {
  const input = document.createElement('input');
  input.type = 'file';
  input.accept = '.json';
  input.onchange = async (e) => {
    const file = e.target.files[0];
    if (!file) return;
    const text = await file.text();
    let data;
    try {
      data = JSON.parse(text);
    } catch (err) {
      alert('Failed to parse waypoint file: ' + err.message);
      return;
    }
    if (!data.waypoints || !Array.isArray(data.waypoints)) {
      alert('Invalid waypoint file format');
      return;
    }
    if (!confirm(`Import ${data.waypoints.length} waypoint(s)? This will add to existing waypoints.`)) return;
    for (const wp of data.waypoints) {
      if (typeof wp.lat === 'number' && typeof wp.lon === 'number') {
        try {
          await addWaypointViaAPI((wp.name || 'Imported').toString().substring(0, 11), wp.lat, wp.lon, (wp.notes || '').toString().substring(0, 255));
        } catch (err) {
          alert('Import error: ' + err.message);
          break;
        }
      }
    }
    await loadWaypoints();
  };
  input.click();
}

async function clearAllWaypoints() {
  if (!confirm('Delete all waypoints for this map?')) return;
  const body = new URLSearchParams();
  body.set('action', 'clear_all');
  const resp = await fetch('/api/waypoints', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body
  });
  const data = await resp.json();
  if (!data.success) alert('Error: ' + (data.error || 'Failed'));
  await loadWaypoints();
}

async function deleteWaypoint(idx) {
  if (!confirm('Delete this waypoint?')) return;
  
  const body = new URLSearchParams();
  body.set('action', 'delete');
  body.set('index', String(idx));
  
  try {
    const resp = await fetch('/api/waypoints', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body
    });
    const data = await resp.json();
    if (data.success) await loadWaypoints();
    else alert('Error: ' + (data.error || 'Failed'));
  } catch (e) {
    alert('Error: ' + e.message);
  }
}

async function gotoWaypoint(idx) {
  const body = new URLSearchParams();
  body.set('action', 'goto');
  body.set('index', String(idx));
  
  try {
    const resp = await fetch('/api/waypoints', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body
    });
    const data = await resp.json();
    if (data.success) {
      await loadWaypoints();
      centerOnWaypoint(idx);
    }
    else alert('Error: ' + (data.error || 'Failed'));
  } catch (e) {
    alert('Error: ' + e.message);
  }
}

async function clearTarget() {
  const body = new URLSearchParams();
  body.set('action', 'clear');
  
  try {
    const resp = await fetch('/api/waypoints', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body
    });
    const data = await resp.json();
    if (data.success) await loadWaypoints();
    else alert('Error: ' + (data.error || 'Failed'));
  } catch (e) {
    alert('Error: ' + e.message);
  }
}

function updateRoutesList() {
  const el = document.getElementById('routes-list');
  if (!el) return;

  if (!currentMap || !currentMap.features) {
    el.innerHTML = '<div style="color:var(--panel-fg)">Load a map to see routes</div>';
    return;
  }

  const sections = [
    { type: 0x20, title: 'Rail', color: '#da77f2' },
    { type: 0x21, title: 'Bus', color: '#fab005' },
    { type: 0x22, title: 'Ferry', color: '#15aabf' }
  ];

  let html = '';
  for (const sec of sections) {
    const byKey = new Map();
    for (const f of currentMap.features) {
      if (f.type !== sec.type) continue;
      const n = String(f.name || '').replace(/\0/g, '').trim();
      if (!n) continue;
      const k = n.toLowerCase();
      const cur = byKey.get(k);
      if (cur) cur.count++;
      else byKey.set(k, { name: n, count: 1 });
    }

    const items = Array.from(byKey.values()).sort((a, b) => a.name.localeCompare(b.name));
    html += `<div style="margin:0 0 6px 0"><strong style="color:${sec.color}">${sec.title} (${items.length})</strong></div>`;
    if (items.length === 0) {
      html += '<div style="color:var(--panel-fg);margin:0 0 10px 0">None</div>';
      continue;
    }

    html += '<div style="display:flex;flex-direction:column;gap:4px;margin:0 0 12px 0">';
    for (let i = 0; i < items.length && i < 60; i++) {
      const item = items[i];
      const suffix = item.count > 1 ? ` (${item.count})` : '';
      html += `<div data-route-name="${encodeURIComponent(item.name)}" style="cursor:pointer;padding:6px;border:1px solid #333;border-radius:4px;background:#0a0a14">${item.name}${suffix}</div>`;
    }
    html += '</div>';
  }

  el.innerHTML = html;
  el.querySelectorAll('[data-route-name]').forEach((node) => {
    node.addEventListener('click', () => {
      const name = decodeURIComponent(node.getAttribute('data-route-name') || '');
      if (name) selectSearchResult(name);
    });
  });
}

// Search functionality
function showSearchDialog() {
  document.getElementById('search-dialog').style.display = 'block';
  document.getElementById('search-input').focus();
}

function hideSearchDialog() {
  document.getElementById('search-dialog').style.display = 'none';
  document.getElementById('search-input').value = '';
  document.getElementById('search-results').innerHTML = '';
}

function searchMapNames() {
  const query = document.getElementById('search-input').value.trim().toLowerCase();
  const resultsDiv = document.getElementById('search-results');
  
  if (!currentMap || query.length < 2) {
    resultsDiv.innerHTML = query.length > 0 && query.length < 2 ? 
      '<div style="color:var(--panel-fg);padding:0.5rem">Type at least 2 characters...</div>' : '';
    return;
  }
  
  // Search through map names (names is array of strings)
  const matches = [];
  if (currentMap.names) {
    for (let i = 0; i < currentMap.names.length; i++) {
      const name = currentMap.names[i];
      if (name && name.toLowerCase().includes(query)) {
        matches.push({ name, index: i });
        if (matches.length >= 20) break; // Limit results
      }
    }
  }
  
  if (matches.length === 0) {
    resultsDiv.innerHTML = '<div style="color:var(--panel-fg);padding:0.5rem">No results found</div>';
    return;
  }
  
  let html = '<div style="border:1px solid var(--border);border-radius:4px;overflow:hidden">';
  matches.forEach((match, idx) => {
    html += `<div style="padding:0.5rem;border-bottom:1px solid var(--border);cursor:pointer;background:var(--bg)" 
             onmouseover="this.style.background='var(--panel-bg)'" 
             onmouseout="this.style.background='var(--bg)'"
             onclick="selectSearchResult('${match.name.replace(/'/g, "\\'")}')">
             ${match.name}
           </div>`;
  });
  html += '</div>';
  resultsDiv.innerHTML = html;
}

function selectSearchResult(name) {
  console.log('Selected:', name);
  // Find ALL features with this name for navigation
  searchResults = [];
  searchResultIndex = 0;
  const nameNorm = String(name).replace(/\0/g, '').trim();
  
  if (currentMap && currentMap.features) {
    for (const feature of currentMap.features) {
      const fNameNorm = String(feature.name || '').replace(/\0/g, '').trim();
      if (fNameNorm === nameNorm && feature.points && feature.points.length > 0) {
        searchResults.push(feature);
      }
    }
    
    if (searchResults.length > 0) {
      console.log('Found', searchResults.length, 'matches for:', name);
      centerOnSearchResult(0);
      hideSearchDialog();
    }
  }
}

function centerOnSearchResult(index) {
  if (index < 0 || index >= searchResults.length) return;
  searchResultIndex = index;
  
  const feature = searchResults[index];
  const midIdx = Math.floor(feature.points.length / 2);
  const point = feature.points[midIdx];
  const canvas = document.getElementById('map-canvas');
  const m = currentMap;
  
  // Calculate scale (same as renderMap)
  const mapWidth = m.maxLon - m.minLon;
  const mapHeight = m.maxLat - m.minLat;
  const scaleX = (canvas.width * 0.9) / mapWidth;
  const scaleY = (canvas.height * 0.9) / mapHeight;
  const scale = Math.min(scaleX, scaleY) * zoom;
  
  // Calculate pan to center on this point
  const mapCenterX = (m.minLon + m.maxLon) / 2;
  const mapCenterY = (m.minLat + m.maxLat) / 2;
  
  // Match toCanvas() math (degrees, center-based), including rotation
  let x = (point.lon - mapCenterX) * scale;
  let y = -((point.lat - mapCenterY) * scale);
  if (rotation !== 0) {
    const rad = rotation * Math.PI / 180;
    const cos = Math.cos(rad);
    const sin = Math.sin(rad);
    const rx = x * cos - y * sin;
    const ry = x * sin + y * cos;
    x = rx;
    y = ry;
  }
  panX = -x;
  panY = -y;
  
  // Set selected feature for pin display
  const typeNames = {0x00:'Highway',0x01:'Major Road',0x02:'Minor Road',0x03:'Path',0x10:'Water',0x11:'Park',0x20:'Railway',0x21:'Bus',0x22:'Ferry',0x30:'Building'};
  selectedFeature = {
    name: feature.name,
    type: feature.type,
    typeName: typeNames[feature.type] || 'Unknown',
    lat: point.lat,
    lon: point.lon,
    pointCount: feature.points.length,
    pinLat: point.lat,
    pinLon: point.lon
  };
  
  console.log('Showing result', index + 1, '/', searchResults.length, 'at', selectedFeature.lat, selectedFeature.lon);
  renderMap();
  showFeatureInfo();
}

function prevSearchResult() {
  if (searchResults.length <= 1) return;
  const newIdx = (searchResultIndex - 1 + searchResults.length) % searchResults.length;
  centerOnSearchResult(newIdx);
}

function nextSearchResult() {
  if (searchResults.length <= 1) return;
  const newIdx = (searchResultIndex + 1) % searchResults.length;
  centerOnSearchResult(newIdx);
}

function showFeatureInfo() {
  if (!selectedFeature) return;
  let infoDiv = document.getElementById('feature-info');
  if (!infoDiv) {
    infoDiv = document.createElement('div');
    infoDiv.id = 'feature-info';
    infoDiv.style.cssText = 'position:absolute;top:10px;right:10px;background:rgba(0,0,0,0.85);border:1px solid var(--border);border-radius:8px;padding:12px;color:#fff;font-size:0.9rem;max-width:280px;z-index:100';
    document.getElementById('map-canvas').parentElement.style.position = 'relative';
    document.getElementById('map-canvas').parentElement.appendChild(infoDiv);
  }
  
  // Build navigation HTML if multiple results
  let navHtml = '';
  if (searchResults.length > 1) {
    navHtml = `
      <div style="display:flex;align-items:center;justify-content:space-between;margin-top:8px;padding-top:8px;border-top:1px solid #444">
        <button onclick="prevSearchResult()" style="padding:4px 10px;background:#333;color:#fff;border:1px solid #555;border-radius:4px;cursor:pointer">&lt; Prev</button>
        <span style="color:#4dabf7;font-size:0.85rem">${searchResultIndex + 1} / ${searchResults.length}</span>
        <button onclick="nextSearchResult()" style="padding:4px 10px;background:#333;color:#fff;border:1px solid #555;border-radius:4px;cursor:pointer">Next &gt;</button>
      </div>
    `;
  }
  
  infoDiv.innerHTML = `
    <div style="display:flex;justify-content:space-between;align-items:start;margin-bottom:8px">
      <strong style="font-size:1.1rem;color:#4dabf7">${selectedFeature.name}</strong>
      <button onclick="clearSelectedFeature()" style="background:none;border:none;color:#888;cursor:pointer;font-size:1.2rem;line-height:1">&times;</button>
    </div>
    <div style="color:#aaa;margin-bottom:6px">${selectedFeature.typeName}</div>
    <div style="font-size:0.8rem;color:#888">
      <div>Lat: ${selectedFeature.lat.toFixed(6)}°</div>
      <div>Lon: ${selectedFeature.lon.toFixed(6)}°</div>
      <div style="margin-top:4px">${selectedFeature.pointCount} points</div>
    </div>
    ${navHtml}
  `;
  infoDiv.style.display = 'block';
}

function clearSelectedFeature() {
  selectedFeature = null;
  const infoDiv = document.getElementById('feature-info');
  if (infoDiv) infoDiv.style.display = 'none';
  renderMap();
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
        html += `<div style="color:var(--panel-fg);padding-left:8px">`;
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

// GPS Track Functions
async function loadGPSTrackFiles() {
  try {
    const resp = await fetch('/api/gps/tracks');
    const data = await resp.json();
    
    const select = document.getElementById('track-file');
    select.innerHTML = '<option value="">Select GPS log file...</option>';
    
    if (data.success && data.files && data.files.length > 0) {
      data.files.forEach(file => {
        const option = document.createElement('option');
        option.value = file.path;
        option.textContent = file.path + ' (' + (file.size / 1024).toFixed(1) + ' KB)';
        select.appendChild(option);
      });
    }
  } catch (e) {
    console.error('Error loading GPS track files:', e);
  }
}

async function loadGPSTrack() {
  const filepath = document.getElementById('track-file').value;
  if (!filepath) {
    alert('Please select a GPS log file');
    return;
  }
  
  try {
    const resp = await fetch('/api/gps/tracks?file=' + encodeURIComponent(filepath));
    const data = await resp.json();
    
    if (data.error) {
      alert('Error: ' + data.error);
      return;
    }
    
    if (data.success && data.points && data.points.length > 0) {
      // Check validation status
      if (data.validation === 'out_of_bounds') {
        alert('Error: GPS track is outside the current map region (' + 
              data.coverage.toFixed(0) + '% visible). Please load a different map or track.');
        return;
      }
      
      gpsTrack = data.points;
      
      // Show validation message with appropriate color
      const infoEl = document.getElementById('track-info');
      infoEl.textContent = `Loaded ${data.count} GPS points. ${data.message}`;
      
      if (data.validation === 'valid') {
        infoEl.style.color = '#69db7c';  // Green
      } else if (data.validation === 'partial') {
        infoEl.style.color = '#ffd93d';  // Yellow warning
      }
      
      renderMap();
    } else {
      alert('No GPS data found in file');
    }
  } catch (e) {
    alert('Error loading GPS track: ' + e.message);
  }
}

function clearGPSTrack() {
  gpsTrack = null;
  document.getElementById('track-info').textContent = '';
  document.getElementById('track-file').value = '';
  renderMap();
}

// Track color functions
function updateTrackColor() {
  trackColor = document.getElementById('track-color').value;
  // Update preset dropdown to match
  const preset = document.getElementById('track-color-preset');
  let found = false;
  for (let i = 0; i < preset.options.length; i++) {
    if (preset.options[i].value.toLowerCase() === trackColor.toLowerCase()) {
      preset.selectedIndex = i;
      found = true;
      break;
    }
  }
  if (!found) preset.selectedIndex = -1;
  renderMap();
}

function applyColorPreset() {
  const preset = document.getElementById('track-color-preset');
  trackColor = preset.value;
  document.getElementById('track-color').value = trackColor;
  renderMap();
}

// Live tracking
let liveTrackInterval = null;
let lastLiveUpdate = 0;

async function pollLiveTrack() {
  try {
    const resp = await fetch('/api/gps/tracks?live=1');
    const data = await resp.json();
    
    if (data.live && data.points && data.points.length > 0) {
      // Only update if track changed
      if (data.lastUpdate !== lastLiveUpdate) {
        lastLiveUpdate = data.lastUpdate;
        gpsTrack = data.points;
        
        // Update track info with live stats
        const infoEl = document.getElementById('track-info');
        const dist = data.distance >= 1000 ? (data.distance/1000).toFixed(2) + 'km' : data.distance.toFixed(0) + 'm';
        const mins = Math.floor(data.duration / 60);
        const secs = Math.floor(data.duration % 60);
        infoEl.innerHTML = '<span style="color:#69db7c">LIVE</span> ' + 
          data.count + ' pts | ' + dist + ' | ' + mins + ':' + String(secs).padStart(2,'0') + 
          ' | ' + data.speed.toFixed(1) + 'm/s';
        
        renderMap();
      }
    } else if (!data.live && liveTrackInterval) {
      // Live tracking stopped on device
      clearInterval(liveTrackInterval);
      liveTrackInterval = null;
      document.getElementById('track-info').textContent = 'Live tracking stopped';
    }
  } catch (e) {
    console.error('Live track poll error:', e);
  }
}

function toggleLiveTrack() {
  if (liveTrackInterval) {
    clearInterval(liveTrackInterval);
    liveTrackInterval = null;
    document.getElementById('track-info').textContent = 'Live tracking stopped';
  } else {
    lastLiveUpdate = 0;
    liveTrackInterval = setInterval(pollLiveTrack, 1000);
    pollLiveTrack(); // Immediate first poll
    document.getElementById('track-info').innerHTML = '<span style="color:#69db7c">LIVE</span> Connecting...';
  }
}

// Initialize
initMapsFileBrowser();
checkSystemStatus();
renderMap();
loadWaypoints();
loadMapFeatures();
loadGPSTrackFiles();
setInterval(loadWaypoints, 5000);
</script>
)JS", HTTPD_RESP_USE_STRLEN);
}

// =============================================================================
// API Handler Declarations (implementations in WebPage_Maps.cpp)
// =============================================================================

// Map select API - select a map file on device
esp_err_t handleMapSelectAPI(httpd_req_t* req);

// Map features API - get map metadata and feature names
esp_err_t handleMapFeaturesAPI(httpd_req_t* req);

// GPS tracks API - load tracks, list files, live tracking
esp_err_t handleGPSTracksAPI(httpd_req_t* req);

// Waypoints page handler (merged from WebPage_Waypoints)
esp_err_t handleWaypointsPage(httpd_req_t* req);

// Waypoints API handler (merged from WebPage_Waypoints)
esp_err_t handleWaypointsAPI(httpd_req_t* req);

// Register all maps-related URI handlers
void registerMapsHandlers(httpd_handle_t server);

// Map file organization (used by upload hook in WebServer_Server.cpp)
bool organizeMapFromAnyPath(const String& srcPath, String& outErr);
bool isMapFileByMagic(const String& path);
bool tryOrganizeLegacyWaypointsAtRoot(const String& fn, String& outErr);

#else // !ENABLE_MAPS

#include <Arduino.h>

// Inline stubs when maps is disabled
inline void registerMapsHandlers(httpd_handle_t) {}
inline bool organizeMapFromAnyPath(const String&, String&) { return false; }
inline bool isMapFileByMagic(const String&) { return false; }
inline bool tryOrganizeLegacyWaypointsAtRoot(const String&, String&) { return false; }

#endif // ENABLE_MAPS

#endif // WEBPAGE_MAPS_H
