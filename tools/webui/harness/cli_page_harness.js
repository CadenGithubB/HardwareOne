/* Behavioral harness for the exact JavaScript extracted from WebPage_CLI.h.
 *
 * The HTTP response is authoritative for interactive-help state.  The fake
 * device below exposes that state through X-HW1-CLI-Help, just as the real
 * /api/cli response does.  This deliberately uses commands whose spelling is
 * insufficient to infer the transition: addressed help, module-level back,
 * and an ordinary command passed through from help mode.
 *
 * Requires _bootstrap.js from tools/webui/js_engine.py.
 *   __argv[0] = path to the extracted WebPage_CLI.h JavaScript
 */

var PAGE_JS_PATH = __argv[0];
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

var absorbed = [];

/* Deterministic thenable. It resolves synchronously by default, but can remain
 * pending so the harness can deliver a bootstrap-log response after a newer
 * command response. It assimilates itself so both shipping shapes work:
 *
 *   postForm(...).then(function (r) { return r.text(); }).then(...)
 *   postForm(...).then(function (r) { return r.text().then(...); })
 *
 * It does not model the browser microtask queue; tests explicitly choose when
 * a deferred response settles instead.
 */
function HarnessPromise() {
    this._cliHarnessPromise = true;
    this._state = 0; // 0 pending, 1 fulfilled, 2 rejected
    this._value = undefined;
    this._handlers = [];
}
HarnessPromise.prototype.then = function (fn) {
    var child = new HarnessPromise();
    this._handlers.push({fulfilled: fn, rejected: null, child: child});
    drainPromise(this);
    return child;
};
HarnessPromise.prototype.catch = function (fn) {
    var child = new HarnessPromise();
    this._handlers.push({fulfilled: null, rejected: fn, child: child});
    drainPromise(this);
    return child;
};

function settlePromise(promise, state, value) {
    if (promise._state !== 0) return;
    if (value && value._cliHarnessPromise) {
        value.then(function (next) { settlePromise(promise, 1, next); })
             .catch(function (error) { settlePromise(promise, 2, error); });
        return;
    }
    promise._state = state;
    promise._value = value;
    drainPromise(promise);
}

function drainPromise(promise) {
    if (promise._state === 0) return;
    while (promise._handlers.length) {
        var handler = promise._handlers.shift();
        var fn = promise._state === 1 ? handler.fulfilled : handler.rejected;
        if (!fn) {
            settlePromise(handler.child, promise._state, promise._value);
            continue;
        }
        try {
            settlePromise(handler.child, 1, fn(promise._value));
        } catch (error) {
            absorbed.push(String(error && error.stack ? error.stack : error));
            settlePromise(handler.child, 2, error);
        }
    }
}

function P(value, failed) {
    var promise = new HarnessPromise();
    settlePromise(promise, failed ? 2 : 1, value);
    return promise;
}

function Deferred() {
    this.promise = new HarnessPromise();
}
Deferred.prototype.resolve = function (value) {
    settlePromise(this.promise, 1, value);
};
Deferred.prototype.reject = function (error) {
    settlePromise(this.promise, 2, error);
};

function Storage(seed) {
    this.data = {};
    seed = seed || {};
    for (var key in seed) {
        if (Object.prototype.hasOwnProperty.call(seed, key)) {
            this.data[key] = String(seed[key]);
        }
    }
}
Storage.prototype.getItem = function (key) {
    return Object.prototype.hasOwnProperty.call(this.data, key)
        ? this.data[key] : null;
};
Storage.prototype.setItem = function (key, value) {
    this.data[key] = String(value);
};
Storage.prototype.removeItem = function (key) { delete this.data[key]; };

function Element(id) {
    this.id = id;
    this.textContent = '';
    this.value = '';
    this.disabled = false;
    this.style = {};
    this.scrollTop = 0;
    this.scrollHeight = 100;
    this.focusCount = 0;
    this.listeners = {};
}
Element.prototype.addEventListener = function (name, fn) {
    if (!this.listeners[name]) this.listeners[name] = [];
    this.listeners[name].push(fn);
};
Element.prototype.focus = function () { this.focusCount++; };

