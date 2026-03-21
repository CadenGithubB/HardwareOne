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

<style>
.map-ctrl{width:38px;height:38px;padding:0;border-radius:6px;min-height:unset;font-size:1.1em;flex-shrink:0}
.map-ctrl-wide{width:auto;padding:0 12px;font-size:0.8em}
.map-bar-v{display:flex;flex-direction:column;gap:6px}
.map-bar-h{display:flex;flex-direction:row;gap:6px}
.lyr-group{margin-bottom:2px}
.lyr-group summary{cursor:pointer;font-size:0.78rem;font-weight:600;color:var(--panel-fg);padding:5px 4px;border-radius:4px;list-style:none;display:flex;align-items:center;gap:6px;user-select:none}
.lyr-group summary::-webkit-details-marker{display:none}
.lyr-group summary::before{content:'\25B6';font-size:0.65em;opacity:0.5;transition:transform .15s}
.lyr-group[open] summary::before{transform:rotate(90deg)}
.lyr-group summary:hover{background:var(--crumb-bg)}
.lyr-items{display:flex;flex-wrap:wrap;gap:6px 14px;padding:4px 8px 8px 20px;font-size:0.78rem}
.lyr-items label{display:inline-flex;align-items:center;gap:5px;white-space:nowrap;cursor:pointer}
</style>
<div style='display:flex;gap:1rem;margin:1rem 0;flex-wrap:wrap'>
  <div style='flex:1;min-width:340px'>
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
  <div style='flex:2;min-width:400px;display:flex;flex-direction:column;gap:0.5rem'>
    <div style='position:relative;background:var(--panel-bg);border-radius:8px;border:1px solid var(--border);overflow:hidden'>
      <canvas id='map-canvas' width='1200' height='800' style='width:100%;display:block;background:var(--code-bg)'></canvas>

      <!-- Zoom bar — top-right, vertical -->
      <div class='map-bar-v' style='position:absolute;top:10px;right:10px;z-index:10'>
        <button class='btn map-ctrl' onclick='zoomIn()' title='Zoom in' style='font-size:1.4em;font-weight:300'>+</button>
        <button class='btn map-ctrl' onclick='zoomOut()' title='Zoom out' style='font-size:1.6em;font-weight:300'>−</button>
      </div>

      <!-- Rotate/Reset bar — top-left, horizontal -->
      <div class='map-bar-h' style='position:absolute;top:10px;left:10px;z-index:10'>
        <button class='btn map-ctrl' onclick='rotateLeft()' title='Rotate left'>↺</button>
        <button class='btn map-ctrl' onclick='rotateRight()' title='Rotate right'>↻</button>
        <button class='btn map-ctrl' onclick='resetView()' title='Reset view'>⟲</button>
      </div>

      <!-- Action bar — bottom-left, above info bar -->
      <div class='map-bar-h' style='position:absolute;bottom:36px;left:10px;z-index:10'>
        <button class='btn map-ctrl map-ctrl-wide' onclick='centerOnGPS()' title='Centre on GPS'>GPS</button>
        <button class='btn map-ctrl map-ctrl-wide' onclick='showSearchDialog()' title='Search'>Search</button>
        <button class='btn map-ctrl' id='btn-add-waypoint' onclick='toggleWaypointMode()' title='Add waypoint'><svg width='16' height='16' viewBox='0 0 16 16' fill='currentColor'><path d='M8 1a4.5 4.5 0 0 1 4.5 4.5c0 3-4.5 9.5-4.5 9.5S3.5 8.5 3.5 5.5A4.5 4.5 0 0 1 8 1zm0 2.5a2 2 0 1 0 0 4 2 2 0 0 0 0-4z'/></svg></button>
      </div>

      <!-- Search popup — floats above the Search button -->
      <div id='search-dialog' style='display:none;position:absolute;bottom:84px;left:10px;width:320px;max-width:calc(100% - 20px);background:var(--menu-item-bg);color:var(--menu-item-fg);border:1px solid var(--border);border-radius:8px;box-shadow:0 4px 24px rgba(0,0,0,.5);z-index:20;overflow:hidden'>
        <input type='text' id='search-input' placeholder='Search map features...' style='width:100%;padding:10px 12px;border:none;border-bottom:1px solid var(--border);background:transparent;color:inherit;font-size:0.95rem;box-sizing:border-box;outline:none' oninput='searchMapNames()' onkeydown='if(event.key==="Escape")hideSearchDialog()'>
        <div id='search-results' style='max-height:240px;overflow-y:auto;font-size:0.85rem'></div>
      </div>

      <!-- Info bar — very bottom -->
      <div style='position:absolute;bottom:0;left:0;right:0;background:rgba(0,0,0,0.55);padding:4px 12px;display:flex;justify-content:space-between;align-items:center;font-size:0.78rem;color:#bbb;pointer-events:none'>
        <span id='zoom-info'>Zoom: 1x</span>
        <span id='rotation-info'>Rot: 0°</span>
        <span id='gps-info'>GPS: --</span>
      </div>
    </div>
  </div>
</div>

