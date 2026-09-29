# Exact embedded web assets

`espnow-core.js` is the unchanged, synchronous script formerly embedded in
`WebPage_ESPNow.h`. It is served from flash at `/assets/espnow-core.js` with gzip
content encoding. The browser decompresses it; the ESP requires no decompressor,
filesystem file, or complete response allocation. Script order remains unchanged.
The static route contains program code only, as with the existing icon route.
Pages and API operations retain their existing authentication and authorization.

Edit the readable JavaScript, then run from the repository root:

```sh
python3 components/hardwareone/web_assets/generate.py
python3 components/hardwareone/web_assets/generate.py --check
```

Commit the source and `WebAssets_Generated.h` together. CMake checks every build
without rewriting the checkout; Arduino or other builds can use the committed
header and run the same check in their build pipeline. The generator uses only
Python's standard library. It performs **no JavaScript minification or rewrite**.
The gzip header has a fixed timestamp and platform byte. The check verifies exact
decompression and both recorded SHA-256 hashes, without requiring different host
zlib versions to produce identical compressed streams.

The existing web syntax gate resolves this allowlisted local script in its
original position. Unknown external script references fail loudly. Run:

```sh
python3 -m unittest tools.webui.tests.test_web_assets
python3 -m unittest discover -s tools/webui/tests -t .
```

The fixed URL uses `Cache-Control: no-store` to prevent a previous firmware's
script being reused after an update. Current browsers advertise gzip support;
clients explicitly excluding gzip receive HTTP 406. The generated response is
17,787 bytes for 82,734 bytes of exact JavaScript (64,947 bytes saved before the
small HTTP handler and script-tag overhead).