function response(body, helpState, headerReads, status) {
    status = status || 200;
    return {
        ok: status >= 200 && status < 300,
        status: status,
        headers: {
            get: function (name) {
                headerReads.push(String(name));
                return String(name).toLowerCase() === 'x-hw1-cli-help'
                    ? helpState : null;
            }
        },
        text: function () { return P(body, false); }
    };
}

function boot(seed, logBodies, commandReplies) {
    var storage = new Storage(seed);
    var elements = {
        'cli-input': new Element('cli-input'),
        'cli-output': new Element('cli-output'),
        'cli-exec': new Element('cli-exec'),
        'cli-target-toggle': new Element('cli-target-toggle'),
        'cli-btn-local': new Element('cli-btn-local'),
        'cli-btn-bonded': new Element('cli-btn-bonded')
    };
    var headerReads = [];
    var posts = [];
    var postFormTextCalls = 0;
    var logFetches = 0;
    var logIndex = 0;
    var replyIndex = 0;
    var deferredLogs = [];
    var intervalId = 1;
    var intervals = {};

    function setIntervalStub(fn) {
        var id = intervalId++;
        intervals[id] = fn;
        return id;
    }
    function clearIntervalStub(id) { delete intervals[id]; }
    function runPollers() {
        var snapshot = [];
        for (var id in intervals) {
            if (Object.prototype.hasOwnProperty.call(intervals, id)) {
                snapshot.push(intervals[id]);
            }
        }
        for (var i = 0; i < snapshot.length; i++) snapshot[i]();
    }

    function fetchStub(url) {
        if (url !== '/api/cli/logs') {
            absorbed.push('unexpected fetch URL: ' + url);
            return P(new Error('unexpected fetch URL'), true);
        }
        logFetches++;
        var scripted = logBodies[Math.min(logIndex, logBodies.length - 1)] || '';
        logIndex++;
        var body = typeof scripted === 'object' ? scripted.body : scripted;
        var help = typeof scripted === 'object' ? scripted.help : null;
        var status = typeof scripted === 'object' ? scripted.status : 200;
        if (typeof scripted === 'object' && scripted.defer) {
            var deferred = new Deferred();
            deferredLogs.push({deferred: deferred, scripted: scripted});
            return deferred.promise;
        }
        if (typeof scripted === 'object' && scripted.error) {
            return P(new Error(scripted.error), true);
        }
        return P(response(body || '', help, [], status), false);
    }

    var windowStub = {
        __cliPoller: null,
        listeners: {},
        addEventListener: function (name, fn) {
            if (!this.listeners[name]) this.listeners[name] = [];
            this.listeners[name].push(fn);
        }
    };

    var hw = {
        $: function (id) { return elements[id] || null; },
        on: function (element, name, fn) {
            if (element) element.addEventListener(name, fn);
        },
        setText: function (element, text) {
            if (typeof element === 'string') element = elements[element];
            if (element) element.textContent = text;
        },
        postForm: function (url, form) {
            var expected = commandReplies[replyIndex++];
            posts.push({url: url, form: form});
            if (!expected) {
                absorbed.push('unexpected command post: ' + (form && form.cmd));
                return P(new Error('unexpected command post'), true);
            }
            if (!form || form.cmd !== expected.command) {
                absorbed.push('expected command ' + expected.command + ', got ' +
                              (form && form.cmd));
            }
            return P(response(expected.body, expected.help, headerReads,
                              expected.status || 200), false);
        },
        postFormText: function () {
            postFormTextCalls++;
            return P(new Error('CLI must inspect the Response headers'), true);
        }
    };

    var quietConsole = {
        log: function () {}, debug: function () {}, warn: function () {},
        error: function () {}
    };
    var pageSource = slurp(PAGE_JS_PATH);
    var probes = null;
    try {
        probes = new Function(
            'window', 'hw', 'fetch', 'setInterval', 'clearInterval',
            'localStorage', 'console',
            pageSource + '\nreturn {' +
                'execute: executeCommand,' +
                'setTarget: cliSetTarget,' +
                'state: function(){ return {' +
                    'inHelp: inHelp, outputBackup: outputBackup,' +
                    'output: cliOutput ? cliOutput.textContent : ""' +
                '}; }' +
            '};'
        )(
            windowStub, hw, fetchStub, setIntervalStub, clearIntervalStub,
            storage, quietConsole
        );
    } catch (error) {
        absorbed.push(String(error && error.stack ? error.stack : error));
    }

    return {
        storage: storage,
        elements: elements,
        headerReads: headerReads,
        posts: posts,
        probes: probes,
        runPollers: runPollers,
        getLogFetches: function () { return logFetches; },
        getPostFormTextCalls: function () { return postFormTextCalls; },
        repliesConsumed: function () { return replyIndex; },
        deferredLogCount: function () { return deferredLogs.length; },
        settleNextDeferredLog: function () {
            var pending = deferredLogs.shift();
            if (!pending) {
                absorbed.push('no deferred log response to settle');
                return;
            }
            var scripted = pending.scripted;
            if (scripted.error) {
                pending.deferred.reject(new Error(scripted.error));
            } else {
                pending.deferred.resolve(response(
                    scripted.body || '', scripted.help, [], scripted.status || 200));
            }
        },
        command: function (text) {
            elements['cli-input'].value = text;
            if (!probes) return;
            probes.execute();
        }
    };
}

