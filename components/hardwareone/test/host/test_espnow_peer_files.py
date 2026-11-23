#!/usr/bin/env python3
"""'espnowunpair' removes what this device kept about the peer, and no more.

After an unpair nothing stays under /system/espnow/peers/<MAC>/ (identity, the
bond caches settings.json and schema.json, the manifest reference, anything
else) and the folder goes; the counted files and sizes are what the reply
reports. The cached command manifest /system/manifests/<fwHash>.json is keyed
by firmware, so each peer that delivered one records its hash in
peers/<MAC>/manifest.hash: an unpair removes the manifest unless another peer
folder still names it (or the folders cannot be listed), and a peer that
delivers a manifest for new firmware drops the one it named before on the same
rule, which bounds the cache to one file per peer. A manifest the bond sync finds
already cached (a second peer on the same firmware, or one cached before the
references) is recorded for that peer as well. Files received from the
peer (/espnow/received/<MAC>/) stay. A file that will not go leaves the folder
reported (and an identity.json that stays leaves the identity cache marked
incomplete). The reply line names what went, what stayed and where received
files are, and never overruns its buffer.

Compiled: peerFilesRemove and its helpers from System_ESPNow_Identity.cpp, the
manifest reference, the bond sync's cache lookup and the reply line from
System_ESPNow.cpp, the cache path
helpers from System_Utils.h, run on the shared in-memory filesystem
(fake_vfs.h) by espnow_peer_files_harness.cpp. Source checks (counted with the
harness's): the unpair teardown (espnowUnpairTeardown, shared by espnowunpair
and espnowrevoke) reads the reference before the folder goes and drops the
manifest after, saves the registry before it deletes the identity (write
order), clears every other store (pins, sensor leases, pairing state, slots),
and cmd_espnow_unpair puts the line in its reply; espnowforget is gone;
espnowrevoke writes the revoked list before the teardown; the bond sync records the reference for a manifest that reached
the cache and looks cached manifests up only through manifestCacheHit, which
records one it finds; the unpair reply's identity flag comes from what
peerIdentityForget removed; an unpair of the bond partner (the partner setting
itself, not isBondPeerMac, which answers false during a partner change) ends
the bond before the peer is deleted, and the unpair calls off this device's
key exchange with the peer and the frames queued for it before the identity
and the folder go, so nothing under way brings them back (finding X4, round
3; test_espnow_bond_partner_change runs the bond side, test_espnow_handshake_
backoff and test_espnow_keyex_hello the calls).

--source-dir <directory> reads the sources from another tree; a tree without
one of the pieces compiles with a stand-in for it that does what its unpair
did, and fails the checks that need it.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent

# Each marker stops before the opening brace: extract_block takes the first "{" after it.
UTILS = ('inline int peerCacheDir(', 'inline int peerCachePath(')
IDENTITY = ('int findSlotByMac(', 'bool buildPeerPath(', 'bool peerIdentityForget(')
PEER_FILES = ('void removeFolderContents(', 'bool peerFilesRemove(')
MANIFEST = ('static void manifestHashToHex(', 'static bool manifestRefReadPath(', 'static bool manifestRefRead(',
            'static bool manifestHashInUse(', 'static uint32_t manifestDropIfUnused(',
            'static void manifestCacheRecordUse(', 'static bool manifestCachedFor(', 'static bool manifestCacheHit(')
FILES_LINE = ('static void appendUnpairText(', 'static void formatStoredSize(', 'static void formatUnpairFilesLine(')
CONSTANTS = r'^constexpr (?:const char\*|uint8_t)\s+k(?:PeerSlots|PeersDir)\s*=.*;$'
CACHE = r'^(?:EXT_RAM_BSS_ATTR )?PeerIdentity gPeerIdentities\[kPeerSlots\]\s*=.*;$'
FLAG = r'^(?:static )?bool gPeerIdentitiesLoaded\s*=.*;$'
REF_NAME = r'^static const char\* const kManifestRefName = .*;$'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('clang++') or shutil.which('c++'))
    p.add_argument('--sanitize', action='store_true')
    p.add_argument('--component', type=Path, default=HERE.parents[1])
    p.add_argument('--source-dir', type=Path)
    a = p.parse_args()
    if not a.cxx:
        p.error('a host C++17 compiler is required')
    sys.path.insert(0, str(HERE))
    from test_web_batch_handlers import extract_block

    def read(name):
        overlay = a.source_dir / name if a.source_dir else None
        return (overlay if overlay and overlay.exists() else a.component / name).read_text()

    identity = read('System_ESPNow_Identity.cpp')
    identity_h = read('System_ESPNow_Identity.h')
    espnow = read('System_ESPNow.cpp')
    utils = read('System_Utils.h')

    def body(text, marker, where):
        if text.count(marker) != 1:
            sys.exit(f'FAIL extraction: {where}: {marker!r} found {text.count(marker)} times, expected 1')
        return extract_block(text, marker)

    def lines(text, pattern, expected, what):
        found = re.findall(pattern, text, re.M)
        if len(found) != expected:
            sys.exit(f'FAIL extraction: expected {expected} {what}, found {len(found)}')
        return found

    def all_present(text, markers):
        return all(m in text for m in markers)

    have_peer_files = all_present(identity, PEER_FILES) and 'struct PeerFilesRemoved' in identity_h
    have_manifest = all_present(espnow, MANIFEST)
    have_line = all_present(espnow, FILES_LINE)

    types = [body(identity_h, 'struct PeerIdentity', 'System_ESPNow_Identity.h') + ';']
    if have_peer_files:
        types.append(body(identity_h, 'struct PeerFilesRemoved', 'System_ESPNow_Identity.h') + ';')
    globals_ = (lines(identity, CONSTANTS, 2, 'peer constants') + lines(identity, CACHE, 1, 'gPeerIdentities')
                + lines(identity, FLAG, 1, 'gPeerIdentitiesLoaded'))
    inserts = {
        '// INSERT_UTILS': '\n\n'.join(body(utils, m, 'System_Utils.h') for m in UTILS),
        '// INSERT_TYPES': '\n'.join(types),
        '// INSERT_GLOBALS': '\n'.join(globals_),
        # removeFolderContents sits in the source's anonymous namespace; so it does here.
        '// INSERT_IDENTITY': '\n\n'.join(body(identity, m, 'System_ESPNow_Identity.cpp')
                                          for m in IDENTITY + (PEER_FILES[:1] if have_peer_files else ())),
        '// INSERT_PEER_FILES': body(identity, PEER_FILES[1], 'System_ESPNow_Identity.cpp') if have_peer_files else '',
        '// INSERT_MANIFEST': ('\n'.join(lines(espnow, REF_NAME, 1, 'kManifestRefName')) + '\n\n' +
                               '\n\n'.join(body(espnow, m, 'System_ESPNow.cpp') for m in MANIFEST))
                              if have_manifest else '',
        '// INSERT_FILES_LINE': '\n\n'.join(body(espnow, m, 'System_ESPNow.cpp') for m in FILES_LINE)
                                if have_line else '',
    }

    # Source checks: the order inside the command, which a host build cannot run.
    source_failures = []
    command = body(espnow, 'const char* cmd_espnow_unpair(', 'System_ESPNow.cpp')
    # The steps live in the teardown that espnowunpair and espnowrevoke share.
    unpair = body(espnow, 'static bool espnowUnpairTeardown(', 'System_ESPNow.cpp')
    pins = body(espnow, 'static bool espnowUnpairClearPins(', 'System_ESPNow.cpp')
    pairing = body(espnow, 'static void espnowUnpairClearPairing(', 'System_ESPNow.cpp')
    revoke = body(espnow, 'const char* cmd_espnow_revoke(', 'System_ESPNow.cpp')
    tidy = body(espnow, 'static void espnowRevokedBootTidy() ', 'System_ESPNow.cpp')
    init = body(espnow, 'bool initEspNow(', 'System_ESPNow.cpp')
    rollback_queue = body(espnow, 'void espnowPairRollbackRevoked(', 'System_ESPNow.cpp')
    rollback_job = body(espnow, 'static void runDeferredPairRollback(', 'System_ESPNow.cpp')
    pair_request = body(espnow, 'static V4Verdict v4h_pair_request(', 'System_ESPNow.cpp')
    manifest_resp = body(espnow, 'static void processBondModeManifestResp(', 'System_ESPNow.cpp')
    forget_crypto = body(espnow, 'static bool espnowForgetPeerCrypto(', 'System_ESPNow.cpp')

    def at(text, needle):
        return text.find(needle)

    # Every store the unpair teardown clears (1.0 design 5.1, "The unpair
    # teardown is completed"), by the call that clears it.
    stores = ('bondEndPartnership();', 'espnowDeletePeerRuntime(mac)', 'removeEspNowDevice(mac);',
              'saveEspNowDevices()', 'espnowKeyExCancel(mac);', 'pendingFrameDropForPeer(mac);',
              'espnowUnpairClearPairing(mac);', 'espnowUnpairClearPins(mac, peerIdentityFindByMac(mac))',
              'espnowSensorLeasesDropFor(mac)', 'espnowForgetPeerCrypto(mac, &out.hadIdentity)',
              'manifestRefRead(mac,', 'peerFilesRemove(mac,', 'manifestDropIfUnused(', 'meshPeerSlotsRelease(mac);')
    missing = [c for c in stores if c not in unpair]
    pin_parts = ('gSettings.espnowMasterFingerprint, String("")', 'gSettings.espnowBackupMasterFingerprint, String("")',
                 'gSettings.meshMasterMac, String("")', 'gChFollowMasterRank = 0;', 'gChFollowBackupResolved = false;')
    pairing_parts = ('removeFromUnpairedList(mac);', 'gPair.in = espnowpair::Request{};',
                     'gPair.out = espnowpair::Request{};', 'pairingWindowCloseFor(mac);')

    checks = (
        (0 <= at(unpair, 'espnowForgetPeerCrypto(mac') < at(unpair, 'manifestRefRead(mac,') and
         0 <= at(unpair, 'manifestRefRead(mac,') < at(unpair, 'peerFilesRemove(mac,') < at(unpair, 'manifestDropIfUnused('),
         'the unpair teardown forgets the identity, reads the manifest reference, removes the peer folder, '
         'then drops the manifest'),
        (at(unpair, 'formatUnpairFilesLine(') >= 0 and command.count('out.filesLine') == 3,
         'every reply of cmd_espnow_unpair after the removal carries the line about the files'),
        ('const char* cmd_espnow_forget(' not in espnow,
         'espnowforget is gone (ESP-NOW 1.0 step 3): espnowunpair and the boot identity check cover it'),
        (0 <= at(unpair, 'saveEspNowDevices()') < at(unpair, 'espnowForgetPeerCrypto(mac') and
         at(unpair, 'removeEspNowDevice(mac);') < at(unpair, 'saveEspNowDevices()'),
         'write order: the registry is saved before the identity is deleted'),
        (not missing, 'the unpair teardown clears every store (missing: ' + ', '.join(missing) + ')'),
        (all(p in pins for p in pin_parts) and at(unpair, 'espnowUnpairClearPins(') < at(unpair, 'espnowForgetPeerCrypto(mac'),
         'the teardown clears the master and backup pins, meshMasterMac and the channel-follow master '
         'while the identity still holds the key'),
        (all(p in pairing for p in pairing_parts),
         'the teardown clears the candidate list, a pending pair request either way and the re-key window'),
        (0 <= at(revoke, 'espnowRevokedAdd(') < at(revoke, 'espnowUnpairTeardown(mac, out'),
         'espnowrevoke writes the revoked list before any teardown'),
        # Review 2026-11-23, findings 2, 4, 5, 6, 8.
        (revoke.count('espnowUnpairTeardown(mac, out, /*revoking=*/true') == 3 and
         '/*radioUp=*/false' in revoke,
         'review 2: espnowrevoke retries a failed teardown, then tears down without the radio peer'),
        (0 <= at(init, 'loadEspNowDevices();') < at(init, 'espnowRevokedBootTidy();') < at(init, 'restoreEspNowPeers();')
         and 'espnowRevokedMacOrKey(' in tidy and '/*radioUp=*/false' in tidy and 'peerIdentityAt(' in tidy,
         'review 2: at start-up a revoked device still listed or stored is torn down before the radio peers return'),
        (0 <= at(unpair, 'out.keptIdentity = true;') < at(unpair, 'espnowForgetPeerCrypto(mac') and
         '!out.devicesOk && !revoking' in unpair,
         'review 5: an unpair whose registry save failed keeps the identity and folder'),
        ('espnowrevoke::isUnicastMac(mac)' in revoke and 'espnowrevoke::isUnicastMac(mac)' in command,
         'review 6: revoke and the unpair raw-MAC path refuse broadcast, group and all-zero addresses'),
        ('espnowrevoke::firstKeyOpen(dev->awaitingKeyUntilMs' in rollback_queue and
         'espnowrevoke::firstKeyOpen(dev->awaitingKeyUntilMs' in rollback_job,
         'review 4: a rollback needs the open first-key window when queued and again in the job'),
        (0 <= at(pair_request, 'espnowRevokedLocked()') < at(pair_request, 'espnowpair::onRequest('),
         'review 8: a pair request is refused without a prompt while the revoked list is locked'),
        (re.search(r'cacheManifestToLittleFs\([^;]*\)\)\s*\{\s*manifestCacheRecordUse\(srcMac,', manifest_resp)
         is not None,
         'the bond sync records the manifest reference only after the manifest reached the cache'),
        (espnow.count('manifestCacheHit(peerMac, gEspNow->lastRemoteCap.fwHash)') == 1 and
         espnow.count('manifestCachedFor(') == 2,
         'the bond sync looks the cached manifest up through manifestCacheHit, which records it for the partner'),
        (re.search(r'espnowForgetPeerCrypto\(mac, &out\.hadIdentity\)', unpair) is not None and
         re.search(r'=\s*peerIdentityForget\(mac\);[^}]*\*removedAny = had \|\| forgot;', forget_crypto) is not None,
         'the unpair reply reports an identity file removed outside the cache as a removed identity'),
        (re.search(r'parseMacAddress\(gSettings\.bondPeerMac, bondPartner\) && macEquals\(bondPartner, mac\)\)'
                   r'\s*\{[^}]*bondEndPartnership\(\);', unpair) is not None and
         0 <= at(unpair, 'bondEndPartnership();') < at(unpair, 'espnowDeletePeerRuntime(mac)') and
         'isBondPeerMac(' not in unpair,
         'the unpair teardown ends the bond first when the partner setting names the device'),
        (0 <= at(unpair, 'espnowKeyExCancel(mac);') < at(unpair, 'espnowForgetPeerCrypto(mac') and
         0 <= at(unpair, 'pendingFrameDropForPeer(mac);') < at(unpair, 'espnowForgetPeerCrypto(mac'),
         'the unpair teardown calls off its key exchange with the peer and the frames queued for it before the '
         'identity goes'),
    )
    for ok, what in checks:
        if not ok:
            source_failures.append(what)
    for what in source_failures:
        print(f'FAIL: {what}', file=sys.stderr)
    print(f'source checks: {len(checks) - len(source_failures)} of {len(checks)} passed', flush=True)
    print('pieces: peerFilesRemove {}, manifest reference {}, reply line {}'.format(
        *('present' if x else 'absent (stand-in)' for x in (have_peer_files, have_manifest, have_line))), flush=True)

    harness = (HERE / 'espnow_peer_files_harness.cpp').read_text()
    for tag, text in inserts.items():
        if harness.count(tag) != 1:
            sys.exit(f'FAIL extraction: harness: {tag} must appear once')
        harness = harness.replace(tag, text + '\n')

    with tempfile.TemporaryDirectory(prefix='hw1-espnow-peer-files-') as tmp:
        unit, exe = Path(tmp) / 'espnow_peer_files.cpp', Path(tmp) / 'espnow_peer_files'
        (Path(tmp) / 'System_MacText.h').write_text(read('System_MacText.h'))
        unit.write_text(harness)
        cmd = [a.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic',
               f'-DHW1_HAVE_PEER_FILES={int(have_peer_files)}', f'-DHW1_HAVE_MANIFEST_REF={int(have_manifest)}',
               f'-DHW1_HAVE_FILES_LINE={int(have_line)}',
               '-I', tmp, '-I', str(HERE / 'web_batch_stubs'), '-I', str(HERE),
               str(unit), '-o', str(exe)]
        if a.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
                        '-fno-omit-frame-pointer', '-g']
        subprocess.run(cmd, check=True, timeout=240)
        result = subprocess.run([str(exe)], timeout=60)
    if result.returncode or source_failures:
        print('FAIL espnow_peer_files: the unpair file checks reported failures', file=sys.stderr)
        sys.exit(1)
    print('PASS espnow_peer_files')


if __name__ == '__main__':
    main()
