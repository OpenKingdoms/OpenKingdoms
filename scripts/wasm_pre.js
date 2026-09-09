/* wasm_pre.js — Emscripten pre-js for the TAK-RE browser build.
 *
 * - Ensures /game exists before main() (HPI archives + Music/ are
 *   preloaded there by the CMake EMSCRIPTEN block).
 * - Caps stderr forwarding: the engine logs gameplay events to stderr,
 *   and console.error per line makes the tab progressively slower as
 *   the console buffer grows. Keep the first 300 lines (boot
 *   diagnostics), then drop the rest. Add ?verbose to the URL to keep
 *   everything. */
Module['preRun'] = Module['preRun'] || [];
Module['preRun'].push(function () {
    try {
        FS.mkdir('/game');
    } catch (e) {
        /* already exists — fine */
    }
});

(function () {
    var verbose = /[?&]verbose/.test(location.search);
    var budget = 300;
    var warned = false;
    var realErr = console.error.bind(console);
    Module['printErr'] = function (text) {
        if (verbose || budget-- > 0) { realErr(text); return; }
        if (!warned) {
            warned = true;
            realErr('[tak-re] stderr muted after 300 lines (add ?verbose to keep)');
        }
    };
})();
