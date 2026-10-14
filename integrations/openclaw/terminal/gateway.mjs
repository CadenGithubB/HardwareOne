// OpenClaw 2026.9.4 (3a9d69d), protocol 4. Only a local authenticated backend.
// Optional persistent paired identity for offered approvals/questions. No shell,
// administrator scope, automatic approval, pairing bypass or send retry.
import { randomUUID, createHash, createPrivateKey, createPublicKey, generateKeyPairSync, sign } from 'node:crypto';
import { readFileSync, statSync, writeFileSync } from 'node:fs';

export function createIdentity(path) {
  const { privateKey } = generateKeyPairSync('ed25519');
  writeFileSync(path, privateKey.export({ type: 'pkcs8', format: 'pem' }), { flag: 'wx', mode: 0o600 });
}
function loadIdentity(path) {
  const st = statSync(path);
  if (!st.isFile() || st.size > 4096 || (st.mode & 0o077)) throw new Error('Identity file must be private (chmod 600)');
  const key = createPrivateKey(readFileSync(path));
  if (key.asymmetricKeyType !== 'ed25519') throw new Error('An Ed25519 identity is required');
  const raw = Buffer.from(createPublicKey(key).export({ format: 'jwk' }).x, 'base64url');
  return { key, publicKey: raw.toString('base64url'), id: createHash('sha256').update(raw).digest('hex') };
}

export function readPrivateToken(path) {
  if (typeof path !== 'string' || !path.startsWith('/')) throw new Error('An absolute token file path is required');
  const st = statSync(path);
  if (!st.isFile() || st.size > 4096 || (st.mode & 0o077)) throw new Error('Token file must be private (chmod 600) and at most 4096 bytes');
  const token = readFileSync(path, 'utf8').trim();
  if (!token || /[\s\x00-\x1f\x7f]/u.test(token)) throw new Error('Invalid token file');
  return token;
}

const METHODS = new Set(['sessions.list', 'chat.history', 'chat.send', 'chat.abort',
  'sessions.subscribe', 'sessions.unsubscribe', 'sessions.messages.subscribe', 'sessions.messages.unsubscribe',
  'approval.get', 'approval.resolve', 'question.list', 'question.resolve']);
