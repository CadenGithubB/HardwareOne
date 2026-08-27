/* Behavioral harness for the exact HW_EVENT_KINDS JavaScript extracted from
 * WebServer_Utils.cpp. Requires _bootstrap.js from tools/webui/js_engine.py.
 *
 * __argv[0] = extracted production helper body, with <script> tags removed
 *
 * The real helper is asynchronous. A deterministic Promise implementation is
 * used here so the harness can settle requests in either order and prove the
 * load-generation fence, without depending on one engine's microtask timing.
 */
var SOURCE_PATH = __argv[0];
var SOURCE = slurp(SOURCE_PATH);
var passes = 0, failures = 0;

function check(name, condition, detail) {
    if (condition) {
        passes++;
        __out('PASS ' + name);
    } else {
        failures++;
        __out('FAIL ' + name + (detail ? '   [' + detail + ']' : ''));
    }
}

function own(object, key) {
    return Object.prototype.hasOwnProperty.call(object, key);
}

/* A small Promises/A+ subset: pending promises, chaining, thrown-error
 * propagation and thenable adoption are all needed by the production helper.
 * Tasks run only when flushTasks() is called, making completion order explicit.
 */
var tasks = [];

function enqueue(task) {
    tasks.push(task);
}

function flushTasks() {
    var turns = 0;
    while (tasks.length) {
        var task = tasks.shift();
        task();
        turns++;
        if (turns > 10000) throw new Error('promise task loop did not quiesce');
    }
}

function FakePromise(executor) {
    this._state = 0;  // 0 pending, 1 fulfilled, 2 rejected
    this._value = undefined;
    this._handlers = [];
    if (typeof executor === 'function') {
        var self = this;
        var called = false;
        try {
            executor(function(value) {
                if (called) return;
                called = true;
                settle(self, 1, value);
            }, function(error) {
                if (called) return;
                called = true;
                settle(self, 2, error);
            });
        } catch (error) {
            if (!called) {
                called = true;
                settle(self, 2, error);
            }
        }
    }
}

function settle(promise, state, value) {
    if (promise._state !== 0) return;
    if (state === 1 && value === promise) {
        settle(promise, 2, new TypeError('promise resolved with itself'));
        return;
    }
    if (state === 1 && value && typeof value.then === 'function') {
        var adopted = false;
        try {
            value.then(function(next) {
                if (adopted) return;
                adopted = true;
                settle(promise, 1, next);
            }, function(error) {
                if (adopted) return;
                adopted = true;
                settle(promise, 2, error);
            });
        } catch (error) {
            if (!adopted) {
                adopted = true;
                settle(promise, 2, error);
            }
        }
        return;
    }
    promise._state = state;
    promise._value = value;
    for (var i = 0; i < promise._handlers.length; i++) {
        scheduleHandler(promise, promise._handlers[i]);
    }
    promise._handlers = [];
}

function scheduleHandler(parent, handler) {
    enqueue(function() {
        var callback = parent._state === 1 ? handler.fulfilled : handler.rejected;
        if (typeof callback !== 'function') {
            settle(handler.child, parent._state, parent._value);
            return;
        }
        try {
            settle(handler.child, 1, callback(parent._value));
        } catch (error) {
            settle(handler.child, 2, error);
        }
    });
}

FakePromise.prototype.then = function(fulfilled, rejected) {
    var child = new FakePromise();
    var handler = {fulfilled: fulfilled, rejected: rejected, child: child};
    if (this._state === 0) this._handlers.push(handler);
    else scheduleHandler(this, handler);
    return child;
};

FakePromise.prototype.catch = function(rejected) {
    return this.then(null, rejected);
};

FakePromise.resolve = function(value) {
    if (value instanceof FakePromise) return value;
    var promise = new FakePromise();
    settle(promise, 1, value);
    return promise;
};

FakePromise.reject = function(error) {
    var promise = new FakePromise();
    settle(promise, 2, error);
    return promise;
};