<div style='margin-top:1rem;display:flex;gap:1rem;align-items:flex-start'>
<div style='flex:1;min-width:0;background:var(--panel-bg);padding:1rem;border-radius:8px;border:1px solid var(--border);height:420px;display:flex;flex-direction:column'>
  <h3 style='margin:0 0 0.5rem 0;color:var(--panel-fg)'>Layers</h3>
  <div style='flex:1;overflow-y:auto'>

  <details class='lyr-group'>
    <summary><span style='color:#ff6b6b'>Roads</span></summary>
    <div class='lyr-items'>
      <label style='display:flex;align-items:center;gap:4px;cursor:pointer'><input type='checkbox' id='layer-road-motorway' checked onchange='renderMap()' style='margin:0'><span style='color:#ff6b6b'>Motorway</span></label>
      <label style='display:flex;align-items:center;gap:4px;cursor:pointer'><input type='checkbox' id='layer-road-trunk' checked onchange='renderMap()' style='margin:0'><span style='color:#ff8787'>Trunk</span></label>
      <label style='display:flex;align-items:center;gap:4px;cursor:pointer'><input type='checkbox' id='layer-road-primary' checked onchange='renderMap()' style='margin:0'><span style='color:#ffd93d'>Primary</span></label>
      <label style='display:flex;align-items:center;gap:4px;cursor:pointer'><input type='checkbox' id='layer-road-secondary' checked onchange='renderMap()' style='margin:0'><span style='color:#ffe066'>Secondary</span></label>
      <label style='display:flex;align-items:center;gap:4px;cursor:pointer'><input type='checkbox' id='layer-road-tertiary' checked onchange='renderMap()' style='margin:0'><span style='color:#ffffff'>Tertiary</span></label>
      <label style='display:flex;align-items:center;gap:4px;cursor:pointer'><input type='checkbox' id='layer-road-residential' checked onchange='renderMap()' style='margin:0'><span style='color:#e9ecef'>Residential</span></label>
      <label style='display:flex;align-items:center;gap:4px;cursor:pointer'><input type='checkbox' id='layer-road-service' checked onchange='renderMap()' style='margin:0'><span style='color:#ced4da'>Service</span></label>
    </div>
  </details>

  <details class='lyr-group'>
    <summary><span style='color:#aaaaaa'>Paths</span></summary>
    <div class='lyr-items'>
      <label style='display:flex;align-items:center;gap:4px;cursor:pointer'><input type='checkbox' id='layer-path-footway' checked onchange='renderMap()' style='margin:0'><span style='color:#aaaaaa'>Footway</span></label>
      <label style='display:flex;align-items:center;gap:4px;cursor:pointer'><input type='checkbox' id='layer-path-cycleway' checked onchange='renderMap()' style='margin:0'><span style='color:#74c0fc'>Cycleway</span></label>
      <label style='display:flex;align-items:center;gap:4px;cursor:pointer'><input type='checkbox' id='layer-path-track' checked onchange='renderMap()' style='margin:0'><span style='color:#8b7355'>Track</span></label>
    </div>
  </details>

  <details class='lyr-group'>
    <summary><span style='color:#4dabf7'>Water</span></summary>
    <div class='lyr-items'>
      <label style='display:flex;align-items:center;gap:4px;cursor:pointer'><input type='checkbox' id='layer-water-lake' checked onchange='renderMap()' style='margin:0'><span style='color:#4dabf7'>Lakes</span></label>
      <label style='display:flex;align-items:center;gap:4px;cursor:pointer'><input type='checkbox' id='layer-water-river' checked onchange='renderMap()' style='margin:0'><span style='color:#339af0'>Rivers</span></label>
      <label style='display:flex;align-items:center;gap:4px;cursor:pointer'><input type='checkbox' id='layer-water-coast' checked onchange='renderMap()' style='margin:0'><span style='color:#1c7ed6'>Coast</span></label>
      <label style='display:flex;align-items:center;gap:4px;cursor:pointer'><input type='checkbox' id='layer-landmask' checked onchange='renderMap()' style='margin:0'><span style='color:#c9b896'>Land Mask</span></label>
    </div>
  </details>

  <details class='lyr-group'>
    <summary><span style='color:#69db7c'>Nature</span></summary>
    <div class='lyr-items'>
      <label style='display:flex;align-items:center;gap:4px;cursor:pointer'><input type='checkbox' id='layer-park-park' checked onchange='renderMap()' style='margin:0'><span style='color:#69db7c'>Parks</span></label>
      <label style='display:flex;align-items:center;gap:4px;cursor:pointer'><input type='checkbox' id='layer-park-forest' checked onchange='renderMap()' style='margin:0'><span style='color:#40c057'>Forests</span></label>
      <label style='display:flex;align-items:center;gap:4px;cursor:pointer'><input type='checkbox' id='layer-park-grass' checked onchange='renderMap()' style='margin:0'><span style='color:#a9e34b'>Grass</span></label>
    </div>
  </details>

  <details class='lyr-group'>
    <summary><span style='color:#da77f2'>Rail &amp; Transit</span></summary>
    <div class='lyr-items'>
      <label style='display:flex;align-items:center;gap:4px;cursor:pointer'><input type='checkbox' id='layer-rail-rail' checked onchange='renderMap()' style='margin:0'><span style='color:#da77f2'>Rail</span></label>
      <label style='display:flex;align-items:center;gap:4px;cursor:pointer'><input type='checkbox' id='layer-rail-subway' checked onchange='renderMap()' style='margin:0'><span style='color:#e599f7'>Subway</span></label>
      <label style='display:flex;align-items:center;gap:4px;cursor:pointer'><input type='checkbox' id='layer-transit-bus' checked onchange='renderMap()' style='margin:0'><span style='color:#fab005'>Bus</span></label>
      <label style='display:flex;align-items:center;gap:4px;cursor:pointer'><input type='checkbox' id='layer-transit-ferry' checked onchange='renderMap()' style='margin:0'><span style='color:#15aabf'>Ferry</span></label>
      <label style='display:flex;align-items:center;gap:4px;cursor:pointer'><input type='checkbox' id='layer-transit-stations' checked onchange='renderMap()' style='margin:0'><span style='color:#f783ac'>Stations</span></label>
    </div>
  </details>

  <details class='lyr-group'>
    <summary><span style='color:#868e96'>Buildings</span></summary>
    <div class='lyr-items'>
      <label style='display:flex;align-items:center;gap:4px;cursor:pointer'><input type='checkbox' id='layer-building' checked onchange='renderMap()' style='margin:0'><span style='color:#868e96'>Buildings</span></label>
      <label style='display:flex;align-items:center;gap:4px;cursor:pointer'><input type='checkbox' id='layer-industrial' checked onchange='renderMap()' style='margin:0'><span style='color:#fab005'>Industrial</span></label>
      <label style='display:flex;align-items:center;gap:4px;cursor:pointer'><input type='checkbox' id='layer-commercial' checked onchange='renderMap()' style='margin:0'><span style='color:#ced4da'>Commercial</span></label>
      <label style='display:flex;align-items:center;gap:4px;cursor:pointer'><input type='checkbox' id='layer-residential-area' checked onchange='renderMap()' style='margin:0'><span style='color:#dee2e6'>Res. Areas</span></label>
    </div>
  </details>

  </div>
  <div style='margin-top:0.5rem;display:flex;gap:1rem;flex-wrap:wrap;font-size:0.8rem;color:var(--panel-fg)'>
    <span><span style='display:inline-block;width:10px;height:10px;background:#ff0000;border-radius:50%;vertical-align:middle'></span> GPS</span>
    <span><span style='display:inline-block;width:10px;height:10px;background:#ffd93d;border-radius:50%;vertical-align:middle'></span> Waypoint</span>
    <span><span id='track-legend-color' style='display:inline-block;width:20px;height:3px;background:#00d9ff;vertical-align:middle'></span> Track</span>
  </div>
</div>
<div style='flex:1;min-width:0;background:var(--panel-bg);padding:1rem;border-radius:8px;border:1px solid var(--border);height:420px;display:flex;flex-direction:column'>
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
  <div id='track-info' style='font-size:0.85rem;color:var(--panel-fg);flex:1;overflow-y:auto'></div>
</div>
<div style='flex:1;min-width:0;background:var(--panel-bg);padding:1rem;border-radius:8px;border:1px solid var(--border);height:420px;display:flex;flex-direction:column'>
  <h3 style='margin:0 0 0.5rem 0;color:var(--panel-fg)'>Transit Routes</h3>
  <div id='routes-list' style='flex:1;overflow-y:auto;font-size:0.85rem'>
    <div style='color:var(--panel-fg)'>Load a map to see routes</div>
  </div>
</div>
</div>

<div style='margin-top:1rem;display:flex;justify-content:center'>
<div style='width:66%;max-width:900px;background:var(--panel-bg);padding:1rem;border-radius:8px;border:1px solid var(--border)'>
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
let searchNavHoldTimer = null;
let searchNavRepeatTimer = null;
let suppressSearchNavClick = false;

function stopSearchNavHold() {
  if (searchNavHoldTimer) {
    clearTimeout(searchNavHoldTimer);
    searchNavHoldTimer = null;
  }
  if (searchNavRepeatTimer) {
    clearInterval(searchNavRepeatTimer);
    searchNavRepeatTimer = null;
  }
}

function startSearchNavHold(direction) {
  stopSearchNavHold();
  suppressSearchNavClick = false;
  searchNavHoldTimer = setTimeout(() => {
    suppressSearchNavClick = true;
    if (direction < 0) prevSearchResult();
    else nextSearchResult();
    searchNavRepeatTimer = setInterval(() => {
      if (direction < 0) prevSearchResult();
      else nextSearchResult();
    }, 120);
  }, 325);
}

function handleSearchNavClick(direction) {
  if (suppressSearchNavClick) {
    suppressSearchNavClick = false;
    return false;
  }
  if (direction < 0) prevSearchResult();
  else nextSearchResult();
  return false;
}

window.addEventListener('pointerup', stopSearchNavHold);
window.addEventListener('pointercancel', stopSearchNavHold);
window.addEventListener('blur', stopSearchNavHold);

