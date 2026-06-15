# HardwareOne — File Browser integration (CLI file commands over BLE)

This is the contract for building a **file browser** in the Android companion
app — browse, view, **edit/write**, create file/folder, rename, and delete —
with the same behavior as the web file browser. It uses the firmware's existing
command channel: **no new GATT service, no web/HTTP dependency.**

---

## TL;DR for the app

1. You already have a working command channel (write a command string to the
   REQUEST characteristic, read the reply on the RESPONSE notify characteristic
   — the same path `login` and `status json` use).
2. **File commands require the Secure Channel.** Establish it *before* any file
   command (see [Transport & security](#transport--security)). On a cleartext
   link every file command is rejected with
   `{"success":false,"error":"secure_channel_required"}`.
3. **File commands require admin.** Log in as an admin user. Non-admins get
   `Error: Admin access required …`. (This matches the existing CLI file
   commands; the device owner is admin.)
4. The command surface (all replies are one JSON object — reassemble your reply
   chunks, then `JSON.parse` once):

   | Action | Command |
   |---|---|
   | List a directory | `files json ["path"]` |
   | Storage usage | `files stats json ["path"]` |
   | Read file (chunked) | `fileread "<path>" [offset] [len] [b64]` |
   | Write file (chunked) | `filewrite "<path>" <offset> <b64chunk> [final]` |
   | Create folder | `mkdir "<path>"` |
   | Create empty file | `filecreate "<path>"` |
   | Rename | `filerename "<oldpath>" "<newname>"` |
   | Delete | `filedelete "<path>" confirm` |

   > **Paths MUST be double-quoted — always, even when they contain no spaces.**
   > Every path argument is wrapped in `"..."` (the firmware reads it as one
   > quoted token). An unquoted path now returns an error (for JSON commands,
   > `{"success":false,"error":"path must be a quoted token"}`). Flag/number
   > args (`json`, `confirm`, `final`, offset, len) stay **bare** — do not quote
   > them. A filename may not contain a literal `"` (the firmware rejects it).

   `mkdir` / `filecreate` / `filerename` return short human-readable strings
   (success if the reply has no `Error:`/`Usage:` prefix). The rest return JSON.

That's the whole integration. Everything below is detail.

---

## Transport & security

- **Same channel as every other command.** Send the literal command string;
  read the reply. The `json` token is detected at a word boundary and is
  case-sensitive (lowercase `json`).
- **Secure Channel is mandatory for file commands.** File contents and paths
  must never cross the link in cleartext, so the firmware rejects the whole file
  command group (`files`, `fileread`, `filewrite`, `fileview`, `filecreate`,
  `filedelete`, `filerename`, `mkdir`, `rmdir`) unless the connection has an
  established Secure Channel — *independent of* the global `blesecure` setting.
  - The operator must set a secret on the device (`blesecret <phrase>`) and the
    user enters the same phrase in the app once. Without a secret the app cannot
    establish the channel and the file browser is unavailable over BLE.
  - Handshake order per connection: `requestMtu(517)` → enable RESPONSE
    notifications (CCCD) → **Secure Channel handshake** → `login <user> <pass>`
    → file commands. See `System_BleSecureChannel.h` for the wire format.
- **Reassembly.** Over the Secure Channel, long replies are chunked into DATA
  frames (~200 plaintext bytes each) and arrive in order; accumulate them and
  retry `JSON.parse` until it succeeds (the standard reply-reassembly you
  already do). A `files json` of a large directory or a `fileread` chunk can
  span many frames — this is expected and handled by the channel.

## Versioning

No version field. Keys are additive — handle unknown keys gracefully and don't
assume key order. The JSON shapes here are the same ones the web file browser's
`/api/files/*` endpoints return, so they won't silently drift.

## Error shape

Every JSON-returning command uses the envelope:

```json
{ "success": false, "error": "<reason>" }
```

`error` strings worth special-casing:

| `error` | Meaning | App action |
|---|---|---|
| `secure_channel_required` | Sent a file command on a cleartext link | Establish the Secure Channel, retry |
| `Admin required` | Path is admin-only territory and you're not admin | Hide/disable; surface "admin only" |
| `Not found or access denied` | Path missing or no READ perm | Show not-found |
| `Offset mismatch` (+ `size`) | A write chunk didn't land at end-of-file | Resync: resume writing from `size` |
| `File too large for BLE …` | Write would exceed the 256 KB BLE cap | Steer the user to the web browser |
| `SD card not available` | `/sd…` stats requested but no card mounted | Show "no SD" |

Admin denial and auth-required come back as **plain text** (not JSON), because
they're produced by the command framework before the handler runs:
`Error: Admin access required for command 'files'. Contact an administrator.`
and `Authentication required. Use: login <username> <password>`.

---

## Listing a directory — `files json [path]`

`path` defaults to `/`. Response mirrors the web `/api/files/list`:

```json
{
  "success": true,
  "dirPerms": 63,
  "files": [
    { "name": "logs",        "type": "folder", "size": "4 items",   "count": 4, "perms": 1 },
    { "name": "config.json", "type": "file",   "size": "812 bytes",             "perms": 63 }
  ]
}
```

- `type` is `"folder"` or `"file"`. `size` is a **display string** (don't parse
  it for folders; for files it's `"<n> bytes"`). Folders also carry a numeric
  `count`.
- `perms` / `dirPerms` is a **bitmask** of what *the logged-in user* may do —
  use it to enable/disable buttons exactly like the web UI:

  | Bit | Value | Meaning |
  |---|---|---|
  | READ | `0x01` | view / download / read |
  | WRITE | `0x02` | edit (overwrite contents) |
  | DELETE | `0x04` | delete |
  | RENAME | `0x08` | rename |
  | CREATE | `0x10` | create file/folder in (dir) |
  | IMPORT | `0x20` | upload/import |

- Admin-only branches a non-admin can't see are already omitted from `files`,
  and listing such a path directly returns `{"success":false,"error":"Admin required"}`.

## Storage usage — `files stats json [path]`

The tier is chosen from the path (`/sd…` → SD card, else internal LittleFS):

```json
{ "success": true, "total": 8388608, "used": 3145728, "free": 5242880, "usagePercent": 37 }
```

---

## Reading a file — `fileread "<path>" [offset] [len] [b64]`

BLE can't stream like HTTP, so pull the file in bounded windows: start at
`offset 0` and loop, advancing `offset` by `len`, until `eof` is true.

- `offset` default `0`. `len` default/maximum **4096** bytes per call (values
  above are clamped). `b64` forces base64 even for text.
- Response:

  ```json
  {
    "success": true,
    "path": "/config.json",
    "size": 812,
    "offset": 0,
    "len": 812,
    "eof": true,
    "enc": "utf8",
    "data": "…"
  }
  ```

- `enc` is `"utf8"` or `"b64"`. The firmware auto-selects `b64` whenever the
  chunk contains a NUL, a high-bit byte (any non-ASCII, including UTF-8 text), or
  a control char other than tab/newline/CR. So:
  - `enc == "utf8"` → `data` is a JSON string of plain ASCII; use it directly.
  - `enc == "b64"` → base64-decode `data` to get the raw bytes.
- `size` is the **total** file size; `len` is the bytes in *this* chunk.

**Pull loop (pseudocode):**

```
offset = 0; buf = []
loop:
  r = cmd("fileread " + path + " " + offset + " 4096")
  bytes = (r.enc == "b64") ? b64decode(r.data) : utf8bytes(r.data)
  buf.append(bytes); offset += r.len
  if r.eof: break
```

**Binary / images:** small icons/thumbnails are fine via `b64`. Large media is
slow over BLE — there's a **256 KB** practical ceiling; send users to the web
browser for big files.

---

## Writing / editing a file — `filewrite "<path>" <offset> <b64chunk> [final]`

The REQUEST characteristic caps each inbound command at ~512 bytes with no
reassembly, so upload as a sequence of **small base64 chunks** (keep the raw
chunk ≲ 256 bytes so the whole command line fits). Writes are **strictly
sequential**:

- `offset 0` → truncates/creates the file, then writes the chunk.
- `offset > 0` → appends; the offset **must equal the current file size**.
- Add `final` to the last chunk to run post-save hooks (e.g. de-duping
  `automations.json`). `final` may be sent on an empty last chunk too.
- Cap: `offset + chunk` may not exceed **256 KB**.

Response:

```json
{ "success": true, "size": 1024, "final": false }
```

On a dropped/duplicated chunk you get
`{"success":false,"error":"Offset mismatch","size":<actualSize>}` — resume
writing from `size`.

**Write loop (pseudocode):**

```
offset = 0
for chunk in chunksOf(fileBytes, 192):
  last = isLast(chunk)
  r = cmd("filewrite " + path + " " + offset + " " + b64(chunk) + (last ? " final" : ""))
  if !r.success: handle(r)         // e.g. offset mismatch → resync to r.size
  offset = r.size
```

To **edit** an existing text file: `fileread` it, let the user edit, then
`filewrite` the whole new content starting at offset 0.

---

## Create / rename / delete

- **Create folder:** `mkdir "<path>"` — idempotent (existing folder is success).
- **Create empty file:** `filecreate "<path>"`.
- **Rename:** `filerename "<oldpath>" "<newname>"` — `newname` is just the basename
  (the file stays in the same directory). Reply `Renamed: <old> -> <new>` on
  success.
- **Delete:** `filedelete "<path>" confirm` — the trailing `confirm` token (bare,
  not quoted) does a one-shot delete (the bare `filedelete "<path>"` is an interactive two-step gate
  meant for the serial console; **always pass `confirm` from the app**). Reply
  `Deleted file: <path>` on success.

These three return **plain text**, not JSON. Treat a reply that starts with
`Error:` or `Usage:` as failure; anything else is success.

---

## Do / Don't

- **Do** establish the Secure Channel and `login` as admin before any file
  command. **Don't** screen-scrape the plain-text `files`/`fileview` output —
  use `files json` / `fileread`.
- **Do** drive button enable/disable from the `perms` bitmask, exactly like the
  web UI. **Don't** assume an action is allowed just because the entry is listed.
- **Do** loop `fileread`/`filewrite` on `offset` until `eof` / last chunk.
  **Don't** try to move large media over BLE — use the web browser for that.
- **Do** pass `confirm` to `filedelete`. **Don't** rely on the interactive
  two-step flow over BLE.

## Firmware references

- Commands: `cmd_files`, `cmd_fileread`, `cmd_filewrite`, `cmd_filedelete`,
  `cmd_filerename`, `cmd_mkdir`, `cmd_filecreate` in
  `components/hardwareone/System_Filesystem.cpp` (registry: `filesystemCommands[]`).
- Shared helpers (single source of truth for web + BLE, in `System_Filesystem.cpp`):
  `buildFilesListJson` (the `{success,dirPerms,files[]}` envelope, used by
  `/api/files/list` too), `buildFilesStatsJson` (used by `/api/files/stats`),
  `buildFilesListing`, and `runFileWritePostSaveHooks` (automations.json
  sanitize, shared with the web write/upload paths).
- Permission model: the role-aware `PathRule` table + `VFS::*Guarded` in
  `System_Filesystem.cpp` / `System_VFS.cpp`.
- Secure Channel gate: `bleIsFileBrowserCommand` + `bleScEstablished` in
  `components/hardwareone/Bluetooth.cpp` (`processBleCommandLine`).