function deferred() {
    var resolve, reject;
    var promise = new FakePromise(function(ok, fail) {
        resolve = ok;
        reject = fail;
    });
    return {promise: promise, resolve: resolve, reject: reject};
}

function observe(promise) {
    var result = {settled: false, fulfilled: false, value: undefined, error: null};
    promise.then(function(value) {
        result.settled = true;
        result.fulfilled = true;
        result.value = value;
    }, function(error) {
        result.settled = true;
        result.error = error;
    });
    return result;
}

function FakeEvent(type, options) {
    this.type = type;
    this.bubbles = !!(options && options.bubbles);
}

function optionsBelow(node, output) {
    output = output || [];
    if (node.tagName === 'OPTION') output.push(node);
    for (var i = 0; i < node.children.length; i++) {
        optionsBelow(node.children[i], output);
    }
    return output;
}

function Element(tag, id) {
    this.tagName = String(tag || 'div').toUpperCase();
    this.id = id || '';
    this.children = [];
    this.parentNode = null;
    this.disabled = false;
    this.selected = false;
    this.textContent = '';
    this.label = '';
    this.title = '';
    this.events = [];
}

Element.prototype.appendChild = function(child) {
    child.parentNode = this;
    this.children.push(child);
    return child;
};

Element.prototype.removeChild = function(child) {
    var index = this.children.indexOf(child);
    if (index < 0) throw new Error('removeChild: child not found');
    this.children.splice(index, 1);
    child.parentNode = null;
    return child;
};

Element.prototype.dispatchEvent = function(event) {
    this.events.push({type: event.type, value: this.value});
    return true;
};

Object.defineProperty(Element.prototype, 'firstChild', {
    get: function() { return this.children.length ? this.children[0] : null; }
});

Object.defineProperty(Element.prototype, 'value', {
    get: function() {
        if (this.tagName === 'OPTION') return own(this, '_value') ? this._value : '';
        if (this.tagName !== 'SELECT') return own(this, '_value') ? this._value : '';
        var options = optionsBelow(this, []);
        for (var i = 0; i < options.length; i++) {
            if (options[i].selected) return options[i].value;
        }
        for (var j = 0; j < options.length; j++) {
            if (!options[j].disabled) return options[j].value;
        }
        return options.length ? options[0].value : '';
    },
    set: function(value) {
        value = String(value);
        if (this.tagName !== 'SELECT') {
            this._value = value;
            return;
        }
        var options = optionsBelow(this, []);
        for (var i = 0; i < options.length; i++) {
            options[i].selected = options[i].value === value;
        }
    }
});

function Document() {
    this.byId = {};
}

Document.prototype.createElement = function(tag) {
    return new Element(tag);
};

Document.prototype.getElementById = function(id) {
    return this.byId[id] || null;
};

Document.prototype.add = function(element) {
    if (element.id) this.byId[element.id] = element;
    return element;
};

function install(fetchJSON) {
    var document = new Document();
    var window = {hw: {}};
    window.hw.$ = function(target) {
        return typeof target === 'string' ? document.getElementById(target) : target;
    };
    window.hw.fetchJSON = fetchJSON;
    // Function parameters become the lexical globals captured by the shipping
    // closures. This avoids mutating the harness's real window/document.
    var factory = Function('window', 'document', 'Promise', 'Event', SOURCE);
    factory(window, document, FakePromise, FakeEvent);
    return {window: window, document: document, hw: window.hw};
}

function clone(value) {
    return JSON.parse(JSON.stringify(value));
}

function optionValues(select) {
    var options = optionsBelow(select, []);
    var values = [];
    for (var i = 0; i < options.length; i++) values.push(options[i].value);
    return values;
}

function groupLabels(select) {
    var labels = [];
    for (var i = 0; i < select.children.length; i++) {
        if (select.children[i].tagName === 'OPTGROUP') labels.push(select.children[i].label);
    }
    return labels;
}