var ESC = String.fromCharCode(27);
var CLEAR = ESC + '[2J' + ESC + '[H';

/* Fresh page: every transition deliberately disagrees with command-name
 * guessing at least once. */
var freshReplies = [
    {command: 'help espnow', help: 'active',
     body: CLEAR + 'ESPNOW HELP PAGE\n'},
    {command: 'p999', help: 'active', status: 400,
     body: 'Error: page p999 is out of range (p1-p6)\n'},
    {command: 'help espnow p999', help: 'active', status: 400,
     body: 'Error: page p999 is out of range (p1-p6)\n'},
    {command: 'back', help: 'active',
     body: CLEAR + 'MAIN HELP PAGE\n'},
    {command: 'exit', help: 'inactive',
     body: 'Returned to normal CLI mode.\n'},
    {command: 'help sensors', help: 'active',
     body: CLEAR + 'SENSORS HELP PAGE\n'},
    {command: 'bogus', help: 'inactive', status: 400,
     body: 'Unknown command: bogus\n'}
];
var fresh = boot({}, [
    {body: 'baseline log', help: 'inactive'},
    {body: 'ambient log while help is active', help: 'active'},
    {body: 'live log after exit', help: 'inactive'}
], freshReplies);

check('shipping CLI source loaded', !!fresh.probes,
      absorbed.length ? absorbed.join(' | ') : 'no probes');
check('normal page fetches its initial logs', fresh.getLogFetches() === 1,
      String(fresh.getLogFetches()));

fresh.command('help espnow');
var state = fresh.probes ? fresh.probes.state() : {};
check('addressed help uses the Response-returning form helper',
      fresh.posts.length === 1 && fresh.posts[0].url === '/api/cli' &&
      fresh.posts[0].form.cmd === 'help espnow');
check('addressed help inspects the authoritative header',
      fresh.headerReads.length >= 1 &&
      fresh.headerReads[0].toLowerCase() === 'x-hw1-cli-help',
      fresh.headerReads.join(','));
check('addressed help activates and persists help mode',
      state.inHelp === true && fresh.storage.getItem('cliInHelp') === 'true');
check('addressed help preserves the pre-help terminal',
      state.outputBackup.indexOf('baseline log') >= 0 &&
      fresh.storage.getItem('cliOutputHistoryBackup') !== null &&
      fresh.storage.getItem('cliOutputHistoryBackup').indexOf('baseline log') >= 0,
      JSON.stringify(state));
check('addressed help owns the terminal surface',
      state.output.indexOf('ESPNOW HELP PAGE') >= 0 &&
      state.output.indexOf('baseline log') < 0,
      JSON.stringify(state.output));

var helpOutputBeforePoll = state.output;
var fetchesBeforeActivePoll = fresh.getLogFetches();
fresh.runPollers();
state = fresh.probes ? fresh.probes.state() : {};
check('active-help polling still fetches authoritative metadata',
      fresh.getLogFetches() === fetchesBeforeActivePoll + 1,
      String(fresh.getLogFetches()));
check('active-help polling never applies the mirrored log body',
      state.inHelp === true && state.output === helpOutputBeforePoll,
      JSON.stringify(state));

