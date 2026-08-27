// _bootstrap.js -- the one engine shim for every harness under tools/webui/harness.
//
// WHY THIS FILE EXISTS
// --------------------
// The web-UI tests need to RUN the JavaScript that ships inside the firmware's
// C++ raw string literals, and they need to do it on whatever JS engine the
// machine happens to have. This box has no node; it has deno, Apple's jsc, and
// osascript. A Linux CI box will have node and nothing else. Those engines
// agree on the language and disagree on absolutely everything around it: how a
// script receives arguments, how it reads a file, and -- worst -- what "print
// this line" means. So a harness that calls console.log() is already broken on
// two of the five targets.
//
// This file resolves four globals up front so a harness can be written once:
//
//     __out(s)      one line to REAL stdout, on every engine
//     slurp(path)   file contents as a string (relative paths resolve to cwd,
//                   which run_js pins to the repo root)
//     __argv        array of the string arguments passed after the script
//     __engine      the detected engine name, for diagnostics only
//
// WHY THE RUNNER CONCATENATES INSTEAD OF IMPORTING
// ------------------------------------------------
// js_engine.run_js() does not load this file from the harness. It glues
// bootstrap + harness into ONE temp file and runs that. The reason is that the
// engines disagree about what a plain .js file even is, and they disagree in
// opposite directions. Measured, not assumed:
//
//                              deno (ES module)     jsc / JXA (classic script)
//     top-level `var` -> global    no                        yes
//     `this` at top level          undefined                 global object
//     bare assignment (x = 1)      ReferenceError            creates a global
//
// So there is no export mechanism that works on both: ES `export` is a syntax
// error to jsc, and leaking a global is impossible under deno. One compilation
// unit sidesteps the whole argument -- a top-level `var` here is simply in
// scope for the harness text that follows it, module goal or not.
//
// TWO TRAPS SPECIFIC TO JXA (osascript -l JavaScript). Both measured:
//
//   (a) console.log() writes to STDERR under JXA, and console.error() does not
//       exist at all (`typeof console.error` is "undefined"). The only real
//       stdout path is NSFileHandle.fileHandleWithStandardOutput, which is what
//       __out does below.
//
//   (b) osascript ECHOES THE SCRIPT'S LAST EXPRESSION VALUE to stdout. A
//       harness whose final statement evaluates to something will emit a stray
//       trailing line that no other engine emits, and stdout comparison across
//       engines fails for a reason that looks like nothing.
//
//       ===> EVERY HARNESS FILE MUST END WITH A BARE `undefined;` <===
//
//       Same rule if you define run(): osascript calls it and echoes its return
//       value, so return undefined. Prefer plain top-level code -- run() is
//       called after the file has finished evaluating, which is too late to be
//       useful here anyway.
//
// Engine identification is by FEATURE DETECTION only. Never sniff a version
// string: bun answers to process.versions, deno 2 defines a `process` shim for
// node compatibility, and any of them can add a global next release. Ask what
// the thing can do, in an order where the more specific answer wins.

var __engine = (typeof Deno !== "undefined") ? "deno"
             : (typeof print === "function" && typeof readFile === "function") ? "jsc"
             : (typeof ObjC !== "undefined") ? "jxa"
             : (typeof process === "object" && process.versions) ? "node"
             : (typeof scriptArgs !== "undefined") ? "qjs"
             : null;

var __argv = [];
var __out = null;
var slurp = null;