var VALID = {
    families: [
        {n: 'Connectivity', k: ['wifi_connected', 'ble_peer_online']},
        {n: 'Updates', k: ['ota_recovery_entered']},
        {n: 'System', k: ['api_ready', 'tof_sample']}
    ]
};

var parseOk = false;
var exported = false;
try {
    var parseEnv = install(function() { return FakePromise.resolve(clone(VALID)); });
    parseOk = true;
    exported = typeof parseEnv.hw.getEventKindFamilies === 'function' &&
               typeof parseEnv.hw.fillEventKindSelect === 'function' &&
               typeof parseEnv.hw.eventKindDisplayName === 'function';
} catch (error) {
    check('production helper parses', false, String(error));
}
if (parseOk) check('production helper parses', true);
check('production helper exports the shared catalog API', exported);

// Validation executes inside getEventKindFamilies(), against production bytes.
function rejectedCatalog(data) {
    var env = install(function() { return FakePromise.resolve(data); });
    var result = observe(env.hw.getEventKindFamilies());
    flushTasks();
    return result.settled && !result.fulfilled;
}

check('missing and empty catalogs are rejected',
      rejectedCatalog(null) &&
      rejectedCatalog({families: []}) &&
      rejectedCatalog({families: [{n: 'Empty', k: []}]}) &&
      rejectedCatalog({families: [
          {n: 'Valid', k: ['valid_kind']}, {n: 'Empty', k: []}
      ]}));

check('malformed families are rejected',
      rejectedCatalog({families: [null]}) &&
      rejectedCatalog({families: [{n: '', k: ['valid_kind']}]}) &&
      rejectedCatalog({families: [{n: 7, k: []}]}) &&
      rejectedCatalog({families: [{n: 'Bad', k: 'not-an-array'}]}) &&
      rejectedCatalog({families: [
          {n: 'Same', k: ['one']}, {n: 'Same', k: ['two']}
      ]}));

check('duplicate and invalid event-kind names are rejected',
      rejectedCatalog({families: [{n: 'Bad', k: ['Bad-Name']}]}) &&
      rejectedCatalog({families: [
          {n: 'One', k: ['same_kind']}, {n: 'Two', k: ['same_kind']}
      ]}) &&
      rejectedCatalog({families: [{n: 'Bad', k: [42]}]}));

check('reserved control and alias event-kind names are rejected',
      rejectedCatalog({families: [{n: 'Bad', k: ['boot']}]}) &&
      rejectedCatalog({families: [{n: 'Bad', k: ['none']}]}) &&
      rejectedCatalog({families: [{n: 'Bad', k: ['set']}]}) &&
      rejectedCatalog({families: [{n: 'Bad', k: ['patch']}]}) &&
      rejectedCatalog({families: [{n: 'Bad', k: ['all']}]}) &&
      rejectedCatalog({families: [{n: 'Bad', k: ['list']}]}));

// One in-flight fetch is shared, and only its validated success is cached.
var shared = deferred();
var sharedCalls = 0;
var cacheEnv = install(function(url) {
    sharedCalls++;
    check('catalog fetch uses the authoritative endpoint', url === '/api/events/kinds', url);
    return shared.promise;
});
var first = cacheEnv.hw.getEventKindFamilies();
var second = cacheEnv.hw.getEventKindFamilies();
check('concurrent catalog consumers share one pending promise', first === second);
check('concurrent catalog consumers issue one request', sharedCalls === 1, String(sharedCalls));
var firstResult = observe(first);
var secondResult = observe(second);
shared.resolve(clone(VALID));
flushTasks();
check('coalesced consumers receive one validated catalog',
      firstResult.fulfilled && secondResult.fulfilled &&
      firstResult.value === secondResult.value &&
      firstResult.value.length === VALID.families.length);
check('validation preserves family and kind declaration order',
      JSON.stringify(firstResult.value) === JSON.stringify(VALID.families),
      JSON.stringify(firstResult.value));
