# Deferred: Studio relay service stopped unexpectedly

Status: deferred. On 2026-11-05 the user said they would investigate why the
Studio relay was off later. Do not install, restart, or reconfigure a service
as part of this note.

The second P4 was already configured for
the Studio relay address (private; port 18790, path `/v1`), model `openclaw/main`. The user confirmed
that this is the correct Mac Studio on another VLAN. The address was left
unchanged.

During testing, Bluetooth authentication and the phone relay were up, but two
requests were refused by the phone with protocol error 7 (connection failure),
before HTTP request data was sent. The user discovered that the Studio relay
was off, started it, and then reported the glasses working. The P4 subsequently
recorded 10 completed Terminal requests carried by the phone relay, with zero
new crash records. This confirms a working phone-to-Studio path at that time;
it does not identify why the service stopped.

The Mac mini is on VLAN 22 and its direct TCP test to that Studio port timed
out even when the phone path worked. Do not treat the Mac mini's connectivity
as proof of the phone's route or change the correct gateway URL based on it.

Future work, when the user resumes it:

- Identify how the Studio relay is launched and why it stopped.
- Inspect its existing process/service state and relevant shutdown/error logs.
- Determine whether persistence, restart policy, or service health reporting
  needs a change. Preserve the existing TLS, credentials and gateway behavior.
- Verify recovery and phone access after any agreed change.

No Studio service or network policy was changed by this investigation.
