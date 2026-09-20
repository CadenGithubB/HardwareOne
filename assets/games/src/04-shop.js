// =============================================
// SHOP SYSTEM
// =============================================
var SHOP_ITEMS_BASE = [
  {id:'heal',  name:'Health Potion', desc:'+40 HP',             cost:15, type:'consumable'},
  {id:'mana',  name:'Mana Crystal',  desc:'+60 Mana',           cost:12, type:'consumable'},
  {id:'speed', name:'Wind Boots',    desc:'+25% Speed (60s)',   cost:25, type:'consumable'},
  {id:'ward',  name:'Warding Stone', desc:'Absorb next hit',    cost:35, type:'consumable'},
  {id:'dmg',   name:'Power Rune',    desc:'+30% Damage (60s)',  cost:30, type:'consumable'},
  // Spell tomes — unlock new spells
  {id:'spell_fire',      name:'Flame Jet Tome',    desc:'Unlock Flame Jet',      cost:20, type:'spell', spellId:'fire'},
  {id:'spell_ice',       name:'Frost Bolt Tome',   desc:'Unlock Frost Bolt',     cost:20, type:'spell', spellId:'ice'},
  {id:'spell_lightning', name:'Lightning Tome',     desc:'Unlock Chain Lightning',cost:25, type:'spell', spellId:'lightning'},
  {id:'spell_poison',    name:'Poison Tome',        desc:'Unlock Poison Cloud',   cost:25, type:'spell', spellId:'poison'},
  {id:'spell_arcane',    name:'Arcane Tome',        desc:'Unlock Arcane Blast',   cost:30, type:'spell', spellId:'arcane'},
  // Upgrade tomes — enhance owned spells
  {id:'upg_missile',  name:'Split Shot',     desc:'3-way homing missiles',   cost:40, type:'upgrade', spellId:'missile'},
  {id:'upg_fire',     name:'Inferno',        desc:'Longer range, wider beam', cost:40, type:'upgrade', spellId:'fire'},
  {id:'upg_ice',      name:'Frost Nova',     desc:'AoE slow on hit',         cost:40, type:'upgrade', spellId:'ice'},
  {id:'upg_lightning',name:'Ball Lightning',  desc:'+1 chain, re-chains',    cost:40, type:'upgrade', spellId:'lightning'},
  {id:'upg_poison',   name:'Plague',          desc:'2-lob spread, bigger cloud, +coins on cloud kills',cost:40, type:'upgrade', spellId:'poison'},
  {id:'upg_arcane',   name:'Shockwave',       desc:'Bigger radius, longer stun',cost:40, type:'upgrade', spellId:'arcane'},
  // Equipment
  {id:'leather_armor',    name:'Leather Armor',     desc:'-10% damage taken',   cost:40,  type:'equipment', equipId:'leather_armor'},
  {id:'chain_mail',       name:'Chain Mail',        desc:'-18% damage taken',   cost:80,  type:'equipment', equipId:'chain_mail'},
  {id:'plate_armor',      name:'Plate Armor',       desc:'-25% damage taken',   cost:140, type:'equipment', equipId:'plate_armor'},
  {id:'cloth_hood',       name:'Cloth Hood',        desc:'+3 mana/s regen',     cost:35,  type:'equipment', equipId:'cloth_hood'},
  {id:'iron_helm',        name:'Iron Helm',         desc:'+2 HP/s regen',       cost:70,  type:'equipment', equipId:'iron_helm'},
  {id:'wind_crown',       name:'Wind Crown',        desc:'+15% move speed',     cost:120, type:'equipment', equipId:'wind_crown'},
  {id:'apprentice_robes', name:'Apprentice Robes',  desc:'+10% spell damage',   cost:45,  type:'equipment', equipId:'apprentice_robes'},
  {id:'mage_robes',       name:'Mage Robes',        desc:'+20% spell damage',   cost:90,  type:'equipment', equipId:'mage_robes'},
  {id:'shadow_robes',     name:'Shadow Robes',      desc:'-20% mana cost',      cost:130, type:'equipment', equipId:'shadow_robes'},
  {id:'leather_boots',    name:'Leather Boots',     desc:'Dash (Shift)',        cost:50,  type:'equipment', equipId:'leather_boots'},
  {id:'winged_boots',     name:'Winged Boots',      desc:'Jump (Space)',        cost:100, type:'equipment', equipId:'winged_boots'},
  {id:'arcane_striders',  name:'Arcane Striders',   desc:'Dash + Jump',         cost:160, type:'equipment', equipId:'arcane_striders'},
  // Companions
  {id:'companion_slime', name:'Slime Companion', desc:'A bouncy friend that attacks enemies', cost:40, type:'companion', companionId:'slime'}
];