if (__engine === "deno") {
    // console.log is genuine stdout here. Reads are sandboxed: run_js grants
    // --allow-read for the repo root and the system temp directory, so slurp
    // works on repo sources and on fixtures a test wrote to a
    // TemporaryDirectory. Anywhere else needs run_js(read_roots=[...]); a
    // denied read raises NotCapable rather than returning empty.
    __argv = Array.prototype.slice.call(Deno.args);
    __out = function (s) {
        console.log(String(s));
    };
    slurp = function (path) {
        return Deno.readTextFileSync(String(path));
    };

} else if (__engine === "jsc") {
    // Top-level `arguments` holds everything after the mandatory "--" separator
    // in the jsc command line. Without that separator jsc treats the first
    // argument as a SECOND SCRIPT FILE and dies "Could not open file: ...".
    // run_js always passes it; the guard is for hand-runs that forget.
    __argv = (typeof arguments !== "undefined")
        ? Array.prototype.slice.call(arguments)
        : [];
    __out = function (s) {
        print(String(s));
    };
    slurp = function (path) {
        return readFile(String(path));
    };

} else if (__engine === "jxa") {
    ObjC.import("Foundation");

    // Each call is its own write(2) through the ObjC bridge, which costs about
    // 20x what the other engines charge -- measured at 0.98s for 5000 lines
    // versus 0.05s on deno and 0.01s on jsc. That is fine for a test harness
    // and is NOT worth buffering: a buffer would need a flush on the throw
    // path, and losing the output that explains a failure is a worse trade
    // than a second of wall clock.
    __out = function (s) {
        var line = String(s) + "\n";
        $.NSFileHandle.fileHandleWithStandardOutput.writeData(
            $.NSString.alloc.initWithUTF8String(line)
                .dataUsingEncoding($.NSUTF8StringEncoding));
    };

    slurp = function (path) {
        var text = $.NSString.stringWithContentsOfFileEncodingError(
            String(path), $.NSUTF8StringEncoding, null);
        // A failed read returns nil, which unwraps to undefined rather than
        // throwing. Turn it into an error so a typo is not silently "".
        var unwrapped = ObjC.unwrap(text);
        if (unwrapped === undefined || unwrapped === null) {
            throw new Error("slurp: cannot read " + path);
        }
        return unwrapped;
    };

    // The arguments are NOT reachable from top-level code -- JXA hands them to
    // run(argv), which osascript calls only after the whole file has evaluated.
    // The process argv is reachable, so take them from there. run_js builds the
    // command line as [osascript, "-l", "JavaScript", <script>, ...args], so
    // everything two slots past "-l" is ours.
    var __raw = ObjC.deepUnwrap($.NSProcessInfo.processInfo.arguments);
    var __dashL = __raw.indexOf("-l");
    __argv = (__dashL >= 0) ? __raw.slice(__dashL + 3) : __raw.slice(4);

} else if (__engine === "node") {
    // Covers bun too: it implements the node globals used here.
    // UNVERIFIED on this machine -- neither node nor bun is installed. The
    // shapes come from the documented APIs, so treat a failure here as this
    // branch being wrong, not the harness.
    __argv = process.argv.slice(2);
    __out = function (s) {
        process.stdout.write(String(s) + "\n");
    };
    slurp = function (path) {
        return require("fs").readFileSync(String(path), "utf8");
    };

} else if (__engine === "qjs") {
    // UNVERIFIED -- quickjs is not installed here. scriptArgs[0] is the script
    // path itself, hence the slice. `std` is a global only when qjs was built
    // or invoked with the std module exposed; say so plainly if it is missing
    // rather than failing inside a stack frame nobody can read.
    __argv = Array.prototype.slice.call(scriptArgs, 1);
    __out = function (s) {
        print(String(s));
    };
    slurp = function (path) {
        if (typeof std === "undefined") {
            throw new Error("slurp: qjs has no `std` global (needs --std)");
        }
        return std.loadFile(String(path));
    };

} else {
    // Nothing recognised. Do not fail at load time -- js_engine.py decides
    // whether an engine exists, and a harness may want to report the problem
    // itself. Fail loudly at the point of use instead.
    __out = function () {
        throw new Error("_bootstrap: unrecognised JS engine, no stdout path");
    };
    slurp = function () {
        throw new Error("_bootstrap: unrecognised JS engine, no file reader");
    };
}

undefined;
