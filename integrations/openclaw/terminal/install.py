#!/usr/bin/env python3
"""Install the pinned official server privately; never start it or run npm scripts."""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import tarfile
import io
import urllib.request

VERSION = '0.10.5'
URL = 'https://registry.npmjs.org/@evenrealities/even-terminal/-/even-terminal-0.10.5.tgz'
INTEGRITY = 'sha512-/XJ3HdoKowsbQDFFiaCmzMWi/0Ghu+iayP7HVX7oNaHRxZmxPGc4PB+xIdtMhfvWW6e/6M1fPUtvwwVCzCU5zw=='
HASHES = {
    'package.json': '3951177bf5b7c4a50ecedd8cd59d8ebfd84b139d1538c50bf0542ee992943f68',
    'dist/session.js': '696a6c3ff1b6a92b3544c5c3bd30ced4ff9de97d7041ff8cc1f1449442cad4d4',
    'dist/routes/core.js': '9c70a3319b30b074bccc258eec08dca29b334076dd5fdd9e3620fdc4260b7119',
    'dist/index.js': '5a6aa912e04eeb54c78109b926a3b1c0f9ff6b4aae1f4877159ea22503e629cb',
}

def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError('Official source changed; refusing an unreviewed patch')
    return text.replace(old, new, 1)