function getVisibleShopItems() {
  var result = [];
  for (var i = 0; i < SHOP_ITEMS_BASE.length; i++) {
    var item = SHOP_ITEMS_BASE[i];
    if (item.type === 'spell') {
      if (!spells[item.spellId].unlocked) result.push(item);
    } else if (item.type === 'upgrade') {
      if (spells[item.spellId].unlocked && spells[item.spellId].tier < 2) result.push(item);
    } else if (item.type === 'equipment') {
      var def = EQUIPMENT_DEFS[item.equipId];
      if (def && (!equipment[def.slot] || equipment[def.slot].id !== def.id)) result.push(item);
    } else if (item.type === 'companion') {
      var owned = false;
      for (var ci = 0; ci < companions.length; ci++) {
        if (companions[ci].type === item.companionId) { owned = true; break; }
      }
      if (!owned) result.push(item);
    } else {
      result.push(item);
    }
  }
  return result;
}
var SHOP_ITEMS = SHOP_ITEMS_BASE; // legacy compat — drawShopOverlay uses dynamic list

function spawnShop() {
  shopMarker = null; shopOpen = false; shopNearby = false;
  if (Math.random() > 0.6) return;
  var candidates = [];
  if (grid && gridW > 0 && gridH > 0) {
    var dirs = [[-1,0],[1,0],[0,-1],[0,1]];
    for (var gy = 0; gy < gridH; gy++) {
      for (var gx = 0; gx < gridW; gx++) {
        if (grid[gy * gridW + gx] !== 0) continue;
        var adj = false;
        for (var d = 0; d < 4; d++) {
          var nx = gx + dirs[d][0], ny = gy + dirs[d][1];
          if (nx < 0 || ny < 0 || nx >= gridW || ny >= gridH || grid[ny * gridW + nx] !== 0) { adj = true; break; }
        }
        if (adj) {
          var cx = gx * cell + cell * 0.5;
          var cy = gy * cell + cell * 0.5;
          if (cx > cell && cy > cell && cx < worldW - cell && cy < worldH - cell) candidates.push({x: cx, y: cy});
        }
      }
    }
  }
  if (candidates.length === 0) { shopMarker = {x: worldW * 0.5, y: worldH * 0.5}; return; }
  var pick = candidates[Math.floor(Math.random() * candidates.length)];
  shopMarker = {x: pick.x, y: pick.y};
}

function applyShopItem(item) {
  if (coins < item.cost) return false;
  coins -= item.cost;
  if (item.type === 'spell') {
    spells[item.spellId].unlocked = true;
    console.log('[SHOP] Unlocked spell: ' + spells[item.spellId].name);
  } else if (item.type === 'upgrade') {
    var sp = spells[item.spellId];
    sp.tier = 2;
    if (item.spellId === 'fire') {
      sp.streamRange = 180;
      sp.streamWidth = 0.55;
      sp.damage = sp.damage * 1.3;
    } else if (item.spellId === 'arcane') {
      sp.novaRadius = 160;
      sp.stunDuration = 1600;
    } else if (item.spellId === 'poison') {
      sp.cloudRadius = 75;
      sp.cloudDuration = 5000;
    } else if (item.spellId === 'lightning') {
      sp.speed = 300;
    }
    console.log('[SHOP] Upgraded: ' + sp.name + ' → Tier 2');
  } else if (item.type === 'equipment') {
    var eqDef = EQUIPMENT_DEFS[item.equipId];
    if (eqDef) {
      equipment[eqDef.slot] = createEquipInstance(item.equipId, 1.0);
      console.log('[SHOP] Equipped: ' + eqDef.name + ' (' + eqDef.slot + ') Q=1.0');
    }
  } else if (item.type === 'companion') {
    var spawnAng = cam.ang + (Math.random() - 0.5) * 1.0;
    companions.push({
      type: item.companionId,
      x: pos.x + Math.cos(spawnAng) * 45,
      y: pos.y + Math.sin(spawnAng) * 45,
      z: 0, phase: 0, squash: 1.0, lastAttackMs: 0,
      wanderAng: Math.random() * Math.PI * 2
    });
    console.log('[SHOP] Purchased companion: ' + item.name);
  } else {
    if (item.id === 'heal')  { health = Math.min(HEALTH_MAX, health + 40); }
    else if (item.id === 'mana')  { mana = Math.min(MANA_MAX, mana + 60); }
    else if (item.id === 'speed') { speedBoostUntil = Date.now() + 60000; }
    else if (item.id === 'ward')  { wardActive = true; }
    else if (item.id === 'dmg')   { dmgBoostUntil = Date.now() + 60000; }
  }
  return true;
}

