// Personal presentation preference, not equipment, progression or a quality
// preset. Add descriptors here and their data-only animation recipes to the
// hand renderer. A style never changes combat timing or projectile mechanics.
var CASTING_STYLES = Object.freeze({
  arcane:Object.freeze({id:'arcane',label:'Arcane (default)',handAnimation:'arcane'}),
  finger_guns:Object.freeze({id:'finger_guns',label:'Finger Guns',handAnimation:'finger_guns'})
});
var CASTING_STYLE_STORAGE_KEY = 'hardwareone.casting-style.v1';

function normalizeCastingStyle(id) {
  return typeof id === 'string' && Object.prototype.hasOwnProperty.call(CASTING_STYLES,id) ? id : 'arcane';
}
function getCastingStyle(id) {
  return CASTING_STYLES[normalizeCastingStyle(id)];
}
function getCastingStyleOptions() {
  // Callers can sort/build controls without changing the shared registry.
  return Object.keys(CASTING_STYLES).map(function(id) {
    return {id:id,label:CASTING_STYLES[id].label};
  });
}
function readCastingStylePreference() {
  try {
    if (typeof localStorage !== 'undefined') return normalizeCastingStyle(localStorage.getItem(CASTING_STYLE_STORAGE_KEY));
  } catch (error) { /* Private/blocked storage leaves the session usable. */ }
  return 'arcane';
}
var _selectedCastingStyle = readCastingStylePreference();
var _castingStyleListeners = [];

function onCastingStyleChange(listener) {
  if (typeof listener !== 'function') throw TypeError('Casting style listener must be a function');
  var subscription = {listener:listener,active:true};
  _castingStyleListeners.push(subscription);
  return function () {
    subscription.active = false;
    var index = _castingStyleListeners.indexOf(subscription);
    if (index >= 0) _castingStyleListeners.splice(index,1);
  };
}

function getSelectedCastingStyle() {
  return normalizeCastingStyle(typeof settings !== 'undefined' && settings ? settings.castingStyle : _selectedCastingStyle);
}
function setSelectedCastingStyle(id) {
  var selected = normalizeCastingStyle(id);
  // Compare the committed preference, not a temporary renderer's settings copy.
  var previous = _selectedCastingStyle;
  _selectedCastingStyle = selected;
  // Only this personal field is changed. In particular, do not apply a quality
  // preset, rebuild the world, alter equipment, or gate it behind an unlock.
  if (typeof settings !== 'undefined' && settings) settings.castingStyle = selected;
  if (typeof pendingSettings !== 'undefined' && pendingSettings) pendingSettings.castingStyle = selected;
  try {
    if (typeof localStorage !== 'undefined') localStorage.setItem(CASTING_STYLE_STORAGE_KEY,selected);
  } catch (error) { /* The selection still works for this browser session. */ }
  if (selected !== previous) {
    _castingStyleListeners.slice().forEach(function (subscription) {
      if (!subscription.active) return;
      try { subscription.listener(selected,previous); }
      catch (error) { /* A failed preview listener must not break other controls. */ }
    });
  }
  return selected;
}
