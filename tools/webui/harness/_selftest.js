/*
 * _selftest.js - proves the _bootstrap.js contract on whatever engine is running.
 *
 * Every harness in this directory depends on three globals behaving identically
 * across engines that genuinely disagree about scope, argument passing and where
 * output goes. This exercises all three and prints a fixed, engine-independent
 * transcript, so tests/test_js_engine.py can compare engines byte for byte.
 *
 * Argument: a path to read back, to prove slurp() works under a sandbox.
 */
__out("ENGINE_CONTRACT begin");
__out("argv " + __argv.length);
for (var i = 0; i < __argv.length; i++) __out("arg" + i + " " + (__argv[i] !== "" ? "nonempty" : "empty"));
var text = __argv.length > 0 ? slurp(__argv[0]) : "";
__out("slurp " + (text.indexOf("marker-line") >= 0 ? "ok" : "BAD"));
// The property that actually matters is that a new Function body is SLOPPY on
// every engine — that is what lets browser-shaped page code run identically
// under deno's module goal and jsc's script goal. Assert it behaviourally: a
// bare assignment to an undeclared name throws in strict mode and creates a
// global in sloppy mode. The previous form asked `typeof(...) === "string"`,
// which is true of every value and so could not fail.
var sloppyOk = false;
try { new Function("__sloppyProbe = 1")(); sloppyOk = true; } catch (e) { sloppyOk = false; }
__out("sloppy " + (sloppyOk ? "ok" : "BAD"));
__out("ENGINE_CONTRACT end");

// osascript echoes the last expression value to stdout; keep it empty.
undefined;
