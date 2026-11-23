'use strict';
// Run the shipping IMU scripts of the web pages for both IMU builds: the
// Sensors page's device detection and already-running check, the dashboard's
// sensor list, card visibility and "detected but not compiled" notice, and
// the IMU card's reading. Only the DOM, the hw helpers and the fetches are fake.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const page = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
let checks = 0;
function equal(actual, expected, label) {
  checks++;
  assert.deepEqual(actual, expected, label);
}

// window.__hwImu as the firmware writes it for a build.
function identity(build) {
  const b = page.builds[build];
  return page.identity.replace('%s', b.names).replace('%s', b.part).replace('%s', b.part);
}
const quiet = {log() {}, warn() {}, error(...args) { throw new Error('page error: ' + args.join(' ')); }};
function device(name, address) { return {name, address, bus: 1, isConnected: true}; }
const flush = () => new Promise(resolve => setImmediate(resolve));

// ---- Sensors page: device detection -------------------------------------------
async function detect(build, devices, status) {
  const shown = new Map();
  const grid = {innerHTML: '', firstChild: null, inserted: [], insertBefore(node) { this.inserted.push(node); }};
  const elements = new Map([['sensors-grid', grid], ['sensors-loading', {}]]);
  const hw = {
    $: id => elements.get(id) || {id},
    toggle(el, show) { if (el && el.id) shown.set(el.id, !!show); },
    hide() {}, show() {},
    fetchJSON: url => Promise.resolve(url === '/api/devices' ? {devices} : status),
  };
  const window = {};
  const document = {createElement: () => ({style: {}, innerHTML: ''})};
  const context = vm.createContext({window, hw, document, console: quiet});
  vm.runInContext(identity(build), context, {filename: 'identity'});
  vm.runInContext(page.detection, context, {filename: 'WebPage_Sensors.h detection'});
  await flush();
  await flush();
  return {imu: shown.get('sensor-card-imu'), notices: grid.inserted.map(node => node.innerHTML), grid};
}

// ---- Sensors page: sensors already running when the page opens --------------------
async function alreadyActive(build, devices, status) {
  const started = [];
  const indicators = new Map();
  const hw = {
    $: id => { if (!indicators.has(id)) indicators.set(id, {className: ''}); return indicators.get(id); },
    fetchJSON: url => Promise.resolve(url === '/api/devices' ? {devices} : status),
  };
  const window = {};
  const context = vm.createContext({window, hw, console: quiet, startSensorPolling: type => started.push(type)});
  vm.runInContext(identity(build), context, {filename: 'identity'});
  vm.runInContext(page.active + '\ncheckAlreadyActiveSensors();', context, {filename: 'WebPage_Sensors.h active'});
  await flush();
  await flush();
  return {started, indicator: (indicators.get('gyro-status-indicator') || {}).className || ''};
}

// ---- Dashboard: the sensor list and the notice -----------------------------------
function dashboard(build, withImuDef) {
  const grid = {firstChild: null, inserted: [], insertBefore(node) { this.inserted.push(node); }};
  const hw = {$: id => id === 'sensor-grid' ? grid : null, toggle() {}};
  const window = {Dash: {}, __dashSensorDefs: [], addEventListener() {}};
  window.createSensorCards = () => 'cards';
  const document = {createElement: () => ({style: {}, innerHTML: ''})};
  const context = vm.createContext({window, hw, document, console: quiet, setInterval() {}});
  vm.runInContext(identity(build), context, {filename: 'identity'});
  if (withImuDef) vm.runInContext(page.dashDef, context, {filename: 'HAL_Motion_Web.h def'});
  vm.runInContext(page.dashList, context, {filename: 'WebPage_Dashboard.h list'});
  vm.runInContext(page.dashNotice, context, {filename: 'WebPage_Dashboard.h notice'});
  return {
    available: devices => JSON.parse(JSON.stringify(window.getAvailableSensors({devices}))),
    notice(devices, status) {
      grid.inserted.length = 0;
      window.createSensorCards(status, {devices});
      return grid.inserted.map(node => node.innerHTML);
    },
  };
}

// ---- Dashboard: which device cards show (Dash.updateDeviceVisibility) ---------------
function visibleCards(build, devices) {
  const shown = new Map();
  const window = {Dash: {showHideCard: (id, show) => shown.set(id, !!show), setIndicator() {}}};
  const context = vm.createContext({window, hw: {$: () => null}, console: quiet});
  vm.runInContext(identity(build), context, {filename: 'identity'});
  vm.runInContext(page.dashVisibility, context, {filename: 'WebPage_Dashboard.h visibility'});
  window.Dash.updateDeviceVisibility({devices});
  return shown;
}

// ---- The IMU card's reading ---------------------------------------------------------
async function reading(json) {
  const element = {textContent: '', innerHTML: ''};
  const orientations = [];
  const hw = {$: id => id === 'gyro-data' ? element : null, fetchJSON: () => Promise.resolve(json)};
  const window = {updateDeviceOrientation: (...args) => orientations.push(args)};
  const context = vm.createContext({window, hw, console: quiet, Date, Number});
  vm.runInContext(page.reader, context, {filename: 'HAL_Motion_Web.h reader'});
  await window._sensorReaders.imu();
  return {html: element.innerHTML, text: element.textContent, orientations};
}