var cachedResult = observe(cacheEnv.hw.getEventKindFamilies());
flushTasks();
check('successful fetch is cached per page',
      sharedCalls === 1 && cachedResult.fulfilled &&
      cachedResult.value === firstResult.value, 'fetches=' + sharedCalls);

// Neither transport errors nor validation errors poison the retry path.
var retryCalls = 0;
var retryEnv = install(function() {
    retryCalls++;
    if (retryCalls === 1) return FakePromise.reject(new Error('radio down'));
    return FakePromise.resolve(clone(VALID));
});
var failedFetch = observe(retryEnv.hw.getEventKindFamilies());
flushTasks();
check('failed catalog request rejects its callers',
      failedFetch.settled && !failedFetch.fulfilled &&
      String(failedFetch.error).indexOf('radio down') >= 0);
var retriedFetch = observe(retryEnv.hw.getEventKindFamilies());
flushTasks();
check('failed catalog request can retry and then cache',
      retryCalls === 2 && retriedFetch.fulfilled &&
      retryEnv.hw.getEventKindFamilies() !== null, 'fetches=' + retryCalls);
flushTasks();
check('successful retry does not issue a third request', retryCalls === 2, String(retryCalls));

var malformedCalls = 0;
var malformedEnv = install(function() {
    malformedCalls++;
    if (malformedCalls === 1) {
        return FakePromise.resolve({families: [{n: 'Bad', k: ['dup', 'dup']}]});
    }
    return FakePromise.resolve(clone(VALID));
});
var malformedFirst = observe(malformedEnv.hw.getEventKindFamilies());
flushTasks();
var malformedSecond = observe(malformedEnv.hw.getEventKindFamilies());
flushTasks();
check('malformed catalog can retry with a later valid response',
      !malformedFirst.fulfilled && malformedSecond.fulfilled && malformedCalls === 2,
      'fetches=' + malformedCalls);

// Picker structure, exact order, display labels, and ordinary selection state.
var pickerCalls = 0;
var pickerEnv = install(function() {
    pickerCalls++;
    return FakePromise.resolve(clone(VALID));
});
var picker = pickerEnv.document.add(new Element('select', 'event-kind'));
var pickerLoad = observe(pickerEnv.hw.fillEventKindSelect('event-kind', {
    selected: 'ota_recovery_entered'
}));
check('picker enters a disabled loading state',
      picker.disabled && picker.children.length === 1 &&
      picker.children[0].textContent === 'Loading event kinds...');
flushTasks();
check('picker load succeeds and re-enables selection', pickerLoad.fulfilled && !picker.disabled);
check('picker preserves non-empty family grouping and order',
      JSON.stringify(groupLabels(picker)) ===
      JSON.stringify(['Connectivity', 'Updates', 'System']),
      JSON.stringify(groupLabels(picker)));
check('picker preserves canonical option order',
      JSON.stringify(optionValues(picker)) === JSON.stringify([
          'wifi_connected', 'ble_peer_online', 'ota_recovery_entered',
          'api_ready', 'tof_sample'
      ]), JSON.stringify(optionValues(picker)));
var pickerOptions = optionsBelow(picker, []);
check('picker derives readable labels without replacing canonical values',
      pickerOptions[0].textContent === 'WiFi connected (wifi_connected)' &&
      pickerOptions[2].textContent === 'OTA recovery entered (ota_recovery_entered)' &&
      pickerOptions[4].textContent === 'ToF sample (tof_sample)');
check('requested known event kind remains selected',
      picker.value === 'ota_recovery_entered' &&
      picker.__hwEventKindSelected === 'ota_recovery_entered');
check('successful picker load dispatches one change event',
      picker.events.length === 1 && picker.events[0].value === 'ota_recovery_entered');

picker.value = 'ble_peer_online';
var rebuilt = observe(pickerEnv.hw.fillEventKindSelect(picker));
flushTasks();
check('cached picker rebuild preserves the current selection',
      rebuilt.fulfilled && picker.value === 'ble_peer_online' && pickerCalls === 1,
      'value=' + picker.value + ' fetches=' + pickerCalls);

