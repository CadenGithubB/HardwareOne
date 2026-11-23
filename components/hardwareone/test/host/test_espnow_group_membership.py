#!/usr/bin/env python3
"""Run the room and tag membership of paired ESP-NOW peers on the production code.

espnowroomcmd and espnowtagcmd send a command with an account name and
password to every member of a room or tag. Room and tags are what each peer
says about itself, so before this any paired device that named a room got the
password sent to that room. This device now keeps, per peer, what its admin
accepted, and only that makes a member (the rules: System_ESPNow_GroupCore.h,
also run alone by test_espnow_group_core.cpp).

Extracts from System_ESPNow.cpp the record glue with its lock and the writer
and reader of the stored fields, the per-sender metadata inbox, the
announcement functions with the report limit and the save gap, the acceptance
functions, the metadata refresh tick, processMetadata,
addEspNowDevice, removeEspNowDevice, resolveDeviceNameOrMac,
countMeshPeerMetaByRoom, the listing marks with espnowSerializeMeshPeerMeta,
cmd_espnow_devices and cmd_espnow_rooms, and the commands; from the headers the device record, the
mesh cache record, the view and the metadata payload. Compiles them with the
rules header and the vendored ArduinoJson beside doubles for the registry
state and its save, the sessions, the encrypted send, the metadata request,
the pending-frame queue, the mutex, events and logging
(espnow_group_membership_harness.cpp). Checks:

- a peer that newly says it is in a room is not sent the room's command until
  accepted, and the reply counts it and names the accept command;
- an accepted peer is sent it, with the credentials, and a pending one beside
  it is not;
- a change of room is out of the old room with that frame and pending for the
  new; clearing the room leaves it; coming back waits again;
- tags likewise, one by one;
- accepting records what is announced at that moment and the reply says what;
- 'all', drop, the JSON forms, the refusals, a registry that cannot be saved;
- unpairing removes the accepted state and a re-paired address starts empty;
  an identity change clears it, drops the frames waiting for a session and is
  said once;
- the stored record round trip, a record without the fields (nothing
  accepted), a record that names as accepted what it does not announce, and
  values that do not fit;
- the notice comes once per change, not per frame and not again after a
  restart, and names the peer by its local name;
- with sixteen peers every member is sent to and the replies stay whole;
- the listings show the announced value with a mark when it is not accepted;
- a peer without a slot in the (smaller) metadata cache still has its room and
  tags recorded, above all when it leaves;
- a peer that keeps changing prints one report a minute and costs one registry
  save per gap, while each change still takes effect at once;
- the lock is never taken twice or held across a log line, a save or a send,
  and a step of the other task just before a critical section is honoured;
- a tag target with a comma is refused before anything is sent;
- the JSON listings stay under the result size and parse with a full table;
- a name that two records share is not used to call or to accept a peer;
- the inbox keeps a frame per sender and answers every peer that asked, and a
  session that came up gets its peer asked for its metadata once.

Source contracts, read as text: the registry writer and reader call the two
field functions, the new-record sites start empty, unpair removes the record,
forgetting or replacing an identity clears the acceptance, the group commands
do not read the mesh cache, the command rows are admin, processMetadata
updates the registry before it looks for a cache slot, the task loop runs the
inbox, the refresh and the two membership ticks, unpair moves records under
the lock, and the lock is a mutex (the records are in PSRAM).

--component <hardwareone dir> reads the production files from another tree.
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

# Regions of System_ESPNow.cpp, each from its first line to the line that follows it.
REGIONS = (
    ('// ---- Room and tag membership of a paired peer -----',
     '// Save named ESP-NOW devices (paired devices with names/keys) to filesystem.'),
    ('// ---- Metadata work waiting for espnow_task -----',
     'static void v4h_metadata_req(const V4RxCtx& ctx) {'),
    ('// ---- Room and tag membership: what a peer announces -----',
     "/**\n * Process received metadata: the paired peer's registry record first"),
)
FUNCTIONS = (
    'static void processMetadata(',
    'static void addEspNowDevice(',
    'void removeEspNowDevice(',
    'bool resolveDeviceNameOrMac(const String& nameOrMac, uint8_t mac[6])',
    'int countMeshPeerMetaByRoom(',
)
LISTING_REGION = ('// ---- What the listings say about room and tag membership -----',
                  '// Shared by the espnowdevices json CLI branch and the web metadata handler so')
# The bounded appender the text listings write with: its declaration with the
# row reserve, and its definition (with the topology results).
APPENDER_REGION = ('// Bounded reply appender, defined with the topology results below',
                   'const char* cmd_espnow_devices(')
APPENDER_DEFINITION = ('static bool appendTopologyOutput(char*& cursor, size_t& remaining,\n'
                       '                                 const char* format, ...) {')
LISTING_FUNCTIONS = ('void espnowSerializeMeshPeerMeta(JsonObject o, const MeshPeerMeta& m)',
                     'const char* cmd_espnow_devices(',
                     'const char* cmd_espnow_rooms(')
COMMAND_REGION = ('// ---- Group commands and the membership they go by -----',
                  '// Mesh role command')
ADMIN_COMMANDS = ('espnowroomcmd', 'espnowtagcmd', 'espnowgroups', 'espnowgroupaccept', 'espnowgroupdrop')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('clang++') or shutil.which('c++'))
    p.add_argument('--sanitize', action='store_true')
    p.add_argument('--component', type=Path, default=HERE.parents[1])
    a = p.parse_args()
    if not a.cxx:
        p.error('a host C++17 compiler is required')
    sys.path.insert(0, str(HERE))
    from test_web_batch_handlers import extract_block

    source = (a.component / 'System_ESPNow.cpp').read_text()
    crypto = (a.component / 'System_ESPNow_Handlers_Crypto.cpp').read_text()
    header = (a.component / 'System_ESPNow.h').read_text()
    wire = (a.component / 'System_ESPNow_Wire.h').read_text()

    def one(text, marker, what='System_ESPNow.cpp'):
        if text.count(marker) != 1:
            sys.exit(f'FAIL extraction: {what} marker {marker!r} found {text.count(marker)} times, expected 1')
        return extract_block(text, marker)

    def region(first, after):
        if source.count(first) != 1 or source.count(after) != 1:
            sys.exit(f'FAIL extraction: region {first!r} .. {after!r} is not unique in System_ESPNow.cpp')
        start, end = source.index(first), source.index(after)
        if end <= start:
            sys.exit(f'FAIL extraction: region {first!r} ends before it starts')
        return source[start:end]

    types = [
        'template <size_t Capacity>\n' + one(header, 'class EspNowInlineText', 'System_ESPNow.h') + ';',
        one(header, 'struct EspNowDevice', 'System_ESPNow.h') + ';',
        one(header, 'struct MeshPeerMeta', 'System_ESPNow.h') + ';',
        one(header, 'struct EspNowGroupView', 'System_ESPNow.h') + ';',
        one(wire, 'struct __attribute__((packed)) V4PayloadMetadata', 'System_ESPNow_Wire.h') + ';',
    ]
    defines = []
    for name in ('ESPNOW_V4_MAX_PAYLOAD', 'ESPNOW_V4_AEAD_TAG_LEN', 'ESPNOW_V4_MAX_PLAINTEXT'):
        found = re.findall(rf'^#define {name}\b.*$', wire, re.MULTILINE)
        if len(found) != 1:
            sys.exit(f'FAIL extraction: #define {name} found {len(found)} times in System_ESPNow_Wire.h')
        defines.append(found[0])

    pieces = [region(*r) for r in REGIONS]
    pieces += [one(source, marker) for marker in FUNCTIONS]
    pieces.append(region(*LISTING_REGION))
    pieces.append(region(*APPENDER_REGION))
    if source.count(APPENDER_DEFINITION) != 1:
        sys.exit('FAIL extraction: the appendTopologyOutput definition is not unique in System_ESPNow.cpp')
    pieces.append(extract_block(source[source.index(APPENDER_DEFINITION):], 'static bool appendTopologyOutput('))
    pieces += [one(source, marker) for marker in LISTING_FUNCTIONS]
    pieces.append(region(*COMMAND_REGION))

    harness = (HERE / 'espnow_group_membership_harness.cpp').read_text()
    for tag, value in (('// INSERT_TYPES', '\n\n'.join(types)),
                       ('// INSERT_WIRE', '\n'.join(defines)),
                       ('// INSERT_PRODUCTION', '\n\n'.join(pieces))):
        if harness.count(tag) != 1:
            sys.exit(f'FAIL: harness marker {tag} must appear exactly once')
        harness = harness.replace(tag, value)

    failures = []
    json_include = a.component.parent / 'hardwareone_libs' / 'ArduinoJson' / 'src'
    with tempfile.TemporaryDirectory(prefix='hw1-espnow-group-membership-') as tmp:
        unit, exe = Path(tmp) / 'espnow_group_membership.cpp', Path(tmp) / 'espnow_group_membership'
        unit.write_text(harness)
        # The log doubles keep printf format checking; -Wformat-pedantic is off
        # because production logs typed pointers with %p (fine on the target).
        cmd = [a.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic', '-Wno-format-pedantic',
               '-Wno-unused-function',
               '-DARDUINOJSON_ENABLE_ARDUINO_STRING=1', '-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0',
               '-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0', '-DARDUINOJSON_ENABLE_PROGMEM=0',
               '-I', str(a.component / 'test/host/web_batch_stubs'), '-I', str(a.component),
               '-isystem', str(json_include), str(unit), '-o', str(exe)]
        if a.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
                        '-fno-omit-frame-pointer', '-g']
        subprocess.run(cmd, check=True, timeout=300)
        sys.stdout.flush()
        if subprocess.run([str(exe)], timeout=60).returncode:
            failures.append('the membership cases reported failures')

    # ---- Source contracts.
    def body(text, marker, what='System_ESPNow.cpp'):
        return one(text, marker, what)

    saver = body(source, 'static bool saveEspNowDevices()')
    loader = body(source, 'static void loadEspNowDevices()')
    unpair = body(source, 'const char* cmd_espnow_unpair(')
    forget = body(source, 'static bool espnowForgetPeerCrypto(')
    regen = body(source, 'const char* cmd_espnow_regenidentity(')
    metadata = body(source, 'static void processMetadata(')
    runner = body(source, 'static const char* groupCommandRun(')
    pairsecure = body(source, 'const char* cmd_espnow_pairsecure(')
    add = body(source, 'static void addEspNowDevice(')

    def at(text, part):
        return text.find(part)

    contracts = (
        ('groupAcceptedToJson(entry, dev);' in saver,
         'saveEspNowDevices writes the accepted room and tags of each record'),
        (0 <= at(loader, 'dev.tags.assign(tags)') < at(loader, 'groupAcceptedFromJson(entry, dev)'),
         'loadEspNowDevices reads the accepted room and tags after the announced ones'),
        ('groupNoteAnnouncement(srcMac, metadata->room, metadata->tags)' in metadata and
         '->room = metadata->room' not in metadata and '->tags = metadata->tags' not in metadata,
         'processMetadata hands room and tags to groupNoteAnnouncement and stores them nowhere else'),
        (0 <= at(metadata, 'groupNoteAnnouncement(srcMac,') < at(metadata, 'if (!gMeshPeerMeta)') <
         at(metadata, 'meshPeerMetaReclaimSlot(srcMac)'),
         'processMetadata updates the registry record before it looks for a cache slot'),
        ('gMeshPeerMeta' not in runner and 'acceptedRoom' not in runner and
         'espnowgroup::inRoom(m, target.c_str())' in runner and 'espnowgroup::hasTag(m, target.c_str())' in runner,
         'groupCommandRun picks its targets through the rules, from the registry and not from the mesh cache'),
        (runner.count('v4_send_encrypted_or_queue(') == 1 and
         at(runner, 'if (!member)') < at(runner, 'v4_send_encrypted_or_queue(') and
         'continue;' in runner[at(runner, 'if (!member)'):at(runner, 'v4_send_encrypted_or_queue(')],
         'groupCommandRun sends in one place, after the membership check'),
        ('removeEspNowDevice(mac);' in unpair,
         'cmd_espnow_unpair removes the device record, and with it what was accepted'),
        ('espnowGroupAcceptanceClear(mac,' in forget,
         'forgetting a peer identity clears what was accepted for it'),
        ('espnowGroupAcceptanceClear(mac,' in regen,
         'regenerating this device identity clears what was accepted for every peer'),
        (crypto.count('espnowGroupAcceptanceClear(') == 2 and
         len(re.findall(r'if \(identityNewOrReplaced\) \{[^}]*espnowGroupAcceptanceClear\(msg->(?:sender|responder)Mac,',
                        crypto)) == 2,
         'both KEY_EX handlers clear the acceptance when a stored identity is new or replaced'),
        (all('acceptedRoom = "";' in text and 'acceptedTags = "";' in text for text in (pairsecure, add)),
         'every place that fills a new device record starts it with nothing accepted'),
    )
    loop = body(source, 'void processMeshHeartbeats()')
    remove = body(source, 'void removeEspNowDevice(')
    serializer = body(source, 'void espnowSerializeMeshPeerMeta(JsonObject o, const MeshPeerMeta& m)')
    more = (
        (all(call in loop for call in ('metadataInboxTakeReply(replyMac)', 'metadataRefreshTick(now);',
                                       'metadataInboxHandleFrame();', 'groupNoticeTick(now);',
                                       'announceSaveTick(now);')),
         'the espnow_task loop answers metadata requests, asks for a refresh, handles a received frame and runs '
         'the report and save ticks'),
        ('deferredMetadataPending' not in source and 'deferredMetadataPending' not in header and
         'metadataPendingResponseMac' not in source and 'metadataPendingResponseMac' not in header,
         'no single shared buffer is left for received metadata or for the peer to answer'),
        (0 <= at(remove, 'GroupLockGuard lock;') < at(remove, 'gEspNow->devices[j] = gEspNow->devices[j + 1]'),
         'removeEspNowDevice moves the records under the membership lock'),
        ('static SemaphoreHandle_t gGroupLock = xSemaphoreCreateMutexStatic(' in source and
         'portENTER_CRITICAL(&gGroupLock)' not in source,
         'the membership lock is a mutex, not a spinlock (device records are in PSRAM)'),
        ('groupPendingToJson(o, m.mac, m.room, m.tags);' in serializer and 'accepted' not in serializer,
         'the per-peer shape of the whole-mesh listing carries what waits and no copy of what is accepted'),
        ('espnowgroup::singleTag(target.c_str()' in runner and
         at(runner, 'espnowgroup::singleTag(target.c_str()') < at(runner, 'v4_send_encrypted_or_queue('),
         'groupCommandRun refuses a tag list before it sends anything'),
    )
    contracts = contracts + more
    for ok, what in contracts:
        if not ok:
            failures.append(what)
    rows = dict(re.findall(r'^\s*\{\s*"(\w+)",\s*"(?:[^"\\]|\\.)*",\s*(true|false),', source, re.MULTILINE))
    for name in ADMIN_COMMANDS:
        if rows.get(name) != 'true':
            failures.append(f'command {name} is not an admin row of espNowCommands[]')

    for line in failures:
        print(f'FAIL: {line}')
    if failures:
        sys.exit(1)
    print(f'source contracts: {len(contracts) + len(ADMIN_COMMANDS)} passed')
    print('PASS espnow_group_membership')


if __name__ == '__main__':
    main()
