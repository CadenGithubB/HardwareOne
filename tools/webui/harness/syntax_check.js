/*
 * syntax_check.js - parse every JavaScript region handed to it, execute none.
 *
 * WHY THIS EXISTS
 *   The firmware build never parses the web UI's JavaScript. To the C++ compiler
 *   an R"JS( ... )JS" literal is an opaque array of bytes, so a missing brace
 *   compiles, links, flashes, and then fails in front of whoever is holding the
 *   board. WebPage_Games.h carries a hand-written window.onerror handler for
 *   exactly this reason -- its own comment says a syntax error otherwise shows
 *   up as a silent black screen.
 *
 *   Worse, some of that JavaScript sits behind #if ENABLE_HTTPS or
 *   #if ENABLE_NEOPIXEL. On a board with those off, the preprocessor discards
 *   the block and NOTHING in the toolchain has ever looked at it. A green build
 *   proves only the board you built.
 *
 * PARSE, DO NOT RUN
 *   Regions are compiled with the AsyncFunction constructor, which is a pure
 *   syntax check that executes nothing:
 *
 *       new (Object.getPrototypeOf(async function(){}).constructor)(src)
 *
 *   AsyncFunction rather than `new Function` because it is strictly more
 *   permissive (it also accepts top-level await), and permissiveness is what
 *   keeps false failures down. Rather than a module goal (import(), deno check)
 *   because these are classic browser scripts: a module goal rejects top-level
 *   `return` and demands import/export, and would fail perfectly good pages.
 *
 * INPUT   __argv[0] = manifest path, one JSON object per line: {name, file}
 * OUTPUT  SYNTAX OK <name>   |   SYNTAX FAIL <name> <message>
 *         then exactly one:  SYNTAX_RESULT PASS   |   SYNTAX_RESULT FAIL <n>
 */

var AsyncFunctionCtor = null;
try {
    AsyncFunctionCtor = Object.getPrototypeOf(async function () {}).constructor;
} catch (e) {
    // Older jsc builds can refuse to construct one. Fall back to plain Function:
    // the only capability lost is top-level await, which no block in this tree
    // uses. Losing the check entirely would be far worse.
    AsyncFunctionCtor = Function;
}

// SECOND PARSE, for the script goal.
//
// new AsyncFunction(src) parses a FunctionBody, where top-level `return` and
// top-level `await` are both LEGAL. In a classic <script> they are SyntaxErrors
// — and every <script> in components/hardwareone is classic (no modules
// anywhere), with real top-level code in most regions. So the primary parse
// alone would pass JavaScript the browser refuses to run.
//
// A class static block is the cheapest construct that imposes the script goal's
// rules on a statement list. It is also STRICT-mode code, so it additionally
// rejects a handful of legal sloppy constructs (octal literals, `with`,
// duplicate parameters, `let` as an identifier). None occur in this tree today,
// which is why this is worth having — but a failure from this parse is tagged
// [script-goal] so that if one ever does appear, the message says which rule
// rejected it instead of looking like an ordinary syntax error.
var scriptGoalOk = false;
try {
    new AsyncFunctionCtor("class __ProbeP { static { var probe = 1; } }");
    scriptGoalOk = true;
} catch (e) {
    // An engine without class static blocks (or the `Function` fallback above)
    // would otherwise reject EVERY region and turn the gate uniformly red.
    // Skipping the second parse loses strictness, not correctness.
    scriptGoalOk = false;
}

var manifestPath = __argv[0];
var lines = slurp(manifestPath).split("\n");
var failures = 0, checked = 0;

for (var i = 0; i < lines.length; i++) {
    var line = lines[i].replace(/^\s+|\s+$/g, "");
    if (!line) continue;
    var entry = JSON.parse(line);
    var src = slurp(entry.file);
    checked++;
    var primaryErr = null;
    try { new AsyncFunctionCtor(src); }
    catch (err) { primaryErr = String(err && err.message ? err.message : err); }

    var goalErr = null;
    if (primaryErr === null && scriptGoalOk) {
        try { new AsyncFunctionCtor("class __ProbeP { static {\n" + src + "\n} }"); }
        catch (err2) {
            goalErr = "[script-goal] " + String(err2 && err2.message ? err2.message : err2);
        }
    }

    if (primaryErr === null && goalErr === null) {
        __out("SYNTAX OK " + entry.name);
    } else {
        failures++;
        __out("SYNTAX FAIL " + entry.name + " " + (primaryErr !== null ? primaryErr : goalErr));
    }
}

if (checked === 0) {
    // An empty manifest must never read as success -- that is the vacuous pass
    // this whole gate exists to avoid.
    __out("SYNTAX FAIL (manifest) no regions were listed");
    failures++;
}

__out(failures === 0 ? "SYNTAX_RESULT PASS" : "SYNTAX_RESULT FAIL " + failures);

// osascript echoes the script's last expression value to stdout; keep it empty.
undefined;
