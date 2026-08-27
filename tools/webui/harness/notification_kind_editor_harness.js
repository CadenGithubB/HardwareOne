/* Behavioral harness for the exact notification-editor JS extracted from
 * WebPage_Dashboard.h.  Requires _bootstrap.js from tools/webui/js_engine.py.
 * __argv[0] = extracted kDashboardNotificationKindEditorJs body
 * __argv[1] = production CMD_INPUT_MAX
 */
var EDITOR_JS_PATH = __argv[0];
var COMMAND_INPUT_MAX = parseInt(__argv[1], 10);
var passes = 0, failures = 0;

function check(name, condition, detail) {
    if (condition) { passes++; __out('PASS ' + name); }
    else { failures++; __out('FAIL ' + name + (detail ? '   [' + detail + ']' : '')); }
}

function P(value, failed) {
    return {
        _value: value,
        _failed: !!failed,
        then: function (fn) {
            if (this._failed) return this;
            try { return P(fn ? fn(this._value) : this._value, false); }
            catch (error) { return P(error, true); }
        },
        catch: function (fn) {
            if (!this._failed) return this;
            try { return P(fn ? fn(this._value) : undefined, false); }
            catch (error) { return P(error, true); }
        }
    };
}

var Promise = {
    all: function (items) {
        var values = [];
        for (var i = 0; i < items.length; i++) {
            if (items[i] && items[i]._failed) return P(items[i]._value, true);
            values.push(items[i] && own(items[i], '_value') ? items[i]._value : items[i]);
        }
        return P(values, false);
    }
};

function own(obj, key) { return Object.prototype.hasOwnProperty.call(obj, key); }
function ClassList() { this.values = {}; }
ClassList.prototype.add = function (name) { this.values[name] = true; };
ClassList.prototype.remove = function (name) { delete this.values[name]; };
ClassList.prototype.contains = function (name) { return !!this.values[name]; };

function El(id) {
    this.id = id;
    this.children = [];
    this.classList = new ClassList();
    this.className = '';
    this.style = {};
    this.disabled = false;
    this.textContent = '';
}
El.prototype.appendChild = function (child) { this.children.push(child); return child; };
Object.defineProperty(El.prototype, 'innerHTML', {
    get: function () { return this._html || ''; },
    set: function (value) { this._html = value; this.children = []; }
});

var elements = {
    'dash-notif-modal': new El('dash-notif-modal'),
    'dash-notif-list': new El('dash-notif-list'),
    'dash-notif-save': new El('dash-notif-save')
};
var document = {
    createElement: function (tag) { return new El('_' + tag); }
};
var window = { Dash: {} };
var alerts = [];
function alert(message) { alerts.push(String(message)); }
var hw = {
    $: function (id) { return elements[id] || null; },
    getEventKindFamilies: function () { return P([], false); },
    fetchJSON: function () { return P({success: true, settings: {notificationMuted: []}}, false); },
    postJSON: function () { return P({ok: false, error: 'not configured'}, false); }
};

var installer = null;
try {
    installer = eval(slurp(EDITOR_JS_PATH) + '\ninstallDashboardNotificationKindEditor;');
} catch (error) {
    __out('FAIL extracted source parses   [' + error + ']');
    __out('HARNESS_RESULT FAIL 1');
}