fresh.command('p999');
state = fresh.probes ? fresh.probes.state() : {};
check('non-2xx page error keeps the authoritative help mode active',
      state.inHelp === true && fresh.storage.getItem('cliInHelp') === 'true');
check('non-2xx page error body remains visible on the current page',
      state.output.indexOf('ESPNOW HELP PAGE') >= 0 &&
      state.output.indexOf('Error: page p999 is out of range') >= 0,
      JSON.stringify(state.output));

fresh.command('help espnow p999');
state = fresh.probes ? fresh.probes.state() : {};
check('invalid addressed p<N> also preserves the active help session',
      state.inHelp === true && fresh.storage.getItem('cliInHelp') === 'true');
check('invalid addressed p<N> response body remains visible',
      state.output.indexOf('ESPNOW HELP PAGE') >= 0 &&
      state.output.indexOf('Error: page p999 is out of range') >= 0,
      JSON.stringify(state.output));

var readsBeforeBack = fresh.headerReads.length;
fresh.command('back');
state = fresh.probes ? fresh.probes.state() : {};
check('module back inspects the authoritative header',
      fresh.headerReads.length > readsBeforeBack,
      fresh.headerReads.join(','));
check('module back remains in help mode',
      state.inHelp === true && fresh.storage.getItem('cliInHelp') === 'true');
check('module back renders the main help page',
      state.output.indexOf('MAIN HELP PAGE') >= 0 &&
      state.output.indexOf('baseline log') < 0,
      JSON.stringify(state.output));
check('module back keeps the original terminal backup',
      state.outputBackup.indexOf('baseline log') >= 0 &&
      fresh.storage.getItem('cliOutputHistoryBackup') !== null &&
      fresh.storage.getItem('cliOutputHistoryBackup').indexOf('baseline log') >= 0);

var readsBeforeExit = fresh.headerReads.length;
fresh.command('exit');
state = fresh.probes ? fresh.probes.state() : {};
check('exit inspects the authoritative header without needing a clear sequence',
      fresh.headerReads.length > readsBeforeExit,
      fresh.headerReads.join(','));
check('inactive exit clears persisted help state',
      state.inHelp === false && fresh.storage.getItem('cliInHelp') === 'false' &&
      fresh.storage.getItem('cliOutputHistoryBackup') === null);
check('inactive exit restores the prior terminal and reports the result',
      state.output.indexOf('baseline log') >= 0 &&
      state.output.indexOf('Returned to normal CLI mode.') >= 0,
      JSON.stringify(state.output));

fresh.runPollers();
state = fresh.probes ? fresh.probes.state() : {};
check('log polling resumes after inactive exit',
      fresh.getLogFetches() === fetchesBeforeActivePoll + 2 &&
      state.output === 'live log after exit',
      fresh.getLogFetches() + ' ' + JSON.stringify(state.output));

var readsBeforeSecondHelp = fresh.headerReads.length;
fresh.command('help sensors');
state = fresh.probes ? fresh.probes.state() : {};
check('a second addressed help response activates mode',
      state.inHelp === true && state.output.indexOf('SENSORS HELP PAGE') >= 0 &&
      fresh.headerReads.length > readsBeforeSecondHelp);

var readsBeforePassthrough = fresh.headerReads.length;
fresh.command('bogus');
state = fresh.probes ? fresh.probes.state() : {};
check('passthrough command inspects its inactive header',
      fresh.headerReads.length > readsBeforePassthrough,
      fresh.headerReads.join(','));
check('inactive passthrough exits and clears help state',
      state.inHelp === false && fresh.storage.getItem('cliInHelp') === 'false' &&
      fresh.storage.getItem('cliOutputHistoryBackup') === null);
check('inactive passthrough restores the normal terminal surface',
      state.output.indexOf('live log after exit') >= 0,
      JSON.stringify(state.output));
check('non-2xx command response body remains visible',
      freshReplies[6].status === 400 &&
      state.output.indexOf('Unknown command: bogus') >= 0,
      JSON.stringify(state.output));

var allFormsInteractive = fresh.posts.length === freshReplies.length;
for (var fp = 0; fp < fresh.posts.length; fp++) {
    var form = fresh.posts[fp].form || {};
    if (form.capture !== '1' || form.interactive !== '1') {
        allFormsInteractive = false;
    }
}
check('all local CLI posts retain capture and interactive flags',
      allFormsInteractive, JSON.stringify(fresh.posts));