export class Gateway {
  constructor({ tokenFile, identityFile, approvals = true, port = 18789, Socket = globalThis.WebSocket } = {}) {
    if (!Number.isInteger(port) || port < 1 || port > 65535) throw new Error('Invalid Gateway port');
    if (!Socket) throw new Error('Node.js 22 or newer is required');
    this.url = `ws://127.0.0.1:${port}`;
    this.tokenFile = tokenFile; this.Socket = Socket;
    this.identity = approvals ? loadIdentity(identityFile) : null;
    this.scopes = ['operator.read', 'operator.write', ...(approvals ? ['operator.approvals', 'operator.questions'] : [])];
    this.pending = new Map(); this.listeners = new Set(); this.disconnectListeners = new Set();
    this.socket = null; this.connecting = null; this.hello = null;
  }
  onEvent(fn) { this.listeners.add(fn); return () => this.listeners.delete(fn); }
  onDisconnect(fn) { this.disconnectListeners.add(fn); return () => this.disconnectListeners.delete(fn); }
  async connect() {
    if (this.hello && this.socket?.readyState === 1) return this.hello;
    if (this.connecting) return this.connecting;
    const token = readPrivateToken(this.tokenFile);
    this.connecting = new Promise((resolve, reject) => {
      const socket = this.socket = new this.Socket(this.url);
      let challenged = false, settled = false;
      const timer = setTimeout(() => fail(new Error('Gateway handshake timed out')), 10000);
      const fail = (error) => {
        clearTimeout(timer);
        if (!settled) { settled = true; reject(error); }
        if (this.socket !== socket) return;
        const hadHello = Boolean(this.hello);
        this.hello = null; this.socket = null;
        for (const p of this.pending.values()) { clearTimeout(p.timer); p.reject(error); }
        this.pending.clear();
        socket.close();
        if (hadHello) for (const listener of this.disconnectListeners) listener();
      };
      socket.addEventListener('message', (event) => {
        if (this.socket !== socket) return;
        if (typeof event.data !== 'string' || Buffer.byteLength(event.data) > 2 * 1024 * 1024) {
          fail(new Error('Invalid or oversized Gateway frame')); return;
        }
        let frame;
        try { frame = JSON.parse(event.data); } catch { fail(new Error('Invalid Gateway JSON')); return; }
        if (frame?.type === 'event' && frame.event === 'connect.challenge' && !challenged) {
          if (typeof frame.payload?.nonce !== 'string' || !frame.payload.nonce) {
            fail(new Error('Invalid Gateway challenge')); return;
          }
          challenged = true;
          const clientMode = this.identity ? 'ui' : 'backend';
          let device;
          if (this.identity) {
            const signedAt = frame.payload.ts;
            if (!Number.isSafeInteger(signedAt) || signedAt <= 0) { fail(new Error('Invalid challenge timestamp')); return; }
            const payload = ['v3', this.identity.id, 'gateway-client', clientMode, 'operator',
              this.scopes.join(','), String(signedAt), token, frame.payload.nonce, process.platform, ''].join('|');
            device = { id: this.identity.id, publicKey: this.identity.publicKey,
              signature: sign(null, Buffer.from(payload), this.identity.key).toString('base64url'),
              signedAt, nonce: frame.payload.nonce };
          }
          this._request('connect', {
            minProtocol: 4, maxProtocol: 4,
            client: { id: 'gateway-client', mode: clientMode, version: '0.1.0',
              platform: process.platform, displayName: 'Hardware One Terminal' },
            role: 'operator', scopes: this.scopes, caps: ['session-scoped-events', 'tool-events'],
            auth: { token }, ...(device ? { device } : {}),
          }).then((hello) => {
            if (this.socket !== socket) return;
            if (hello?.type !== 'hello-ok' || hello.protocol !== 4 ||
                !/^2026\.9\.4(?:$|[-+])/u.test(hello.server?.version ?? '')) {
              fail(new Error('Unsupported Gateway version; expected 2026.9.4 protocol 4')); return;
            }
            this.hello = hello; clearTimeout(timer); settled = true; resolve(hello);
          }, fail);
          return;
        }
        if (frame?.type === 'res' && typeof frame.id === 'string') {
          const p = this.pending.get(frame.id);
          if (!p) return;
          this.pending.delete(frame.id); clearTimeout(p.timer);
          if (frame.ok === true) p.resolve(frame.payload);
          else p.reject(Object.assign(new Error(`Gateway request rejected (${String(frame.error?.code ?? 'unknown').slice(0, 64)})`), { gatewayCode: frame.error?.code }));
        } else if (this.hello && frame?.type === 'event' && typeof frame.event === 'string') {
          for (const listener of this.listeners) listener(frame);
        }
      });
      socket.addEventListener('error', () => fail(new Error('Gateway connection failed')));
      socket.addEventListener('close', () => {
        fail(new Error('Gateway disconnected; a submitted run may continue'));
      });
    }).finally(() => { this.connecting = null; });
    return this.connecting;
  }
  _request(method, params) {
    if (this.pending.size >= 16 || this.socket?.readyState !== 1) return Promise.reject(new Error('Gateway unavailable or busy'));
    const id = randomUUID();
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error('Gateway request timed out; do not automatically resend'));
      }, 15000);
      this.pending.set(id, { resolve, reject, timer });
      try { this.socket.send(JSON.stringify({ type: 'req', id, method, params })); }
      catch { this.pending.delete(id); clearTimeout(timer); reject(new Error('Gateway send failed; outcome uncertain')); }
    });
  }
  async request(method, params) {
    if (!METHODS.has(method)) throw new Error('Unsupported Gateway operation');
    await this.connect();
    if (!this.hello.features?.methods?.includes(method)) throw new Error('Gateway does not offer this operation');
    return this._request(method, params);
  }
  close() { this.socket?.close(); }
}