// Toggle waypoint add mode
function toggleWaypointMode() {
  waypointMode = !waypointMode;
  const btn = document.getElementById('btn-add-waypoint');
  if (waypointMode) {
    btn.style.background = 'var(--crumb-bg)';
    btn.style.borderColor = 'var(--panel-fg)';
    btn.style.color = '';
    document.getElementById('map-canvas').style.cursor = 'crosshair';
  } else {
    btn.style.background = '';
    btn.style.borderColor = '';
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

// Feature type constants — must match System_Maps.h MapFeatureType enum
const FT_HIGHWAY   = 0x00, FT_ROAD_MAJOR = 0x01, FT_ROAD_MINOR = 0x02, FT_PATH      = 0x03;
const FT_WATER     = 0x10, FT_PARK       = 0x11, FT_LAND_MASK  = 0x12;
const FT_RAILWAY   = 0x20, FT_BUS        = 0x21, FT_FERRY      = 0x22;
const FT_BUILDING  = 0x30, FT_STATION    = 0x40;

// Subtype constants — must match System_Maps.h SUBTYPE_* defines
const ST_HIGHWAY_MOTORWAY = 0, ST_HIGHWAY_TRUNK = 1;
const ST_MAJOR_PRIMARY = 0, ST_MAJOR_SECONDARY = 1;
const ST_MINOR_TERTIARY = 0, ST_MINOR_RESIDENTIAL = 1, ST_MINOR_SERVICE = 2;
const ST_PATH_FOOTWAY = 0, ST_PATH_CYCLEWAY = 1, ST_PATH_TRACK = 2;
const ST_WATER_LAKE = 0, ST_WATER_RIVER = 1, ST_WATER_COASTLINE = 2;
const ST_PARK_PARK = 0, ST_PARK_FOREST = 1, ST_PARK_GRASSLAND = 2;
const ST_RAILWAY_RAIL = 0, ST_RAILWAY_SUBWAY = 1;
const ST_BUILDING_BUILDING = 0, ST_BUILDING_INDUSTRIAL = 1, ST_BUILDING_COMMERCIAL = 2, ST_BUILDING_RESIDENTIAL = 3;

// Header feature-type bitmask constants (must match HWMAP_FTYPE_* in System_Maps.h)
const HF_HIGHWAY  = 1 << 0,  HF_MAJOR    = 1 << 1,  HF_MINOR    = 1 << 2,  HF_PATH     = 1 << 3;
const HF_WATER    = 1 << 4,  HF_PARK     = 1 << 5,  HF_LAND     = 1 << 6;
const HF_RAILWAY  = 1 << 7,  HF_BUS      = 1 << 8,  HF_FERRY    = 1 << 9;
const HF_BUILDING = 1 << 10, HF_STATION  = 1 << 11;

const WEB_LAYER_DEFS = [
  { id: 'layer-road-motorway',  hf: HF_HIGHWAY,  type: FT_HIGHWAY, subtype: ST_HIGHWAY_MOTORWAY },
  { id: 'layer-road-trunk',     hf: HF_HIGHWAY,  type: FT_HIGHWAY, subtype: ST_HIGHWAY_TRUNK },
  { id: 'layer-road-primary',   hf: HF_MAJOR,    type: FT_ROAD_MAJOR, subtype: ST_MAJOR_PRIMARY },
  { id: 'layer-road-secondary', hf: HF_MAJOR,    type: FT_ROAD_MAJOR, subtype: ST_MAJOR_SECONDARY },
  { id: 'layer-road-tertiary',  hf: HF_MINOR,    type: FT_ROAD_MINOR, subtype: ST_MINOR_TERTIARY },
  { id: 'layer-road-residential',hf: HF_MINOR,   type: FT_ROAD_MINOR, subtype: ST_MINOR_RESIDENTIAL },
  { id: 'layer-road-service',   hf: HF_MINOR,    type: FT_ROAD_MINOR, subtype: ST_MINOR_SERVICE },
  { id: 'layer-path-footway',   hf: HF_PATH,     type: FT_PATH, subtype: ST_PATH_FOOTWAY },
  { id: 'layer-path-cycleway',  hf: HF_PATH,     type: FT_PATH, subtype: ST_PATH_CYCLEWAY },
  { id: 'layer-path-track',     hf: HF_PATH,     type: FT_PATH, subtype: ST_PATH_TRACK },
  { id: 'layer-water-lake',     hf: HF_WATER,    type: FT_WATER, subtype: ST_WATER_LAKE },
  { id: 'layer-water-river',    hf: HF_WATER,    type: FT_WATER, subtype: ST_WATER_RIVER },
  { id: 'layer-water-coast',    hf: HF_WATER,    type: FT_WATER, subtype: ST_WATER_COASTLINE },
  { id: 'layer-landmask',       hf: HF_LAND,     type: FT_LAND_MASK },
  { id: 'layer-park-park',      hf: HF_PARK,     type: FT_PARK, subtype: ST_PARK_PARK },
  { id: 'layer-park-forest',    hf: HF_PARK,     type: FT_PARK, subtype: ST_PARK_FOREST },
  { id: 'layer-park-grass',     hf: HF_PARK,     type: FT_PARK, subtype: ST_PARK_GRASSLAND },
  { id: 'layer-rail-rail',      hf: HF_RAILWAY,  type: FT_RAILWAY, subtype: ST_RAILWAY_RAIL },
  { id: 'layer-rail-subway',    hf: HF_RAILWAY,  type: FT_RAILWAY, subtype: ST_RAILWAY_SUBWAY },
  { id: 'layer-transit-bus',    hf: HF_BUS,      type: FT_BUS },
  { id: 'layer-transit-ferry',  hf: HF_FERRY,    type: FT_FERRY },
  { id: 'layer-transit-stations',hf: HF_STATION,  type: FT_STATION },
  { id: 'layer-building',       hf: HF_BUILDING, type: FT_BUILDING, subtype: ST_BUILDING_BUILDING },
  { id: 'layer-industrial',     hf: HF_BUILDING, type: FT_BUILDING, subtype: ST_BUILDING_INDUSTRIAL },
  { id: 'layer-commercial',     hf: HF_BUILDING, type: FT_BUILDING, subtype: ST_BUILDING_COMMERCIAL },
  { id: 'layer-residential-area',hf: HF_BUILDING, type: FT_BUILDING, subtype: ST_BUILDING_RESIDENTIAL }
];

function featureAvailabilityKey(type, subtype) {
  return subtype === undefined || subtype === null ? `${type}:*` : `${type}:${subtype}`;
}

function mapHasFeatureAvailability(type, subtype) {
  if (!currentMap) return true;
  if (currentMap.featureAvailability instanceof Set) {
    return currentMap.featureAvailability.has(featureAvailabilityKey(type, subtype));
  }
  if (!currentMap.features) return false;
  return currentMap.features.some((feature) => feature.type === type && (subtype === undefined || subtype === null || (feature.subtype || 0) === subtype));
}

function mapHasHeaderBit(hfBit) {
  if (!currentMap) return true;
  const mask = currentMap.featureTypeMask;
  if (mask === undefined || mask === 0) return true;  // Old maps without mask: assume all present
  return (mask & hfBit) !== 0;
}

function setLayerAvailabilityState(layerId, available) {
  const input = document.getElementById(layerId);
  if (!input) return;
  input.disabled = !available;
  input.title = available ? '' : 'Not present in this map';
  const label = input.closest('label');
  if (!label) return;
  label.style.opacity = available ? '1' : '0.35';
  label.style.filter = available ? '' : 'grayscale(1)';
  label.style.cursor = available ? 'pointer' : 'not-allowed';
}

function updateLayerAvailability() {
  WEB_LAYER_DEFS.forEach((layer) => {
    const available = layer.hf ? mapHasHeaderBit(layer.hf) : mapHasFeatureAvailability(layer.type, layer.subtype);
    setLayerAvailabilityState(layer.id, available);
  });
}

// LOD zoom thresholds — must match System_Maps.h LOD_ZOOM_* defines
// Below each threshold the feature type is hidden entirely.
// Web renderer adds smooth fade-ins starting at these same cutoffs.
const LOD_MAJOR_ROAD   = 0.15;
const LOD_WATER        = 0.30;
const LOD_RAILWAY      = 0.30;
const LOD_MINOR_ROAD   = 0.50;
const LOD_PARK         = 0.50;
const LOD_TRANSIT      = 0.50;
const LOD_PATH         = 1.00;
const LOD_BUILDING     = 2.00;

// DPP-based LOD thresholds (degrees-per-pixel) — must match map tool preview
const LOD_DPP_BUILDING_CLUSTERS = 0.00025;  // ~28m/px — show cluster blobs above this dpp
const LOD_DPP_LINE_SCALE_REF    = 0.0003;   // ~33m/px — reference dpp where line widths = 1.0x
const LOD_SERVICE_ROAD = 0.70;
const LOD_TRACK        = 0.70;

// Feature colors (widths doubled for high-DPI canvas) - MUST MATCH preview in map-tool
const COLORS = {
  [FT_HIGHWAY]:   { stroke: '#ff6b6b', width: 6 },
  [FT_ROAD_MAJOR]:{ stroke: '#ffd93d', width: 4 },
  [FT_ROAD_MINOR]:{ stroke: '#ffffff', width: 3 },
  [FT_PATH]:      { stroke: '#aaaaaa', width: 2, dash: [8,8] },
  [FT_WATER]:     { stroke: '#4dabf7', width: 2, fill: '#1864ab' },
  [FT_PARK]:      { stroke: '#69db7c', width: 1, fill: '#2b8a3e' },
  [FT_LAND_MASK]: { stroke: '#c9b896', width: 0, fill: '#c9b896' },
  [FT_RAILWAY]:   { stroke: '#da77f2', width: 4, dash: [16,8] },
  [FT_BUS]:       { stroke: '#fab005', width: 3, dash: [12,6] },
  [FT_FERRY]:     { stroke: '#15aabf', width: 4, dash: [16,8] },
  [FT_BUILDING]:  { stroke: '#868e96', width: 2, fill: '#495057' },
  [FT_STATION]:   { stroke: '#f06595', width: 0, radius: 6 }
};

// Subtype-specific colors
const SUBTYPE_COLORS = {
  [FT_HIGHWAY]: {
    [ST_HIGHWAY_MOTORWAY]: { stroke: '#ff6b6b', width: 7 },
    [ST_HIGHWAY_TRUNK]:    { stroke: '#ff8787', width: 5 }
  },
  [FT_ROAD_MAJOR]: {
    [ST_MAJOR_PRIMARY]:   { stroke: '#ffd93d', width: 4 },
    [ST_MAJOR_SECONDARY]: { stroke: '#ffe066', width: 3 }
  },
  [FT_ROAD_MINOR]: {
    [ST_MINOR_TERTIARY]:    { stroke: '#ffffff', width: 3 },
    [ST_MINOR_RESIDENTIAL]: { stroke: '#e9ecef', width: 2 },
    [ST_MINOR_SERVICE]:     { stroke: '#ced4da', width: 1 }
  },
  [FT_PATH]: {
    [ST_PATH_FOOTWAY]:  { stroke: '#aaaaaa', width: 2, dash: [6,6] },
    [ST_PATH_CYCLEWAY]: { stroke: '#74c0fc', width: 2, dash: [8,4] },
    [ST_PATH_TRACK]:    { stroke: '#8b7355', width: 2, dash: [10,5] }
  },
  [FT_WATER]: {
    [ST_WATER_LAKE]:      { stroke: '#4dabf7', width: 2, fill: '#1864ab' },
    [ST_WATER_RIVER]:     { stroke: '#339af0', width: 3 },
    [ST_WATER_COASTLINE]: { stroke: '#1c7ed6', width: 2, fill: '#1864ab' }
  },
  [FT_PARK]: {
    [ST_PARK_PARK]:      { stroke: '#69db7c', width: 1, fill: '#2b8a3e' },
    [ST_PARK_FOREST]:    { stroke: '#40c057', width: 1, fill: '#1b4332' },
    [ST_PARK_GRASSLAND]: { stroke: '#a9e34b', width: 1, fill: '#5c940d' }
  },
  [FT_RAILWAY]: {
    [ST_RAILWAY_RAIL]:   { stroke: '#da77f2', width: 4, dash: [16,8] },
    [ST_RAILWAY_SUBWAY]: { stroke: '#e599f7', width: 3, dash: [8,8] }
  },
  [FT_BUILDING]: {
    [ST_BUILDING_BUILDING]:    { stroke: '#868e96', width: 2, fill: '#495057' },
    [ST_BUILDING_INDUSTRIAL]:  { stroke: '#fab005', width: 2, fill: '#5c4a1d' },
    [ST_BUILDING_COMMERCIAL]:  { stroke: '#4dabf7', width: 2, fill: '#1c4a6e' },
    [ST_BUILDING_RESIDENTIAL]: { stroke: '#69db7c', width: 2, fill: '#2d4a3a' }
  }
};

// Get style for feature using subtype
function getFeatureStyle(type, subtype) {
  if (SUBTYPE_COLORS[type] && SUBTYPE_COLORS[type][subtype]) {
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
        // Show immediate visual feedback before blocking load
        const canvas = document.getElementById('map-canvas');
        const ctx = canvas.getContext('2d');
        const lbg = getComputedStyle(document.documentElement).getPropertyValue('--code-bg').trim() || '#1e1e1e';
        ctx.fillStyle = lbg;
        ctx.fillRect(0, 0, canvas.width, canvas.height);
        ctx.fillStyle = getComputedStyle(document.documentElement).getPropertyValue('--panel-fg').trim() || '#e8e8ee';
        ctx.font = '36px -apple-system, BlinkMacSystemFont, sans-serif';
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';
        ctx.fillText('Loading Map...', canvas.width / 2, canvas.height / 2);
        
        // Use setTimeout to allow UI to update before blocking load
        setTimeout(() => loadMap(filePath), 10);
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
    selectedFeature = null;
    searchResults = [];
    searchResultIndex = 0;
    stopSearchNavHold();
    const infoDiv = document.getElementById('feature-info');
    if (infoDiv) infoDiv.style.display = 'none';
    updateLayerAvailability();
    
    if (currentMap) {
      try {
        const selResp = await fetch('/api/maps/select?file=' + encodeURIComponent(path), { credentials: 'include' });
        const selData = await selResp.json();
        if (!selData || selData.success !== true) {
          console.warn('[MAP] Device map select failed:', selData);
          const wpStatus = document.getElementById('waypoint-status');
          if (wpStatus) wpStatus.textContent = 'Warning: Device map load failed — waypoints unavailable';
        }
      } catch (e) {
        console.warn('[MAP] Device map select error:', e);
      }
      document.getElementById('map-info').style.display = 'block';

      // --- Computed stats ---
      const latSpan = currentMap.maxLat - currentMap.minLat;
      const lonSpan = currentMap.maxLon - currentMap.minLon;
      const centerLat = (currentMap.minLat + currentMap.maxLat) / 2;
      const centerLon = (currentMap.minLon + currentMap.maxLon) / 2;
      const kmH = latSpan * 111.32;
      const kmW = lonSpan * 111.32 * Math.cos(centerLat * Math.PI / 180);
      const fmtKm = v => v >= 10 ? v.toFixed(1) + ' km' : (v * 1000).toFixed(0) + ' m';

      // Feature type counts
      const typeCounts = {};
      let namedCount = 0;
      currentMap.features.forEach(f => {
        typeCounts[f.type] = (typeCounts[f.type] || 0) + 1;
        if (f.name && String(f.name).replace(/\0/g,'').trim()) namedCount++;
      });
      console.log('[MAP] Feature types:', typeCounts);

      const TYPE_META = [
        { type: FT_HIGHWAY,   label: 'Highways',  color: '#ff6b6b' },
        { type: FT_ROAD_MAJOR,label: 'Major Rds', color: '#ffd93d' },
        { type: FT_ROAD_MINOR,label: 'Minor Rds', color: '#e9ecef' },
        { type: FT_PATH,      label: 'Paths',     color: '#aaaaaa' },
        { type: FT_WATER,     label: 'Water',     color: '#4dabf7' },
        { type: FT_PARK,      label: 'Parks',     color: '#69db7c' },
        { type: FT_LAND_MASK, label: 'Land',      color: '#c9b896' },
        { type: FT_RAILWAY,   label: 'Rail',      color: '#da77f2' },
        { type: FT_BUS,       label: 'Bus',       color: '#fab005' },
        { type: FT_FERRY,     label: 'Ferry',     color: '#15aabf' },
        { type: FT_BUILDING,  label: 'Buildings', color: '#868e96' },
        { type: FT_STATION,   label: 'Stations',  color: '#f783ac' },
      ];
      const typeChips = TYPE_META
        .filter(m => typeCounts[m.type])
        .map(m => `<span style="display:inline-flex;align-items:center;gap:4px;background:rgba(255,255,255,0.06);border:1px solid rgba(255,255,255,0.12);border-radius:4px;padding:2px 7px;font-size:0.75rem;white-space:nowrap">
            <span style="width:8px;height:8px;border-radius:50%;background:${m.color};flex-shrink:0"></span>
            <span style="color:var(--panel-fg)">${m.label}</span>
            <strong style="color:#fff">${typeCounts[m.type].toLocaleString()}</strong>
          </span>`).join('');

      // Unknown types (hex codes)
      const knownTypes = new Set(TYPE_META.map(m => m.type));
      const unknownChips = Object.entries(typeCounts)
        .filter(([t]) => !knownTypes.has(Number(t)))
        .map(([t, c]) => `<span style="display:inline-flex;align-items:center;gap:4px;background:rgba(255,255,255,0.04);border:1px solid rgba(255,255,255,0.08);border-radius:4px;padding:2px 7px;font-size:0.75rem">
            <span style="color:#666">0x${Number(t).toString(16)}</span>
            <strong style="color:#aaa">${c.toLocaleString()}</strong>
          </span>`).join('');

      document.getElementById('map-details').innerHTML = `
        <div style="font-size:0.8rem;color:#888;margin-bottom:8px;padding-bottom:8px;border-bottom:1px solid var(--border);display:flex;flex-wrap:wrap;gap:10px;align-items:center">
          <strong style="color:var(--panel-fg)">${currentMap.filename}</strong>
          <span style="color:#555">|</span>
          <span>v${currentMap.version}</span>
          <span style="color:#555">|</span>
          <span>${currentMap.tileGridSize}×${currentMap.tileGridSize} tiles</span>
        </div>

        ${currentMap.regionName ? `<div style="font-size:1rem;font-weight:600;color:var(--panel-fg);margin-bottom:10px">${currentMap.regionName}</div>` : ''}

        <div style="display:flex;gap:12px;margin-bottom:10px;font-size:0.82rem">
          <div><span style="color:#888">Features</span> <strong style="color:var(--panel-fg)">${currentMap.features.length.toLocaleString()}</strong></div>
          <div><span style="color:#888">Named</span> <strong style="color:var(--panel-fg)">${namedCount.toLocaleString()}</strong></div>
          <div><span style="color:#888">Name table</span> <strong style="color:var(--panel-fg)">${(currentMap.names||[]).length.toLocaleString()}</strong></div>
        </div>

        <div style="display:flex;flex-wrap:wrap;gap:5px;margin-bottom:12px">
          ${typeChips}
          ${unknownChips}
        </div>

        <div style="font-size:0.78rem;color:#666;border-top:1px solid var(--border);padding-top:8px;display:grid;grid-template-columns:auto 1fr;gap:3px 8px">
          <span>Lat</span><span style="color:var(--panel-fg)">${currentMap.minLat.toFixed(5)}° → ${currentMap.maxLat.toFixed(5)}°</span>
          <span>Lon</span><span style="color:var(--panel-fg)">${currentMap.minLon.toFixed(5)}° → ${currentMap.maxLon.toFixed(5)}°</span>
          <span>Center</span><span style="color:var(--panel-fg)">${centerLat.toFixed(5)}°, ${centerLon.toFixed(5)}°</span>
          <span>Size</span><span style="color:var(--panel-fg)">${fmtKm(kmH)} N–S · ${fmtKm(kmW)} E–W</span>
        </div>
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

// Parse HWMAP binary format (v6)
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
  if (version !== 6) throw new Error(`Unsupported map version: ${version} (need v6)`);
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
  const featureTypeMask = readU16(); // HWMAP_FTYPE_* bitmask (0 = old file, treat as all present)

  // Extract tiling params from flags (matches generator encoding)
  // bits 0-1: tileGridCode (0=16, 1=32, 2=64), bits 2-6: haloPct (0-31), bits 7-10: quantBits-10
  const tileGridCode = flags & 0x03;
  const tileGridSize = tileGridCode === 0 ? 16 : tileGridCode === 2 ? 64 : 32;
  const haloPct = ((flags >> 2) & 0x1F) / 100.0;
  const quantBits = Math.min(16, ((flags >> 7) & 0x0F) + 10);
  const qMax = (1 << quantBits);
  const tileCount = tileGridSize * tileGridSize;

  console.log(`[MAP] v${version} tiled: ${tileGridSize}x${tileGridSize}, halo=${haloPct}, quantBits=${quantBits}`);
  const hdrSize = 6;  // type(1) + subtype(1) + nameIndex(2) + pointCount(2)

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
  const featureAvailability = new Set();
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
        const subtype = readU8();
        const nameIndex = readU16();
        const pointCount = readU16();

        if (pointCount > 0) {
          featureAvailability.add(featureAvailabilityKey(type, null));
          featureAvailability.add(featureAvailabilityKey(type, subtype));
        }

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
    spatialIndex: buildSpatialIndex(), featureAvailability, featureTypeMask
  };
}

// Render map to canvas
function renderMap() {
  const canvas = document.getElementById('map-canvas');
  const ctx = canvas.getContext('2d');
  
  // Enable anti-aliasing
  ctx.imageSmoothingEnabled = true;
  ctx.imageSmoothingQuality = 'high';
  
  // Clear with theme background color (must be a solid color, not a gradient)
  const bgColor = getComputedStyle(document.documentElement).getPropertyValue('--code-bg').trim() || '#1e1e1e';
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
  
  // Compute degrees-per-pixel for DPP-based LOD (matches map tool preview)
  const dpp = 1 / scale;
  const dppRatio = dpp / LOD_DPP_LINE_SCALE_REF;
  
  // Set line style for smooth rendering
  ctx.lineCap = 'round';
  ctx.lineJoin = 'round';

  const isOn = (id, def = true) => {
    const el = document.getElementById(id);
    return el ? el.checked !== false : def;
  };

  const layerEnabled = (type, subtype) => {
    const st = subtype || 0;

    if (type === FT_LAND_MASK) return isOn('layer-landmask', true);

    if (type === FT_HIGHWAY)    return st === ST_HIGHWAY_MOTORWAY ? isOn('layer-road-motorway', true) : isOn('layer-road-trunk', true);
    if (type === FT_ROAD_MAJOR) return st === ST_MAJOR_PRIMARY    ? isOn('layer-road-primary', true) : isOn('layer-road-secondary', true);
    if (type === FT_ROAD_MINOR) return st === ST_MINOR_TERTIARY   ? isOn('layer-road-tertiary', true) : (st === ST_MINOR_RESIDENTIAL ? isOn('layer-road-residential', true) : isOn('layer-road-service', true));
    if (type === FT_PATH)       return st === ST_PATH_FOOTWAY     ? isOn('layer-path-footway', true) : (st === ST_PATH_CYCLEWAY ? isOn('layer-path-cycleway', true) : isOn('layer-path-track', true));
    if (type === FT_WATER)      return st === ST_WATER_LAKE       ? isOn('layer-water-lake', true) : (st === ST_WATER_RIVER ? isOn('layer-water-river', true) : isOn('layer-water-coast', true));
    if (type === FT_PARK)       return st === ST_PARK_PARK        ? isOn('layer-park-park', true) : (st === ST_PARK_FOREST ? isOn('layer-park-forest', true) : isOn('layer-park-grass', true));
    if (type === FT_RAILWAY)    return st === ST_RAILWAY_RAIL     ? isOn('layer-rail-rail', true) : isOn('layer-rail-subway', true);
    if (type === FT_BUS)     return isOn('layer-transit-bus', true);
    if (type === FT_FERRY)   return isOn('layer-transit-ferry', true);
    if (type === FT_STATION) return isOn('layer-transit-stations', true);
    if (type === FT_BUILDING)   return st === ST_BUILDING_BUILDING ? isOn('layer-building', true) : (st === ST_BUILDING_INDUSTRIAL ? isOn('layer-industrial', true) : (st === ST_BUILDING_COMMERCIAL ? isOn('layer-commercial', true) : isOn('layer-residential-area', true)));

    return true;
  };

  const anyWaterEnabled = layerEnabled(FT_WATER, ST_WATER_LAKE) || layerEnabled(FT_WATER, ST_WATER_RIVER) || layerEnabled(FT_WATER, ST_WATER_COASTLINE);
  const anyParkEnabled = layerEnabled(FT_PARK, ST_PARK_PARK) || layerEnabled(FT_PARK, ST_PARK_FOREST) || layerEnabled(FT_PARK, ST_PARK_GRASSLAND);
  
  // Check if map has land mask features (coastal map)
  const hasLandMask = m.features.some(f => f.type === FT_LAND_MASK) && layerEnabled(FT_LAND_MASK, 0);
  
  // Strategy: tan bg -> coastline water (combined) -> island land masks -> inland water
  // Mainland = tan background. Coastline water carves out ocean. Island land masks restore islands.
  // Coordinates are now in DEGREES (converted in parser)
  
  // 1. Fill background with LAND color (tan)
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
  
  // 2. Draw coastline water as combined path (carve ocean from tan bg)
  //    Combined path avoids tile-boundary seams in ocean.
  if (layerEnabled(FT_WATER, ST_WATER_COASTLINE)) {
    const coastlinePolys = [];
    for (const feature of m.features) {
      if (feature.type !== FT_WATER || (feature.subtype ?? 0) !== ST_WATER_COASTLINE) continue;
      if (feature.points.length < 3) continue;
      coastlinePolys.push(feature.points);
    }
    if (coastlinePolys.length > 0) {
      ctx.beginPath();
      ctx.fillStyle = COLORS[FT_WATER].fill;
      for (const pts of coastlinePolys) {
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
  
  // 3. Draw island land masks on top of ocean (restore islands)
  //    Only closed-path land polygons (islands) are stored as land masks.
  //    Mainland is the tan background — no open-path land masks needed.
  if (hasLandMask) {
    const validLandMasks = [];
    for (const feature of m.features) {
      if (feature.type !== FT_LAND_MASK) continue;
      const pts = feature.points;
      if (pts.length < 3) continue;
      const isClosed = Math.abs(pts[0].lat - pts[pts.length-1].lat) < 0.01 &&
                       Math.abs(pts[0].lon - pts[pts.length-1].lon) < 0.01;
      if (!isClosed) continue;
      validLandMasks.push(pts);
    }
    if (validLandMasks.length > 0) {
      ctx.beginPath();
      ctx.fillStyle = COLORS[FT_LAND_MASK].fill;
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
  
  // 4. Draw individual water polygons (inland lakes, rivers) on top of land
  //    Coastline water already drawn as combined path above — skip it here.
  if (anyWaterEnabled) {
    for (const feature of m.features) {
      if (feature.type !== FT_WATER) continue;
      if (!layerEnabled(FT_WATER, feature.subtype || 0)) continue;
      if ((feature.subtype ?? 0) === ST_WATER_COASTLINE) continue;  // Already drawn above
      const pts = feature.points;
      if (pts.length < 3) continue;
      ctx.beginPath();
      ctx.fillStyle = COLORS[FT_WATER].fill;
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
  
  // 5. Draw building clusters when zoomed out (BEFORE parks so parks show on top)
  if (dpp >= LOD_DPP_BUILDING_CLUSTERS && m.spatialIndex && layerEnabled(FT_BUILDING, ST_BUILDING_BUILDING)) {
    const si = m.spatialIndex;
    for (let cellIdx = 0; cellIdx < si.cells.length; cellIdx++) {
      let cMinLat = Infinity, cMaxLat = -Infinity;
      let cMinLon = Infinity, cMaxLon = -Infinity;
      let buildingCount = 0;
      for (const fi of si.cells[cellIdx]) {
        const f = m.features[fi];
        if (f && f.type === FT_BUILDING && (f.subtype ?? 0) === ST_BUILDING_BUILDING && f.points.length >= 2) {
          for (const pt of f.points) {
            if (pt.lat < cMinLat) cMinLat = pt.lat;
            if (pt.lat > cMaxLat) cMaxLat = pt.lat;
            if (pt.lon < cMinLon) cMinLon = pt.lon;
            if (pt.lon > cMaxLon) cMaxLon = pt.lon;
          }
          buildingCount++;
        }
      }
      if (buildingCount < 3) continue;
      const c1 = toCanvas(cMaxLat, cMinLon);
      const c2 = toCanvas(cMinLat, cMaxLon);
      const cx = Math.min(c1.x, c2.x);
      const cy = Math.min(c1.y, c2.y);
      const cw = Math.abs(c2.x - c1.x);
      const ch = Math.abs(c2.y - c1.y);
      if (cw > 4 && ch > 4) {
        const alpha = Math.min(0.25, 0.05 + buildingCount * 0.008);
        ctx.fillStyle = `rgba(73, 80, 87, ${alpha})`;
        const r = Math.min(4, cw / 4, ch / 4);
        ctx.beginPath();
        ctx.roundRect(cx, cy, cw, ch, r);
        ctx.fill();
      }
    }
  }
  
  // 6. Draw parks (green areas on top of building clusters)
  if (anyParkEnabled) {
    const parkFeatures = m.features.filter(f => f.type === FT_PARK);
    for (const feature of parkFeatures) {
      if (!layerEnabled(FT_PARK, feature.subtype || 0)) continue;
      const pts = feature.points;
      if (pts.length < 3) continue;
      
      const isClosed = Math.abs(pts[0].lat - pts[pts.length-1].lat) < 0.01 &&
                       Math.abs(pts[0].lon - pts[pts.length-1].lon) < 0.01;
      if (!isClosed) continue;
      
      const style = getFeatureStyle(feature.type, feature.subtype || 0);
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
  const renderOrder = [FT_BUILDING, FT_PATH, FT_ROAD_MINOR, FT_ROAD_MAJOR, FT_FERRY, FT_BUS, FT_RAILWAY, FT_HIGHWAY, FT_STATION];
  
  // First pass: draw all highway casings (outlines) together
  ctx.beginPath();
  ctx.strokeStyle = '#8b0000';  // Dark red casing
  // Scale line width using DPP ratio matching generator preview
  let casingLineScale = 1;
  if (dppRatio < 1) {
    casingLineScale = Math.max(0.3, Math.sqrt(dppRatio));
  } else {
    casingLineScale = Math.max(0.4, 1 / dppRatio);
  }
  ctx.lineWidth = (COLORS[FT_HIGHWAY].width + 4) * casingLineScale;
  ctx.setLineDash([]);
  // Match preview: iterate ALL highways without culling for casings
  for (const feature of m.features) {
    if (feature.type !== FT_HIGHWAY || feature.points.length < 2) continue;
    if (!layerEnabled(FT_HIGHWAY, feature.subtype || 0)) continue;
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
      
      const style = getFeatureStyle(feature.type, feature.subtype || 0);
      const pts = feature.points;
      
      // LOD with smooth fade-in transitions (matching preview)
      let fadeOpacity = 1.0;
      let fadeLineScale = 1.0;
      
      // LOD: hide features below the shared threshold, then fade in over a short range
      if (feature.type === FT_ROAD_MAJOR) {
        if (zoom < LOD_MAJOR_ROAD) continue;
        if (zoom < LOD_MAJOR_ROAD + 0.15) { const t = (zoom - LOD_MAJOR_ROAD) / 0.15; fadeOpacity = t * t; fadeLineScale = 0.3 + t * 0.7; }
      }
      if (feature.type === FT_WATER || feature.type === FT_RAILWAY) {
        if (zoom < LOD_WATER) continue;
        if (zoom < LOD_WATER + 0.15) { const t = (zoom - LOD_WATER) / 0.15; fadeOpacity = t * t; fadeLineScale = 0.3 + t * 0.7; }
      }
      if (feature.type === FT_ROAD_MINOR) {
        if (zoom < LOD_MINOR_ROAD) continue;
        if (zoom < LOD_MINOR_ROAD + 0.20) { const t = (zoom - LOD_MINOR_ROAD) / 0.20; fadeOpacity = t * t; fadeLineScale = 0.3 + t * 0.7; }
      }
      if (feature.type === FT_PARK) {
        if (zoom < LOD_PARK) continue;
        if (zoom < LOD_PARK + 0.20) { const t = (zoom - LOD_PARK) / 0.20; fadeOpacity = t * t; fadeLineScale = 0.3 + t * 0.7; }
      }
      if (feature.type === FT_BUS || feature.type === FT_STATION) {
        if (zoom < LOD_TRANSIT) continue;
        if (zoom < LOD_TRANSIT + 0.20) { const t = (zoom - LOD_TRANSIT) / 0.20; fadeOpacity = t * t; fadeLineScale = 0.3 + t * 0.7; }
      }
      if (feature.type === FT_PATH) {
        if (zoom < LOD_PATH) continue;
        if (zoom < LOD_PATH + 0.30) { const t = (zoom - LOD_PATH) / 0.30; fadeOpacity = t * t; fadeLineScale = 0.2 + t * 0.8; }
      }
      if (feature.type === FT_ROAD_MINOR && (feature.subtype || 0) === ST_MINOR_SERVICE && zoom < LOD_SERVICE_ROAD) continue;
      if (feature.type === FT_PATH       && (feature.subtype || 0) === ST_PATH_TRACK    && zoom < LOD_TRACK) continue;
      if (feature.type === FT_BUILDING) {
        if (zoom < LOD_BUILDING) continue;
        // Sharp step: semi-transparent buildings look terrible as "transparent squares"
        if (zoom < LOD_BUILDING + 1.0) { const t = (zoom - LOD_BUILDING) / 1.0; if (t < 0.3) continue; fadeOpacity = 1.0; fadeLineScale = 1.0; }
      }
      
      ctx.beginPath();
      ctx.strokeStyle = style.stroke;
      // Scale line width using DPP ratio matching generator preview
      let lineScale = 1;
      if (feature.type === FT_HIGHWAY || feature.type === FT_ROAD_MAJOR) {
        if (dppRatio < 1) {
          lineScale = Math.max(0.3, Math.sqrt(dppRatio));
        } else {
          lineScale = Math.max(0.4, 1 / dppRatio);
        }
      } else if (feature.type === FT_FERRY) {
        if (dppRatio < 1) {
          lineScale = Math.max(0.25, Math.sqrt(dppRatio));
        } else {
          lineScale = Math.max(0.35, 1 / dppRatio);
        }
      } else if (feature.type === FT_PATH) {
        if (dppRatio > 1) {
          lineScale = Math.max(0.5, 1 / dppRatio);
        }
      } else if (dppRatio > 1) {
        lineScale = Math.max(0.4, 1 / dppRatio);
      }
      ctx.lineWidth = style.width * lineScale * fadeLineScale;
      const alpha = feature.type === FT_FERRY ? (fadeOpacity * 0.55) : fadeOpacity;
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
      
      if (style.fill && isClosed && feature.type === FT_BUILDING) {
        ctx.closePath();
        ctx.fillStyle = style.fill;
        ctx.globalAlpha = fadeOpacity;
        ctx.fill();
        ctx.globalAlpha = 1.0;
        continue;
      }
      
      if (style.width > 0) ctx.stroke();
      ctx.globalAlpha = 1.0;
    }
  }
  
  // Draw street/feature names with zoom-based visibility
  ctx.textAlign = 'center';
  ctx.textBaseline = 'middle';
  
  const drawnLabels = [];  // Track all label positions to avoid general overlap
  const drawnNamePositions = new Map();  // name -> [{x,y}] for same-name spacing dedup

  // Max instances of same name allowed on screen, by feature type
  const NAME_MAX = {
    [FT_HIGHWAY]:    1,
    [FT_ROAD_MAJOR]: 1,
    [FT_RAILWAY]:    1,
    [FT_BUS]:        1,
    [FT_FERRY]:      1,
    [FT_WATER]:      1,
    [FT_PARK]:       1,
  };
  const NAME_MAX_DEFAULT = 2;
  // Minimum pixel distance between two labels with the same name
  const MIN_SAME_NAME_PX = 300;
  
  for (const feature of m.features) {
    if (!feature.name || feature.points.length < 2) continue;
    if (!layerEnabled(feature.type, feature.subtype || 0)) continue;

    const displayName = String(feature.name).replace(/\0/g, '').trim();
    if (!displayName) continue;
    const nameKey = displayName.toLowerCase();
    
    // Check per-type instance limit
    const maxInst = (NAME_MAX[feature.type] !== undefined) ? NAME_MAX[feature.type] : NAME_MAX_DEFAULT;
    const prevPositions = drawnNamePositions.get(nameKey);
    if (prevPositions && prevPositions.length >= maxInst) continue;
    
    // Zoom-based visibility by feature type
    let minZoom = 20;  // Default: don't show
    let fontSize = 18;
    
    if (feature.type === FT_HIGHWAY) {
      minZoom = 5;
      fontSize = zoom >= 10 ? 28 : 22;
    } else if (feature.type === FT_ROAD_MAJOR) {
      minZoom = 7;
      fontSize = zoom >= 12 ? 24 : 20;
    } else if (feature.type === FT_ROAD_MINOR) {
      minZoom = 10;
      fontSize = 18;
    } else if (feature.type === FT_PATH) {
      minZoom = 15;
      fontSize = 16;
    } else if (feature.type === FT_WATER) {
      minZoom = 8;
      fontSize = zoom >= 12 ? 24 : 20;
    } else if (feature.type === FT_PARK) {
      minZoom = 8;
      fontSize = zoom >= 12 ? 24 : 20;
    } else if (feature.type === FT_BUILDING) {
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
    
    // Skip if too close to any existing label (general overlap)
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

    // Skip if too close to a previous instance of the same name
    if (prevPositions) {
      for (const p of prevPositions) {
        const dx = pos.x - p.x;
        const dy = pos.y - p.y;
        if (dx * dx + dy * dy < MIN_SAME_NAME_PX * MIN_SAME_NAME_PX) {
          tooClose = true;
          break;
        }
      }
      if (tooClose) continue;
    }
    
    // Draw text with outline for visibility
    ctx.strokeStyle = 'rgba(0,0,0,0.8)';
    ctx.lineWidth = 4;
    ctx.strokeText(displayName, pos.x, pos.y);
    
    // Color based on feature type
    if      (feature.type === FT_HIGHWAY)   ctx.fillStyle = '#ff6b6b';
    else if (feature.type === FT_ROAD_MAJOR) ctx.fillStyle = '#ffd93d';
    else if (feature.type === FT_WATER)      ctx.fillStyle = '#4dabf7';
    else if (feature.type === FT_PARK)       ctx.fillStyle = '#69db7c';
    else if (feature.type === FT_BUILDING)   ctx.fillStyle = '#b197fc';
    else ctx.fillStyle = '#ffffff';
    
    ctx.fillText(displayName, pos.x, pos.y);
    drawnLabels.push({ x: pos.x, y: pos.y, w: labelWidth });
    if (prevPositions) prevPositions.push({ x: pos.x, y: pos.y });
    else drawnNamePositions.set(nameKey, [{ x: pos.x, y: pos.y }]);
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
  try {
    await loadWaypoints();
  } catch (e) {
    renderMap();
    throw new Error('Waypoint added but failed to refresh list: ' + e.message);
  }
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

  // Preserve open/closed state of existing details elements
  const openStates = {};
  el.querySelectorAll('details.lyr-group').forEach((details) => {
    const summary = details.querySelector('summary span');
    if (summary) {
      const title = summary.textContent.split(' (')[0]; // Extract title before count
      openStates[title] = details.open;
    }
  });

  const sections = [
    { type: FT_RAILWAY, title: 'Rail',  color: '#da77f2' },
    { type: FT_BUS,     title: 'Bus',   color: '#fab005' },
    { type: FT_FERRY,   title: 'Ferry', color: '#15aabf' }
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
    if (items.length === 0) continue;

    const wasOpen = openStates[sec.title] || false;
    html += `<details class="lyr-group" style="margin-bottom:4px"${wasOpen ? ' open' : ''}>`;
    html += `<summary><span style="color:${sec.color}">${sec.title} (${items.length})</span></summary>`;
    html += '<div style="display:flex;flex-direction:column;gap:3px;padding:4px 4px 6px 16px">';
    for (let i = 0; i < items.length && i < 60; i++) {
      const item = items[i];
      const suffix = item.count > 1 ? ` (${item.count})` : '';
      html += `<div data-route-name="${encodeURIComponent(item.name)}" style="cursor:pointer;padding:5px 8px;border:1px solid var(--border);border-radius:4px;background:var(--crumb-bg);font-size:0.8rem;color:var(--panel-fg)">${item.name}${suffix}</div>`;
    }
    html += '</div></details>';
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
  const dlg = document.getElementById('search-dialog');
  dlg.style.display = 'block';
  const inp = document.getElementById('search-input');
  inp.value = '';
  document.getElementById('search-results').innerHTML = '';
  inp.focus();
  // Dismiss on click outside
  setTimeout(() => {
    document.addEventListener('pointerdown', _searchOutside, { once: true, capture: true });
  }, 0);
}

function _searchOutside(e) {
  const dlg = document.getElementById('search-dialog');
  if (dlg && !dlg.contains(e.target)) {
    hideSearchDialog();
  } else {
    document.addEventListener('pointerdown', _searchOutside, { once: true, capture: true });
  }
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
  
  let html = '';
  matches.forEach((match, idx) => {
    html += `<div style="padding:8px 12px;border-bottom:1px solid var(--border);cursor:pointer;font-size:0.9rem;color:var(--menu-item-fg)" 
             onmouseover="this.style.background='var(--crumb-bg)'" 
             onmouseout="this.style.background=''"
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
  const typeNames = {[FT_HIGHWAY]:'Highway',[FT_ROAD_MAJOR]:'Major Road',[FT_ROAD_MINOR]:'Minor Road',[FT_PATH]:'Path',[FT_WATER]:'Water',[FT_PARK]:'Park',[FT_RAILWAY]:'Railway',[FT_BUS]:'Bus',[FT_FERRY]:'Ferry',[FT_BUILDING]:'Building'};
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
        <button id="feature-prev-btn" type="button" onpointerdown="startSearchNavHold(-1)" onpointerup="stopSearchNavHold()" onpointerleave="stopSearchNavHold()" onpointercancel="stopSearchNavHold()" onclick="return handleSearchNavClick(-1)" style="padding:4px 10px;background:#333;color:#fff;border:1px solid #555;border-radius:4px;cursor:pointer">&lt; Prev</button>
        <span style="color:#4dabf7;font-size:0.85rem">${searchResultIndex + 1} / ${searchResults.length}</span>
        <button id="feature-next-btn" type="button" onpointerdown="startSearchNavHold(1)" onpointerup="stopSearchNavHold()" onpointerleave="stopSearchNavHold()" onpointercancel="stopSearchNavHold()" onclick="return handleSearchNavClick(1)" style="padding:4px 10px;background:#333;color:#fff;border:1px solid #555;border-radius:4px;cursor:pointer">Next &gt;</button>
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
  stopSearchNavHold();
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
  // Update legend color
  const legend = document.getElementById('track-legend-color');
  if (legend) legend.style.background = trackColor;
  renderMap();
}

function applyColorPreset() {
  const preset = document.getElementById('track-color-preset');
  trackColor = preset.value;
  document.getElementById('track-color').value = trackColor;
  // Update legend color
  const legend = document.getElementById('track-legend-color');
  if (legend) legend.style.background = trackColor;
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

async function toggleLiveTrack() {
  if (liveTrackInterval) {
    clearInterval(liveTrackInterval);
    liveTrackInterval = null;
    await fetch('/api/gps/tracks?live=stop');
    document.getElementById('track-info').textContent = 'Live tracking stopped';
  } else {
    await fetch('/api/gps/tracks?live=start');
    lastLiveUpdate = 0;
    liveTrackInterval = setInterval(pollLiveTrack, 1000);
    pollLiveTrack();
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
