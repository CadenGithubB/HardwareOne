// Pure personal casting preference: no browser renderer or optional audit lab.
(function () {
  var G = (0,eval)('this'), checks = 0, source = slurp('assets/games/src/01-casting-styles.js');
  var config = slurp('assets/games/src/01-config-state.js');
  var settingsSource = config.slice(config.indexOf('// ── Settings / Quality'),config.indexOf('// ── Day/Night Cycle'));
  var oldRandom = Math.random;
  function check(name,value) { if (!value) throw Error(name); checks++; __out('PASS '+name); }
  function load(storage) {
    delete G.localStorage; delete G.settings; delete G.pendingSettings;
    if (storage) G.localStorage = storage;
    (0,eval)(source);
  }
  function initSettings() { (0,eval)(settingsSource); }
  function memoryStorage(initial) {
    var values = Object.assign({},initial), calls = [];
    return {values:values,calls:calls,
      getItem:function (key) { calls.push(['get',key]); return Object.prototype.hasOwnProperty.call(values,key) ? values[key] : null; },
      setItem:function (key,value) { calls.push(['set',key,value]); values[key] = value; }};
  }
  function withoutStyle(value) {
    var copy = Object.assign({},value); delete copy.castingStyle; return JSON.stringify(copy);
  }
  Math.random = function () { throw Error('casting preferences must not consume gameplay randomness'); };
  try {
    load(); initSettings();
    check('missing storage initializes both active and staged style to Arcane',
      G.getSelectedCastingStyle() === 'arcane' && G.settings.castingStyle === 'arcane' && G.pendingSettings.castingStyle === 'arcane');
    check('registry exposes stable personal styles in default-first order',
      JSON.stringify(G.getCastingStyleOptions()) === '[{"id":"arcane","label":"Arcane (default)"},{"id":"finger_guns","label":"Finger Guns"}]');
    check('registered styles point to data-driven animation recipe ids',
      G.getCastingStyle('arcane').handAnimation === 'arcane' && G.getCastingStyle('finger_guns').handAnimation === 'finger_guns');
    check('registry descriptors cannot be changed through control consumers',
      Object.isFrozen(G.CASTING_STYLES) && Object.isFrozen(G.getCastingStyle('arcane')) && Object.isFrozen(G.getCastingStyle('finger_guns')));
    var options = G.getCastingStyleOptions(); options[0].label = 'Changed'; options.pop();
    check('options are independent copies rather than writable shared records',
      G.getCastingStyleOptions().length === 2 && G.getCastingStyleOptions()[0].label === 'Arcane (default)');
    var invalid = [undefined,null,0,false,{},[],'','Finger Guns','finger-guns','ARCANE','__proto__','constructor','toString','future_style'];
    check('invalid and inherited-property ids safely normalize to Arcane', invalid.every(function (id) { return G.normalizeCastingStyle(id) === 'arcane'; }));
    check('invalid descriptor lookups use the same documented fallback', invalid.every(function (id) { return G.getCastingStyle(id) === G.CASTING_STYLES.arcane; }));
    check('valid stable ids survive normalization', G.normalizeCastingStyle('finger_guns') === 'finger_guns' && G.normalizeCastingStyle('arcane') === 'arcane');
    var storage = memoryStorage({'unrelated.game.save':'leave this alone'}); load(storage); initSettings();
    G.pendingSettings.resolution = 0.5; G.pendingSettings.viewDist = 1700; G._settingsDirty = true;
    var activeQuality = withoutStyle(G.settings), stagedQuality = withoutStyle(G.pendingSettings), presets = JSON.stringify(G.QUALITY_PRESETS);
    check('selecting Finger Guns updates the personal active and staged fields',
      G.setSelectedCastingStyle('finger_guns') === 'finger_guns' && G.getSelectedCastingStyle() === 'finger_guns' && G.pendingSettings.castingStyle === 'finger_guns');
    check('style selection preserves active and pending quality values independently',
      withoutStyle(G.settings) === activeQuality && withoutStyle(G.pendingSettings) === stagedQuality && G._settingsDirty === true);
    check('style selection leaves every quality preset and selected preset unchanged',
      JSON.stringify(G.QUALITY_PRESETS) === presets && G.qualityPreset === 'medium' && G._pendingQualityPreset === 'medium');
    check('persistence uses only the dedicated versioned browser-local key',
      G.CASTING_STYLE_STORAGE_KEY === 'hardwareone.casting-style.v1' &&
      storage.values[G.CASTING_STYLE_STORAGE_KEY] === 'finger_guns' && storage.values['unrelated.game.save'] === 'leave this alone' &&
      storage.calls.every(function (call) { return call[1] === G.CASTING_STYLE_STORAGE_KEY; }));
    load(storage); initSettings();
    check('a fresh script load restores the saved personal style into both settings copies',
      G.getSelectedCastingStyle() === 'finger_guns' && G.settings.castingStyle === 'finger_guns' && G.pendingSettings.castingStyle === 'finger_guns');
    G.pendingSettings.castingStyle = 'arcane';
    check('an uncommitted staged style does not change active selection', G.getSelectedCastingStyle() === 'finger_guns');
    G.setSelectedCastingStyle('not-a-style');
    check('invalid selected ids save normalized Arcane rather than corrupt preference data',
      G.getSelectedCastingStyle() === 'arcane' && storage.values[G.CASTING_STYLE_STORAGE_KEY] === 'arcane');
    load(memoryStorage({'hardwareone.casting-style.v1':'removed-style'})); initSettings();
    check('unknown saved styles fall back to Arcane during initialization', G.settings.castingStyle === 'arcane' && G.pendingSettings.castingStyle === 'arcane');
    load(memoryStorage({'hardwareone.casting-style.v1':'{"id":"finger_guns"}'}));
    check('malformed or wrong-format persisted values are not treated as executable data', G.getSelectedCastingStyle() === 'arcane');
    load({getItem:function () { throw Error('blocked read'); },setItem:function () { throw Error('blocked write'); }}); initSettings();
    check('blocked storage reads fall back without preventing settings initialization', G.settings.castingStyle === 'arcane');
    G.setSelectedCastingStyle('finger_guns');
    check('blocked writes still retain a usable in-session selection',
      G.getSelectedCastingStyle() === 'finger_guns' && G.pendingSettings.castingStyle === 'finger_guns');
    delete G.localStorage; delete G.settings; delete G.pendingSettings;
    Object.defineProperty(G,'localStorage',{configurable:true,get:function () { throw Error('storage property denied'); }});
    (0,eval)(source); initSettings();
    check('a denied localStorage property is guarded as well as its methods', G.setSelectedCastingStyle('finger_guns') === 'finger_guns');
    load(); G.setSelectedCastingStyle('finger_guns'); initSettings();
    check('preference API also works before configuration initializes', G.settings.castingStyle === 'finger_guns');
    G.coins = 0; G.playerLevel = 1; G.spells = {missile:{unlocked:false}}; G.equipment = {robes:null,relic:null};
    G.projectiles = [{x:5,y:6,speed:360}]; G.pendingMissileCasts = [{releaseAt:120}]; G.mana = 17; G.lastShotMs = 1000;
    var gameplay = JSON.stringify([G.coins,G.playerLevel,G.spells,G.equipment,G.projectiles,G.pendingMissileCasts,G.mana,G.lastShotMs]);
    G.setSelectedCastingStyle('arcane'); G.setSelectedCastingStyle('finger_guns');
    check('both styles remain available without coins equipment or spell unlocks', G.getCastingStyleOptions().length === 2 && G.getSelectedCastingStyle() === 'finger_guns');
    check('style selection never changes combat reservations projectiles equipment or resources',
      JSON.stringify([G.coins,G.playerLevel,G.spells,G.equipment,G.projectiles,G.pendingMissileCasts,G.mana,G.lastShotMs]) === gameplay);
    var otherBrowser = memoryStorage(); load(otherBrowser); initSettings();
    check('an unrelated browser storage starts at the default rather than inheriting a game save', G.getSelectedCastingStyle() === 'arcane');
    var events = [], unsubscribe = G.onCastingStyleChange(function (selected,previous) {
      events.push([selected,previous,G.settings.castingStyle,G.pendingSettings.castingStyle,otherBrowser.values[G.CASTING_STYLE_STORAGE_KEY]]);
    });
    G.setSelectedCastingStyle('finger_guns');
    check('change listeners receive both ids after settings and persistence are updated',
      JSON.stringify(events) === '[["finger_guns","arcane","finger_guns","finger_guns","finger_guns"]]');
    G.setSelectedCastingStyle('finger_guns'); G.setSelectedCastingStyle('unknown'); G.setSelectedCastingStyle(null);
    check('unchanged and equivalent invalid selections do not rebroadcast a style change', events.length === 2 && events[1][0] === 'arcane' && events[1][1] === 'finger_guns');
    unsubscribe(); unsubscribe(); G.setSelectedCastingStyle('finger_guns');
    check('listener unsubscribe is safe to repeat and stops subsequent events', events.length === 2);
    var receivedAfterFailure = 0, stopBad = G.onCastingStyleChange(function () { throw Error('failed preview'); });
    var stopGood = G.onCastingStyleChange(function () { receivedAfterFailure++; });
    G.setSelectedCastingStyle('arcane');
    check('one failed listener cannot interrupt other controls or saving',
      receivedAfterFailure === 1 && G.getSelectedCastingStyle() === 'arcane' && otherBrowser.values[G.CASTING_STYLE_STORAGE_KEY] === 'arcane');
    stopBad(); stopGood();
    var firstCalls = 0, laterCalls = 0, stopLater, stopFirst = G.onCastingStyleChange(function () {
      firstCalls++; stopFirst(); stopLater = G.onCastingStyleChange(function () { laterCalls++; });
    });
    G.setSelectedCastingStyle('finger_guns');
    check('listeners added during notification wait until the next committed change', firstCalls === 1 && laterCalls === 0);
    G.setSelectedCastingStyle('arcane'); stopLater();
    check('self-unsubscribe during notification preserves later independent subscribers', firstCalls === 1 && laterCalls === 1);
    var committedEvents = [], stopCommitted = G.onCastingStyleChange(function (selected,previous) { committedEvents.push([selected,previous]); });
    var actualSettings = G.settings; G.settings = Object.assign({},actualSettings,{castingStyle:'finger_guns'});
    G.getSelectedCastingStyle(); G.settings = actualSettings;
    check('reading a temporary preview settings copy does not publish or save a preference',
      committedEvents.length === 0 && G.getSelectedCastingStyle() === 'arcane' && otherBrowser.values[G.CASTING_STYLE_STORAGE_KEY] === 'arcane');
    G.settings.castingStyle = 'finger_guns'; G.setSelectedCastingStyle('finger_guns'); stopCommitted();
    check('a committed style change is still notified after an applying UI sets its active field',
      JSON.stringify(committedEvents) === '[["finger_guns","arcane"]]');
    var manifest = JSON.parse(slurp('assets/games/manifest.json')).parts.MAIN;
    var styleAt = manifest.indexOf('src/01-casting-styles.js'), configAt = manifest.indexOf('src/01-config-state.js');
    check('registry is assembled once before configuration without changing script scope',
      styleAt >= 0 && styleAt < configAt && manifest.filter(function (name) { return name === 'src/01-casting-styles.js'; }).length === 1);
  } finally { Math.random = oldRandom; }
  __out('CASTING_STYLES_RESULT PASS '+checks);
}());
undefined;
