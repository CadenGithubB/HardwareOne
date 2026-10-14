// Deliberately bypasses the official CLI's expose/update/account setup paths.
import { existsSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { homedir } from 'node:os';
import { randomBytes } from 'node:crypto';
import { createIdentity, readPrivateToken, Gateway } from './official/dist/hardwareone/gateway.mjs';
const root = dirname(fileURLToPath(import.meta.url));
const configPath = join(root, 'hardwareone.json');
process.umask(0o077);
const args = process.argv.slice(2);
if (args.length > 1 || args.some(a => !['--init', '--check'].includes(a))) throw new Error('Usage: node start.mjs [--init|--check]');
if (process.argv.includes('--init')) {
  if (existsSync(configPath)) throw new Error('Configuration exists; refusing to overwrite');
  const identityFile = join(root, 'gateway-identity.pem'), bridgeTokenFile = join(root, 'bridge-token.txt');
  if (!existsSync(identityFile)) createIdentity(identityFile);
  if (!existsSync(bridgeTokenFile)) writeFileSync(bridgeTokenFile, randomBytes(32).toString('hex')+'\n', {flag:'wx',mode:0o600});
  writeFileSync(configPath, JSON.stringify({ port: 3457, gatewayPort: 18789,
    gatewayTokenFile: join(homedir(), '.openclaw/hardwareone-relay/gateway-token.txt'),
    identityFile, bridgeTokenFile, approvals: true }, null, 2)+'\n', {flag:'wx',mode:0o600});
  console.log(`Created private configuration: ${configPath}. Review paths, then run node start.mjs. Gateway pairing may be required.`);
} else {
  const c = JSON.parse(readFileSync(configPath, 'utf8'));
  if (!Number.isInteger(c.port) || c.port < 1024 || c.port > 65535 || typeof c.approvals !== 'boolean') throw new Error('Invalid local server configuration');
  for (const name of ['EVEN_TERMINAL_EXPOSE_PROVIDER', 'EVEN_HOST_MODE', 'EVEN_HOST_INTERFACE', 'EVEN_ALLOW_CORS',
    'PROJECT_DIR', 'VERBOSE', 'DEBUG', 'NODE_USE_ENV_PROXY']) delete process.env[name];
  process.env.HW1_OPENCLAW_ONLY = '1'; process.env.DEFAULT_PROVIDER = 'openclaw';
  process.env.PORT = String(c.port); process.env.BRIDGE_TOKEN = readPrivateToken(c.bridgeTokenFile);
  process.env.HW1_GATEWAY_CONFIG = JSON.stringify({ tokenFile: c.gatewayTokenFile, identityFile: c.identityFile,
    approvals: c.approvals, port: c.gatewayPort });
  process.chdir(root);
  if (args.includes('--check')) {
    const gateway = new Gateway(JSON.parse(process.env.HW1_GATEWAY_CONFIG));
    if (gateway.identity) console.log(`Hardware One Terminal device ID: ${gateway.identity.id}`);
    try {
      const hello = await gateway.connect();
      console.log(`Gateway ${hello.server.version} connected. Granted scopes: ${(hello.auth?.scopes || []).join(', ')}`);
    } catch (e) {
      console.error(e.message);
      console.error('If pairing is required: run openclaw devices list, verify this device ID and requested scopes, then approve its exact request ID.');
      process.exitCode = 1;
    } finally { gateway.close(); }
  } else await import('./official/dist/index.js');
}