(async () => {
  const on = {imuCompiled: true, imuRunning: true};
  const lsm = [device('LSM6DSOX', 0x6A), device('LIS3MDL', 0x1C), device('SSD1306', 0x3D)];
  const bno = [device('BNO055', 0x28), device('SSD1306', 0x3D)];

  // The identity statement itself, for both builds.
  for (const [build, part] of [['bno055', 'BNO055'], ['lsm6dsox', 'LSM6DSOX']]) {
    const context = vm.createContext({window: {}});
    vm.runInContext(identity(build), context);
    equal(context.window.__hwImu.part, part, build + ': the chip');
    equal(context.window.__hwImu.label, 'IMU (' + part + ')', build + ': the label');
  }

  // Sensors page: the IMU card shows for the build's chip(s), by the table's names.
  equal((await detect('lsm6dsox', lsm, on)).imu, true, '4517 build: the IMU card shows');
  equal((await detect('lsm6dsox', [device('LIS3MDL', 0x1C)], on)).imu, true, '4517 build: one of its chips is enough');
  equal((await detect('bno055', bno, on)).imu, true, 'BNO055 build: the IMU card shows');
  equal((await detect('lsm6dsox', bno, on)).imu, false, '4517 build: a BNO055 on the bus is not its IMU');
  equal((await detect('bno055', lsm, on)).imu, false, 'BNO055 build: the 4517 chips are not its IMU');
  equal((await detect('lsm6dsox', [device('SSD1306', 0x3D)], on)).imu, false, 'no IMU on the bus: no card');
  // Found but not compiled in (a build without the IMU names the table's row).
  const off = await detect('bno055', bno, {imuCompiled: false});
  equal(off.imu, false, 'not compiled: no card');
  equal(off.notices.length, 1, 'not compiled: one notice');
  equal(off.notices[0].includes('IMU (BNO055)'), true, 'the notice names the chip the table has');
  equal((await detect('lsm6dsox', lsm, on)).notices.length, 0, 'compiled in: no notice');

  // Sensors page: an IMU that is already running starts its polling once.
  const active = await alreadyActive('lsm6dsox', lsm, on);
  equal([...new Set(active.started)], ['imu'], '4517 build: the running IMU is polled');
  equal(active.indicator, 'status-indicator status-enabled', '4517 build: its indicator is on');
  equal((await alreadyActive('bno055', bno, on)).started, ['imu'], 'BNO055 build: the running IMU is polled');
  equal((await alreadyActive('lsm6dsox', lsm, {imuCompiled: true, imuRunning: false})).started, [],
        'a stopped IMU is not polled');
  equal((await alreadyActive('lsm6dsox', bno, on)).started, [], "another build's chip is not this IMU");

  // Dashboard: one IMU entry, named after the build's chip.
  const dashLsm = dashboard('lsm6dsox', true);
  equal(dashLsm.available(lsm), [{key: 'imu', name: 'IMU (LSM6DSOX)', desc: 'Gyroscope & Accelerometer'}],
        '4517 build: one IMU entry for its two chips');
  equal(dashLsm.available([device('LIS3MDL', 0x1C)]).length, 1, '4517 build: one chip is enough');
  equal(dashLsm.available(bno), [], '4517 build: a BNO055 is not its IMU');
  const dashBno = dashboard('bno055', true);
  equal(dashBno.available(bno), [{key: 'imu', name: 'IMU (BNO055)', desc: 'Gyroscope & Accelerometer'}],
        'BNO055 build: the IMU entry');
  equal(dashBno.available(lsm), [], 'BNO055 build: the 4517 chips are not its IMU');
  // The notice for a chip that is found but not compiled in (no IMU entry then).
  const dashOff = dashboard('bno055', false);
  equal(dashOff.available(bno), [], 'not compiled: no entry');
  const notice = dashOff.notice(bno, {imuCompiled: false});
  equal(notice.length === 1 && notice[0].includes('IMU (BNO055)'), true, 'not compiled: the notice names the chip');
  equal(dashLsm.notice(lsm, {imuCompiled: true}), [], 'compiled in: no notice');
  const twice = dashboard('lsm6dsox', false).notice(lsm, {imuCompiled: false});
  equal(twice.length === 1 && twice[0].split('IMU (LSM6DSOX)').length === 2, true,
        'two chips of one module are named once');

  // Dashboard: the IMU card's visibility follows the same names.
  equal(visibleCards('lsm6dsox', lsm).get('dash-imu-card'), true, '4517 build: the dashboard IMU card shows');
  equal(visibleCards('lsm6dsox', bno).get('dash-imu-card'), false, '4517 build: not for a BNO055');
  equal(visibleCards('bno055', bno).get('dash-imu-card'), true, 'BNO055 build: the dashboard IMU card shows');
  equal(visibleCards('bno055', [device('SSD1306', 0x3D)]).get('dash-imu-card'), false, 'no IMU: no card');

  // The IMU card: yaw is a heading only when the reading says so ("hdg").
  const base = {valid: true, connected: true, ts: 5, accel: {x: 0.1, y: 0.2, z: 9.8}, gyro: {x: 0, y: 0, z: 0},
                ori: {yaw: 123.4, pitch: -5, roll: 2}, temp: 28};
  const relative = await reading({...base, hdg: false, backend: 'LSM6DSOX'});
  equal(relative.html.includes('Yaw (relative)=123.4'), true, 'no heading: yaw is labelled relative');
  equal(relative.orientations, [[-5, 2, 123.4]], 'the drawing still turns with the reading');
  const heading = await reading({...base, hdg: true, backend: 'BNO055'});
  equal(heading.html.includes('Euler: Yaw=123.4') && !heading.html.includes('relative'), true,
        'a heading: yaw as before');
  // A reading without the key (another device's, over ESP-NOW) reads as before.
  equal((await reading(base)).html.includes('Euler: Yaw=123.4'), true, 'no hdg key: yaw as before');
  equal((await reading({valid: false, connected: true, ts: 0})).text.startsWith('IMU not ready'), true,
        'no reading yet');

  console.log('IMU web surfaces: ' + checks + ' checks passed (detection, running check, dashboard list, visibility and notice, card reading)');
})().catch(error => { console.error(error); process.exit(1); });