check('CLI no longer discards headers through postFormText',
      fresh.getPostFormTextCalls() === 0,
      String(fresh.getPostFormTextCalls()));
check('fresh scenario consumed every scripted response',
      fresh.repliesConsumed() === freshReplies.length,
      fresh.repliesConsumed() + '/' + freshReplies.length);

/* A reload can inherit cliInHelp=true after a reset, timeout, or older page
 * bug.  The next server response must reconcile that stale client state even
 * when the command is not an exit keyword. */
var staleReplies = [
    {command: 'uptime', help: 'inactive', body: 'Uptime after reload\n'}
];
var stale = boot({
    cliInHelp: 'true',
    cliOutputHistoryBackup: 'saved before stale help'
}, [{body: 'live log after reconciliation', help: 'inactive'}], staleReplies);

check('initial logs request asks the server to reconcile stale help state',
      stale.getLogFetches() === 1,
      String(stale.getLogFetches()));
var staleInitialState = stale.probes ? stale.probes.state() : {};
check('initial inactive metadata clears stale localStorage',
      staleInitialState.inHelp === false &&
      stale.storage.getItem('cliInHelp') === 'false' &&
      stale.storage.getItem('cliOutputHistoryBackup') === null,
      JSON.stringify(staleInitialState));
stale.command('uptime');
var staleState = stale.probes ? stale.probes.state() : {};
check('stale session inspects the inactive response header',
      stale.headerReads.length >= 1 &&
      stale.headerReads[0].toLowerCase() === 'x-hw1-cli-help',
      stale.headerReads.join(','));
check('inactive response reconciles stale localStorage',
      staleState.inHelp === false && stale.storage.getItem('cliInHelp') === 'false' &&
      stale.storage.getItem('cliOutputHistoryBackup') === null);
check('stale reconciliation renders the command result',
      staleState.output.indexOf('Uptime after reload') >= 0,
      JSON.stringify(staleState.output));
stale.runPollers();
staleState = stale.probes ? stale.probes.state() : {};
check('polling resumes after stale state becomes inactive',
      stale.getLogFetches() === 2 &&
      staleState.output === 'live log after reconciliation',
      stale.getLogFetches() + ' ' + JSON.stringify(staleState.output));
check('stale scenario also avoids postFormText',
      stale.getPostFormTextCalls() === 0,
      String(stale.getPostFormTextCalls()));

/* A bootstrap log snapshot can be much larger than a command response and
 * finish later on a different connection. Its older inactive state must not
 * undo a newer addressed-help response. */
var race = boot({
    cliInHelp: 'false',
    cliOutputHistory: 'race baseline'
}, [
    {defer: true, body: 'stale bootstrap body', help: 'inactive'},
    {body: 'current help metadata', help: 'active'}
], [
    {command: 'help espnow', help: 'active',
     body: CLEAR + 'RACE HELP PAGE\n'}
]);
check('bootstrap log response can remain pending while commands run',
      race.deferredLogCount() === 1 && race.getLogFetches() === 1,
      race.deferredLogCount() + ' ' + race.getLogFetches());
race.command('help espnow');
var raceState = race.probes ? race.probes.state() : {};
check('newer command response enters help before bootstrap completes',
      raceState.inHelp === true && raceState.output.indexOf('RACE HELP PAGE') >= 0,
      JSON.stringify(raceState));
race.settleNextDeferredLog();
raceState = race.probes ? race.probes.state() : {};
check('generation fence ignores the older bootstrap response',
      raceState.inHelp === true &&
      race.storage.getItem('cliInHelp') === 'true' &&
      raceState.output.indexOf('RACE HELP PAGE') >= 0 &&
      raceState.output.indexOf('stale bootstrap body') < 0,
      JSON.stringify(raceState));

/* A transient bootstrap failure must not strand stale localStorage: the
 * periodic metadata poll retries even while the local client believes help is
 * active. */