if (installer) {
    check('production command limit loaded', COMMAND_INPUT_MAX > 0,
          String(COMMAND_INPUT_MAX));

    var hooks = {};
    installer(COMMAND_INPUT_MAX, hooks);

    check('batch success requires the exact committed marker',
          hooks.batchFailure(['notifyusermute set a on'], {
              ok: true, count: 1,
              results: ['[Settings] Configuration updated']
          }) === '' &&
          hooks.batchFailure(['notifyusermute set a on'], {
              ok: true, count: 1,
              results: ['prefix [Settings] Configuration updated suffix']
          }) !== '' &&
          hooks.batchFailure(['notifyusermute set a on'], {
              ok: true, count: 1,
              results: ['[settings] configuration updated']
          }) !== '');

    hooks.setState([{n: 'Snapshot', k: ['a']}], {a: true}, {a: true});
    var badSnapshotRejected = false;
    try {
        hooks.applySettingsSnapshot({success: false, error: 'read_failed'});
    } catch (_) {
        badSnapshotRejected = true;
    }
    var afterBadSnapshot = hooks.getState();
    check('failed settings snapshot cannot clear authoritative client state',
          badSnapshotRejected && afterBadSnapshot.muted.a === true &&
          afterBadSnapshot.initialMuted.a === true);

    var noOpNone = hooks.buildMutationCommands(['a', 'b'], {}, {});
    check('unchanged terminal none is a no-op', noOpNone.length === 0,
          JSON.stringify(noOpNone));

    var manualAll = hooks.buildMutationCommands(['a', 'b'], {}, {a: true, b: true});
    check('manually reaching all uses a preserving patch',
          JSON.stringify(manualAll) === JSON.stringify(['notifyusermute patch +a,+b']),
          JSON.stringify(manualAll));

    var manualNone = hooks.buildMutationCommands(
        ['a', 'b'], {a: true, b: true}, {});
    check('manually reaching none uses a preserving patch',
          JSON.stringify(manualNone) === JSON.stringify(['notifyusermute patch -a,-b']),
          JSON.stringify(manualNone));

    var explicitNone = hooks.buildMutationCommands(['a', 'b'], {}, {}, 'none');
    var explicitAll = hooks.buildMutationCommands(['a', 'b'], {}, {}, 'all');
    check('explicit global none uses bulk command',
          JSON.stringify(explicitNone) === JSON.stringify(['notifyusermute none']),
          JSON.stringify(explicitNone));
    check('explicit global all uses bulk command',
          JSON.stringify(explicitAll) === JSON.stringify(['notifyusermute all']),
          JSON.stringify(explicitAll));

    var arbitrary = hooks.buildMutationCommands(
        ['a', 'b', 'c', 'd'], {a: true, c: true}, {b: true, c: true, d: true});
    check('arbitrary state is a known-kind delta',
          JSON.stringify(arbitrary) === JSON.stringify(['notifyusermute patch -a,+b,+d']),
          JSON.stringify(arbitrary));

    var unchanged = hooks.buildMutationCommands(
        ['a', 'b'], {a: true}, {a: true});
    check('unchanged mixed state emits no command', unchanged.length === 0,
          JSON.stringify(unchanged));

    var smallHooks = {};
    installer(37, smallHooks);
    var tightlyPacked = smallHooks.buildMutationCommands(
        ['kind000', 'kind001', 'kind002'], {}, {kind000: true, kind002: true});
    check('greedy planner splits only when next token exceeds limit',
          tightlyPacked.length === 2 && tightlyPacked[0].length <= 37 &&
          tightlyPacked[1].length <= 37,
          JSON.stringify(tightlyPacked));

    // Synthetic catalog growth: alternate the before/after sets so neither
    // bulk command applies, then replay every emitted delta.
    var many = [], before = {}, after = {};
    for (var n = 0; n < 1200; n++) {
        var name = 'synthetic_kind_' + ('0000' + n).slice(-4) + '_xxxxxxxxxxxx';
        many.push(name);
        if (n % 3 === 0) before[name] = true;
        if (n % 4 === 0) after[name] = true;
    }
    installer(COMMAND_INPUT_MAX, hooks);
    var grown = hooks.buildMutationCommands(many, before, after);
    var allBounded = grown.length > 1;
    var greedy = true;
    var prefix = 'notifyusermute patch ';
    var replay = {};
    for (var bk in before) if (own(before, bk)) replay[bk] = true;
    for (var g = 0; g < grown.length; g++) {
        if (grown[g].length > COMMAND_INPUT_MAX || grown[g].indexOf(prefix) !== 0) allBounded = false;
        var tokens = grown[g].substring(prefix.length).split(',');
        for (var t = 0; t < tokens.length; t++) {
            var kind = tokens[t].substring(1);
            if (tokens[t].charAt(0) === '+') replay[kind] = true;
            else delete replay[kind];
        }
        if (g + 1 < grown.length) {
            var nextToken = grown[g + 1].substring(prefix.length).split(',')[0];
            if (grown[g].length + 1 + nextToken.length <= COMMAND_INPUT_MAX) greedy = false;
        }
    }
    var replayMatches = true;
    for (var m = 0; m < many.length; m++) {
        if (!!replay[many[m]] !== !!after[many[m]]) { replayMatches = false; break; }
    }
    check('synthetic catalog growth stays within production limit', allBounded,
          'commands=' + grown.length);
    check('synthetic catalog growth is greedily packed', greedy,
          'commands=' + grown.length);
    check('synthetic packed deltas reproduce final state', replayMatches);

    // Exercise the public bulk action, not just the planner parameter: it is
    // the only UI operation allowed to set replacement intent.
    var bulkPosts = [];
    hw.postJSON = function (url, body) {
        bulkPosts.push(body.commands.slice());
        return P({
            ok: true,
            count: body.commands.length,
            results: body.commands.map(function () { return '[Settings] Configuration updated'; })
        }, false);
    };
    hooks.setState([{n: 'Bulk', k: ['a', 'b']}], {a: true}, {a: true});
    window.Dash.notifMuteAll(false);
    window.Dash.saveNotifPrefs();
    hooks.setState([{n: 'Bulk', k: ['a', 'b']}], {}, {});
    window.Dash.notifMuteAll(true);
    window.Dash.saveNotifPrefs();
    check('global bulk actions submit exact none and all commands',
          JSON.stringify(bulkPosts) === JSON.stringify([
              ['notifyusermute none'], ['notifyusermute all']
          ]), JSON.stringify(bulkPosts));

    hooks.setState([{n: 'Bulk', k: ['a', 'b']}], {}, {});
    window.Dash.notifMuteAll(true);
    var familyHeader = elements['dash-notif-list'].children[0].children[0];
    familyHeader.children[2].onclick({stopPropagation: function () {}});
    check('a later family edit clears global bulk intent',
          hooks.getState().bulkIntent === null);

    hooks.setState([{n: 'Per kind', k: ['a', 'b']}], {}, {});
    window.Dash.notifMuteAll(true);
    var kindHeader = elements['dash-notif-list'].children[0].children[0];
    kindHeader.onclick();
    var firstKindButton = elements['dash-notif-list'].children[0].children[1].children[1];
    firstKindButton.onclick();
    check('a later per-kind edit clears global bulk intent',
          hooks.getState().bulkIntent === null);

    // Drive the shipping save/recovery path with top-level ok=true but one
    // failed result after an earlier command succeeded.  The later toString
    // tripwire proves validation does not stop after finding the first error.
    var behaviorHooks = {};
    installer(COMMAND_INPUT_MAX, behaviorHooks);
    var families = [{n: 'Synthetic', k: many.slice(0, 180)}];
    var initialKnown = many[1];
    var reloadedKnown = many[3];
    var settingsFetches = 0;
    var reloadSawSaveDisabled = false;
    var posted = null;
    var lateResultInspected = false;
    var eventOrder = [];
    hw.getEventKindFamilies = function () { return P(families, false); };
    hw.fetchJSON = function (url) {
        settingsFetches++;
        if (settingsFetches === 1) {
            return P({success: true, settings: {notificationMuted: [initialKnown, 'future_unknown_kind']}}, false);
        }
        reloadSawSaveDisabled = elements['dash-notif-save'].disabled;
        eventOrder.push('reload');
        return P({success: true, settings: {notificationMuted: [reloadedKnown]}}, false);
    };
    hw.postJSON = function (url, body) {
        posted = {url: url, body: body};
        eventOrder.push('post');
        var results = [];
        for (var i = 0; i < body.commands.length; i++) {
            results.push('[Settings] Configuration updated');
        }
        results[0] = 'Error: injected first-command failure';
        results[results.length - 1] = {
            toString: function () {
                lateResultInspected = true;
                return '[Settings] Configuration updated';
            }
        };
        return P({ok: true, count: body.commands.length, results: results}, false);
    };
    alerts = [];
    window.Dash._openNotifEditorImpl();
    var opened = behaviorHooks.getState();
    check('open snapshots only known muted kinds',
          opened.initialMuted[initialKnown] === true &&
          !own(opened.initialMuted, 'future_unknown_kind'));

    var desired = {};
    for (var di = 0; di < families[0].k.length; di += 2) desired[families[0].k[di]] = true;
    behaviorHooks.setCurrentMuted(desired);
    window.Dash.saveNotifPrefs();

    var recovered = behaviorHooks.getState();
    var everyPostedCommandBounded = !!posted && posted.body.commands.length > 1;
    if (posted) {
        for (var pc = 0; pc < posted.body.commands.length; pc++) {
            if (posted.body.commands[pc].length > COMMAND_INPUT_MAX) everyPostedCommandBounded = false;
        }
    }
    check('save uses cli batch with bounded packed commands',
          posted && posted.url === '/api/cli/batch' && everyPostedCommandBounded,
          posted ? JSON.stringify(posted.body.commands.map(function (c) { return c.length; })) : 'not posted');
    check('per-result failure is detected despite top-level ok',
          alerts.length === 1 && alerts[0].indexOf('injected first-command failure') >= 0,
          alerts.join(' | '));
    check('every result item is inspected', lateResultInspected);
    check('partial failure reloads before editor is re-enabled',
          settingsFetches === 2 && reloadSawSaveDisabled &&
          eventOrder.join(',') === 'post,reload' && !elements['dash-notif-save'].disabled,
          eventOrder.join(',') + ' fetches=' + settingsFetches);
    check('reload replaces partial client state and snapshot',
          recovered.muted[reloadedKnown] === true &&
          recovered.initialMuted[reloadedKnown] === true &&
          !recovered.muted[families[0].k[0]] &&
          recovered.saving === false && recovered.needsReload === false);

    __out(failures ? 'HARNESS_RESULT FAIL ' + failures : 'HARNESS_RESULT PASS');
}