var unknown = pickerEnv.document.add(new Element('select', 'unknown-event-kind'));
var unknownLoad = observe(pickerEnv.hw.fillEventKindSelect(unknown, {
    selected: 'future_device_event'
}));
flushTasks();
var unknownGroup = unknown.children[unknown.children.length - 1];
var unknownOption = unknownGroup && unknownGroup.children[0];
check('stored unknown event kind is preserved as unavailable',
      unknownLoad.fulfilled && unknown.value === 'future_device_event' &&
      unknownGroup.label === 'Unavailable' && unknownOption.disabled &&
      unknownOption.textContent === 'Unavailable event (future_device_event)');
check('an unavailable stored value does not disable the whole picker', !unknown.disabled);

// Override only the catalog supplier so two picker generations can complete in
// the opposite order. The production fill helper must fence the older one.
var oldLoad = deferred();
var newLoad = deferred();
var generationCall = 0;
var staleEnv = install(function() { return FakePromise.resolve(clone(VALID)); });
staleEnv.hw.getEventKindFamilies = function() {
    generationCall++;
    return generationCall === 1 ? oldLoad.promise : newLoad.promise;
};
var staleSelect = staleEnv.document.add(new Element('select', 'stale-select'));
var oldResult = observe(staleEnv.hw.fillEventKindSelect(staleSelect, {
    selected: 'wifi_connected'
}));
var newResult = observe(staleEnv.hw.fillEventKindSelect(staleSelect, {
    selected: 'api_ready'
}));
newLoad.resolve(clone(VALID.families));
flushTasks();
check('newer picker generation can complete before the older request',
      newResult.fulfilled && staleSelect.value === 'api_ready' &&
      staleSelect.events.length === 1);
oldLoad.resolve(clone(VALID.families));
flushTasks();
check('stale async completion cannot replace the newer selection',
      oldResult.fulfilled && staleSelect.value === 'api_ready' &&
      staleSelect.events.length === 1,
      'value=' + staleSelect.value + ' changes=' + staleSelect.events.length);

// Failure leaves no selectable default. A stored value is shown for recovery,
// but the control remains disabled so it cannot be mistaken for catalog proof.
var closedEnv = install(function() {
    return FakePromise.reject(new Error('catalog offline'));
});
var closed = closedEnv.document.add(new Element('select', 'closed-select'));
var closedResult = observe(closedEnv.hw.fillEventKindSelect(closed));
flushTasks();
check('catalog failure rejects the picker load promise',
      closedResult.settled && !closedResult.fulfilled);
check('catalog failure leaves an empty picker fail-closed',
      closed.disabled && closed.children.length === 1 &&
      closed.children[0].disabled &&
      closed.children[0].textContent === 'Event kinds unavailable' &&
      closed.title.indexOf('catalog offline') >= 0);

var storedClosedEnv = install(function() {
    return FakePromise.reject(new Error('bad catalog'));
});
var storedClosed = storedClosedEnv.document.add(new Element('select', 'stored-closed'));
var storedClosedResult = observe(storedClosedEnv.hw.fillEventKindSelect(storedClosed, {
    selected: 'future_device_event'
}));
flushTasks();
check('failed load shows but cannot authorize a stored event kind',
      !storedClosedResult.fulfilled && storedClosed.disabled &&
      storedClosed.value === 'future_device_event' &&
      !storedClosed.children[0].disabled &&
      storedClosed.children[0].textContent ===
          'Stored event (future_device_event) - catalog unavailable');

var missingTarget = observe(storedClosedEnv.hw.fillEventKindSelect('not-present'));
flushTasks();
check('missing picker target rejects without a fetch',
      missingTarget.settled && !missingTarget.fulfilled &&
      String(missingTarget.error).indexOf('select not found') >= 0);

__out(failures ? 'HARNESS_RESULT FAIL ' + failures : 'HARNESS_RESULT PASS');

// See _bootstrap.js: JXA echoes the last expression unless it is undefined.
undefined;
