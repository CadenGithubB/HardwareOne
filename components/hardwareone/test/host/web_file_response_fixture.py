"""Synthetic manual browser fixture; never contacts a device or runs CLI commands."""
from __future__ import annotations

import html
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
from urllib.parse import parse_qs, urlparse
import wave

SVG = '''<svg xmlns="http://www.w3.org/2000/svg" width="700" height="200"
 onload="fetch('/probe/event?case=CASE',{method:'POST'})">
<style>text { fill: white; font: 20px sans-serif; }</style>
<defs><rect id="shape" width="680" height="180" rx="12" fill="#13593a"/></defs>
<use href="#shape" x="10" y="10"/>
<text id="result" x="25" y="45">Static SVG preview</text>
<text x="25" y="80">CASE</text>
<foreignObject x="25" y="100" width="620" height="60">
<div xmlns="http://www.w3.org/1999/xhtml">
<button onclick="fetch('/probe/click?case=CASE',{method:'POST'})">Event handler probe</button>
</div></foreignObject>
<script><![CDATA[
(async()=>{
 const r=await fetch('/probe/read?case=CASE');
 const p=await fetch('/probe/write?case=CASE',{method:'POST',body:'synthetic-only'});
 document.getElementById('result').textContent='Script ran: '+r.status+' / '+p.status;
})();
]]></script></svg>'''


def serve(policies: dict[str, dict[str, str]], port: int) -> None:
    headers = policies["0"]
    svg_headers = policies["1"]
    events = []
    audio = io.BytesIO()
    with wave.open(audio, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(8000)
        wav.writeframes(b"\0\0" * 16000)
    wav_bytes = audio.getvalue()

    class Handler(BaseHTTPRequestHandler):
        def send(self, body, mime="text/html; charset=utf-8", extra=None, status=200):
            body = body.encode() if isinstance(body, str) else body
            self.send_response(status)
            self.send_header("Content-Type", mime)
            self.send_header("Content-Length", str(len(body)))
            for key, value in (extra or {}).items():
                self.send_header(key, value)
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            path = urlparse(self.path)
            case = parse_qs(path.query).get("case", [""])[0]
            if path.path == "/":
                self.send('''<h1>File preview policy fixture</h1>
<p>Synthetic data only. The production header helper supplies the restrictions.</p>
<p><a href="/view.svg?case=baseline">Unprotected baseline</a></p>
<p><a href="/view.svg?case=protected">Protected SVG</a></p>
<p><a href="/view.svg?case=raw&amp;mode=raw">Protected raw SVG</a></p>
<p><a href="/pretty">Escaped text preview</a></p>
<p><a href="/audio.wav">Native audio preview</a></p>
<p><a href="/download">Binary download</a></p>
<p><a href="/results">Recorded probes</a></p>
<p id="trusted">Trusted page script pending</p>
<script>fetch('/read').then(r=>r.text()).then(t=>{
document.getElementById('trusted').textContent='Trusted page script and file fetch: '+t;
});</script>''', extra={"Set-Cookie": "session=SYNTHETIC; Path=/; HttpOnly; SameSite=Strict"})
            elif path.path == "/view.svg":
                self.send(SVG.replace("CASE", case), "image/svg+xml",
                          {} if case == "baseline" else svg_headers)
            elif path.path == "/pretty":
                self.send("<style>body{color:#13593a}</style><h1>Escaped file</h1><pre>"
                          + html.escape(SVG.replace("CASE", "text"))
                          + '</pre><a href="/read">Raw</a>'
                          + '<p><a href="/navigation-check">Authenticated navigation check</a></p>',
                          extra=headers)
            elif path.path == "/navigation-check":
                authenticated = "session=SYNTHETIC" in self.headers.get("Cookie", "")
                self.send("<p>Navigation authenticated: " + str(authenticated) + "</p>")
            elif path.path == "/read":
                self.send("safe file contents", "text/plain; charset=utf-8", headers)
            elif path.path == "/audio.wav":
                self.send(wav_bytes, "audio/wav", headers)
            elif path.path == "/download":
                self.send(b"synthetic download\n", "application/octet-stream",
                          {**headers, "Content-Disposition": 'attachment; filename="fixture.hwmap"'})
            elif path.path == "/results":
                self.send("<h1>Recorded probes</h1><pre>" + html.escape(json.dumps(events)) + "</pre>")
            elif path.path.startswith("/probe/"):
                self.probe(case)
            else:
                self.send("missing", "text/plain", status=404)

        def probe(self, case):
            authenticated = "session=SYNTHETIC" in self.headers.get("Cookie", "")
            events.append({"case": case, "method": self.command,
                           "path": urlparse(self.path).path, "authenticated": authenticated})
            self.send("SYNTHETIC DATA" if authenticated else "DENIED", "text/plain",
                      status=200 if authenticated else 401)

        def do_POST(self):
            self.rfile.read(int(self.headers.get("Content-Length", "0")))
            path = urlparse(self.path)
            if path.path.startswith("/probe/"):
                self.probe(parse_qs(path.query).get("case", [""])[0])
            else:
                self.send("missing", "text/plain", status=404)

    print(f"Browser fixture: http://127.0.0.1:{port}/ (Ctrl-C to stop)", flush=True)
    # Browsers preconnect; one idle socket must not stall other fixture tabs.
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