var retry = boot({
    cliInHelp: 'true',
    cliOutputHistory: 'cached stale help page',
    cliOutputHistoryBackup: 'retry baseline'
}, [
    {error: 'temporary log failure'},
    {body: 'logs after retry', help: 'inactive'}
], []);
var retryState = retry.probes ? retry.probes.state() : {};
check('failed bootstrap leaves the saved help surface intact',
      retry.getLogFetches() === 1 && retryState.inHelp === true &&
      retryState.output === 'cached stale help page',
      retry.getLogFetches() + ' ' + JSON.stringify(retryState));
retry.runPollers();
retryState = retry.probes ? retry.probes.state() : {};
check('poller retries metadata while local help state is active',
      retry.getLogFetches() === 2,
      String(retry.getLogFetches()));
check('successful retry clears stale help and renders live logs',
      retryState.inHelp === false &&
      retry.storage.getItem('cliInHelp') === 'false' &&
      retry.storage.getItem('cliOutputHistoryBackup') === null &&
      retryState.output === 'logs after retry',
      JSON.stringify(retryState));

/* The inverse stale-state case: the server owns help but this page loaded with
 * cliInHelp=false. Preserve the last saved help surface, then let a later
 * inactive poll return the tab to normal logs. */
var serverActive = boot({
    cliInHelp: 'false',
    cliOutputHistory: 'cached server-owned help page',
    cliOutputHistoryBackup: 'normal surface before external help'
}, [
    {body: 'normal mirror while server help is active', help: 'active'},
    {body: 'normal logs after external exit', help: 'inactive'}
], []);
var serverActiveState = serverActive.probes ? serverActive.probes.state() : {};
check('server-active metadata repairs local false state',
      serverActiveState.inHelp === true &&
      serverActive.storage.getItem('cliInHelp') === 'true');
check('server-active reconciliation preserves cached help output',
      serverActiveState.output === 'cached server-owned help page' &&
      serverActiveState.output.indexOf('normal mirror while server help is active') < 0,
      JSON.stringify(serverActiveState));
serverActive.runPollers();
serverActiveState = serverActive.probes ? serverActive.probes.state() : {};
check('inactive poll reconciles an externally-ended help session',
      serverActiveState.inHelp === false &&
      serverActive.storage.getItem('cliInHelp') === 'false' &&
      serverActive.storage.getItem('cliOutputHistoryBackup') === null &&
      serverActiveState.output === 'normal logs after external exit',
      JSON.stringify(serverActiveState));

/* A log response slower than the 500 ms interval must not trigger a growing
 * stack of requests whose generations invalidate one another forever. */
var slow = boot({}, [
    {defer: true, body: 'slow first log', help: 'inactive'},
    {body: 'log after slow request', help: 'inactive'}
], []);
check('slow log scenario starts exactly one request',
      slow.getLogFetches() === 1 && slow.deferredLogCount() === 1,
      slow.getLogFetches() + ' ' + slow.deferredLogCount());
slow.runPollers();
slow.runPollers();
check('overlapping poll ticks do not start concurrent log requests',
      slow.getLogFetches() === 1 && slow.deferredLogCount() === 1,
      slow.getLogFetches() + ' ' + slow.deferredLogCount());
slow.settleNextDeferredLog();
slow.runPollers();
check('polling resumes after the slow request settles',
      slow.getLogFetches() === 2 &&
      slow.probes.state().output === 'log after slow request',
      slow.getLogFetches() + ' ' + JSON.stringify(slow.probes.state()));

/* Switching to the bonded terminal invalidates a local log response already
 * in flight, so it cannot erase the bonded-target surface when it arrives. */
var bondRace = boot({}, [
    {defer: true, body: 'late local log', help: 'inactive'}
], []);
bondRace.probes.setTarget(true);
bondRace.settleNextDeferredLog();
check('late local log cannot overwrite the bonded terminal',
      bondRace.probes.state().output.indexOf('[Bonded device CLI]') >= 0 &&
      bondRace.probes.state().output.indexOf('late local log') < 0,
      JSON.stringify(bondRace.probes.state()));

check('page callbacks threw no exceptions', absorbed.length === 0,
      absorbed.join(' | '));

__out(failures ? 'HARNESS_RESULT FAIL ' + failures : 'HARNESS_RESULT PASS');

// JXA echoes a script's final expression.  Keep this explicitly undefined.
undefined;