def patched_sources(package):
    sources = {}
    for name, digest in HASHES.items():
        raw = (package / name).read_bytes()
        if hashlib.sha256(raw).hexdigest() != digest:
            raise ValueError(f'Official source integrity mismatch: {name}')
        sources[name] = raw.decode()
    sources.pop('package.json')
    sources['dist/session.js'] = replace_once(sources['dist/session.js'],
        '["claude", "claude-sync", "codex"]', '["claude", "claude-sync", "codex", "openclaw"]')
    core = sources['dist/routes/core.js']
    core = 'import { createOpenClawProvider } from "../hardwareone/provider.mjs";\nimport { createDeviceApi } from "../hardwareone/device-api.mjs";\nconst hw1Only = process.env.HW1_OPENCLAW_ONLY === "1";\n' + core
    for function, module in [('createClaudeProvider', '../claude/provider.js'),
                             ('createClaudeSyncProvider', '../claude-sync/provider.js'),
                             ('createCodexProvider', '../codex/provider.js'),
                             ('ClaudeSyncTransport', '../claude-sync/transport.js'),
                             ('CodexAppServerClient', '../codex/app-server.js')]:
        core = replace_once(core, f'import {{ {function} }} from "{module}";',
            f'const {{ {function} }} = hw1Only ? {{}} : await import("{module}");')
    core = replace_once(core, 'const claudeProvider = createClaudeProvider(emit);', 'const claudeProvider = hw1Only ? null : createClaudeProvider(emit);')
    core = replace_once(core, 'const claudeSyncProvider = createClaudeSyncProvider(emit, claudeSyncTransport);', 'const claudeSyncProvider = hw1Only ? null : createClaudeSyncProvider(emit, claudeSyncTransport);')
    core = replace_once(core, 'const codexProvider = createCodexProvider(emit, () => codexClient);', 'const codexProvider = hw1Only ? null : createCodexProvider(emit, () => codexClient);\nconst openclawProvider = hw1Only ? createOpenClawProvider(emit) : null;')
    core = replace_once(core, 'const codexClient = new CodexAppServerClient(`ws://127.0.0.1:${CODEX_APP_SERVER_PORT}`);', 'const codexClient = hw1Only ? null : new CodexAppServerClient(`ws://127.0.0.1:${CODEX_APP_SERVER_PORT}`);')
    core = replace_once(core, 'const claudeSyncTransport = new ClaudeSyncTransport(emit);', 'const claudeSyncTransport = hw1Only ? null : new ClaudeSyncTransport(emit);')
    core = replace_once(core, 'const providerRegistry = {', 'const providerRegistry = {\n    openclaw: openclawProvider,')
    core = replace_once(core, 'router.use((req, res, next) => {', '''router.use((req, res, next) => {
    if (hw1Only && [req.query.provider, req.body?.provider].some(p => p !== undefined && p !== "openclaw")) {
        res.status(400).json({error: "Only the OpenClaw provider is enabled"}); return;
    }''')
    core = replace_once(core, '// GET /api/sessions — list resumable sessions', '''if (hw1Only) {
    const deviceApi = createDeviceApi(openclawProvider);
    router.post("/hardwareone", async (req, res) => {
        try { res.json(await deviceApi(req.body)); }
        catch { res.status(400).json({error: "Terminal operation failed; inspect host status"}); }
    });
}
// GET /api/sessions — list resumable sessions''')
    core = replace_once(core, 'console.log(`[prompt] sessionId=', 'if (!hw1Only) console.log(`[prompt] sessionId=')
    core = replace_once(core, 'res.status(202).json({ ok: true, sessionId: result.sessionId, provider: result.provider });',
                        'res.status(202).json({ ok: true, ...result });')
    # Existing handlers are synchronous because their original SDK adapters are.
    # Await the Gateway verdict, so official clients cannot see a false success.
    for route in ['permission-response', 'question-response', 'interrupt']:
        core = replace_once(core, f'router.post("/{route}", (req, res) => {{', f'router.post("/{route}", async (req, res) => {{')
    core = replace_once(core, 'const accepted = targetProvider.respondPermission(sessionId, decision || "deny");', 'const accepted = await targetProvider.respondPermission(sessionId, decision || "deny", req.body?.requestId);')
    core = replace_once(core, 'targetProvider.respondQuestion(sessionId, answer || "skip");', '''const accepted = await targetProvider.respondQuestion(sessionId, answer || "skip", req.body?.requestId);
    if (accepted === false) { res.status(400).json({error: "Question is no longer pending or answer was not offered"}); return; }''')
    core = replace_once(core, 'targetProvider.interrupt(sessionId);', 'await targetProvider.interrupt(sessionId);')
    core = replace_once(core, 'console.log(`[question-response]', 'if (!hw1Only) console.log(`[question-response]')
    sources['dist/routes/core.js'] = core
    index = sources['dist/index.js']
    index = replace_once(index, 'const HOST = resolveHost();', 'const HW1_ONLY = process.env.HW1_OPENCLAW_ONLY === "1";\nconst HOST = HW1_ONLY ? {label: "Local", address: "127.0.0.1"} : resolveHost();')
    index = replace_once(index, 'const app = express();', '''const app = express();
if (HW1_ONLY) app.use((req, res, next) => {
    const allowed = req.method === "GET" && /^\\/api\\/(info|sessions|status|messages|events|sessions\\/hwo_[a-f0-9]{64}\\/history)$/.test(req.path)
        || req.method === "POST" && /^\\/api\\/(prompt|interrupt|permission-response|question-response|hardwareone)$/.test(req.path);
    if (!allowed) { res.status(404).json({error: "Not enabled by Hardware One"}); return; }
    if (req.headers.origin || req.query.token) { res.status(403).json({error: "Use the local authenticated bridge"}); return; }
    next();
});''')
    index = replace_once(index, 'app.use(express.json({ limit: "10mb" }));', 'app.use(express.json({ limit: HW1_ONLY ? "16kb" : "10mb" }));')
    index = replace_once(index, 'app.use("/api", auth, coreRouter);', '''app.use("/api", auth, coreRouter);
if (HW1_ONLY) app.use((err, req, res, next) => {
    if (res.headersSent) { res.end(); return; }
    const status = err.type === "entity.too.large" ? 413 : Number.isInteger(err.statusCode) && err.statusCode >= 400 && err.statusCode <= 599 ? err.statusCode : 400;
    res.status(status).json({error: "Terminal request failed; inspect host status before retrying an action"});
});''')
    index = replace_once(index, 'claudeSyncTransport.attach(httpServer);', 'if (!HW1_ONLY) claudeSyncTransport.attach(httpServer);')
    index = replace_once(index, 'claudeSyncTransport.attach(loopbackServer);', 'if (!HW1_ONLY) claudeSyncTransport.attach(loopbackServer);')
    index = replace_once(index, 'printServerBanner(PORT, TOKEN, process.env.PROJECT_DIR || process.cwd(), true, HOST, true);',
        'if (HW1_ONLY) console.log(`Hardware One / Even Terminal listening on http://127.0.0.1:${PORT}; token is read from a private file.`);\n    else printServerBanner(PORT, TOKEN, process.env.PROJECT_DIR || process.cwd(), true, HOST, true);')
    index = replace_once(index, 'writeInstancePidfile({', 'if (!HW1_ONLY) writeInstancePidfile({')
    index = replace_once(index, 'startExposeProvider(PORT, TOKEN);', 'if (!HW1_ONLY) startExposeProvider(PORT, TOKEN);')
    index = replace_once(index, 'console.error(`[server] UNCAUGHT EXCEPTION: ${err.message}\\n${err.stack}`);',
        'console.error(HW1_ONLY ? "[server] Unhandled local host error" : `[server] UNCAUGHT EXCEPTION: ${err.message}\\n${err.stack}`);')
    index = replace_once(index, 'console.error(`[server] UNHANDLED REJECTION: ${reason}`);',
        'console.error(HW1_ONLY ? "[server] Unhandled local host rejection" : `[server] UNHANDLED REJECTION: ${reason}`);')
    sources['dist/index.js'] = index
    return sources

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, default=Path.home()/'.local/share/hardwareone-even-terminal')
    parser.add_argument('--tarball', type=Path, help='Already downloaded official 0.10.5 tarball (still verified)')
    args = parser.parse_args()
    target = args.directory.expanduser().resolve()
    if target.exists(): parser.error('Install directory already exists; preserve its config before choosing a new empty directory')
    npm, node = shutil.which('npm'), shutil.which('node')
    if not npm or not node: parser.error('Node.js 22+ and npm are required')
    try:
        major = int(subprocess.check_output([node, '-p', 'process.versions.node.split(".")[0]'], text=True).strip())
    except (ValueError, OSError, subprocess.SubprocessError): parser.error('Unable to verify Node.js version')
    if major < 22: parser.error('Node.js 22 or newer is required')
    os.umask(0o077)
    target.parent.mkdir(parents=True, exist_ok=True)
    # A failed download, npm operation or source check removes only our staging
    # directory. Existing installations are never used as staging or overwritten.
    with tempfile.TemporaryDirectory(prefix='.'+target.name+'-install-', dir=target.parent) as tmp:
        staged = Path(tmp)/'install'
        staged.mkdir(mode=0o700)
        if args.tarball:
            if args.tarball.stat().st_size > 2*1024*1024: parser.error('Tarball too large')
            raw = args.tarball.read_bytes()
        else:
            with urllib.request.urlopen(URL, timeout=30) as response: raw = response.read(2*1024*1024+1)
        if len(raw) > 2*1024*1024 or 'sha512-'+base64.b64encode(hashlib.sha512(raw).digest()).decode() != INTEGRITY:
            parser.error('Official package integrity check failed')
        package = staged/'official'
        # The official tarball is verified first. Still reject links, traversal,
        # exceptional member types and unexpected expansion before writing it.
        with tarfile.open(fileobj=io.BytesIO(raw), mode='r:gz') as archive:
            members = archive.getmembers()
            if len(members) > 256 or sum(m.size for m in members) > 2*1024*1024:
                parser.error('Unexpected official package layout')
            for member in members:
                parts = Path(member.name).parts
                if not parts or parts[0] != 'package' or '..' in parts or not (member.isfile() or member.isdir()):
                    parser.error('Unsafe package member')
            for member in members:
                out = package.joinpath(*Path(member.name).parts[1:])
                if member.isdir(): out.mkdir(parents=True, exist_ok=True)
                else:
                    out.parent.mkdir(parents=True, exist_ok=True)
                    out.write_bytes(archive.extractfile(member).read())
        changed = patched_sources(package)  # Verify every input before installing dependencies.
        extension = package/'dist/hardwareone'; extension.mkdir()
        extension_hashes = {}
        for name in ['provider.mjs', 'gateway.mjs', 'device-api.mjs']:
            source = Path(__file__).with_name(name)
            shutil.copyfile(source, extension/name)
            extension_hashes[name] = hashlib.sha256(source.read_bytes()).hexdigest()
        for name, content in changed.items(): (package/name).write_text(content)
        shutil.copyfile(Path(__file__).with_name('start.mjs'), staged/'start.mjs')
        (staged/'package.json').write_text(json.dumps({'private': True, 'name': 'hardwareone-even-terminal', 'version': '0.1.0',
            'dependencies': {'express':'5.2.1','cors':'2.8.6','qrcode-terminal':'0.12.0'}}))
        subprocess.run([npm, 'install', '--ignore-scripts', '--no-audit', '--no-fund', '--no-update-notifier'], cwd=staged, check=True)
        (staged/'provenance.json').write_text(json.dumps({'officialVersion': VERSION, 'tarball': URL, 'integrity': INTEGRITY,
            'patchedInputs': HASHES, 'extensionSha256': extension_hashes,
            'packageLockSha256': hashlib.sha256((staged/'package-lock.json').read_bytes()).hexdigest()}, indent=2)+'\n')
        target.mkdir(mode=0o700)  # Reserve the exact unused destination atomically.
        try: os.replace(staged, target)
        except BaseException:
            target.rmdir()  # Only our still-empty reservation; never recursive cleanup.
            raise
    print(f'Installed official Even Terminal {VERSION} with the Hardware One provider in {target}.')
    print('No server was started. Run node start.mjs --init from that directory to create private configuration.')

if __name__ == '__main__': main()
