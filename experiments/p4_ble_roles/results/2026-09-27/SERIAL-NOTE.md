# Serial completion failure — 2026-09-27

The HTTP/coexistence attempt associated with private serial run
`20260927T175541Z-c8b41e9d` remains **failed**. The S3 joined the P4 test AP and
the runner validated 16 HTTP responses. It then entered the mesh callback before
the remaining HTTP checks and normal logout could finish. Failure cleanup
cleared the HTTP fixture's cookie.

At 17:58:00 UTC, the P4 returned a complete `espnowmessages` JSON object. Its
following `whoami` response was interleaved at character level with an ESP-Hosted
RPC warning. Consequently, the host's strict line-start completion pattern
could not recognize an intact identity line. The normal 15-second deadline and
10-second passive recovery wait expired; the coordinator correctly marked the
console unusable and closed it. The original command was not repeated.

Five subsequent periodic mesh broadcasts reported peer acknowledgements while
the host waited. No panic or abort appeared in that interval. This evidence
supports corrupted serial completion framing rather than an application-wide
hang; it does not establish the health of every task or completion of the
specific mesh test message. The coexistence callback and overall HTTP test must
not be reported as passing.

The existing runtime command `loglink on` routes ESP-IDF logs through the
application output queue, addressing the competing IDF output path. The roles
coordinator now automatically sends it after each board login, so the mitigation
is implemented for subsequent runs without firmware changes. This does not
change the result of the failed attempt. A distinct successful retry is
recorded below; no relaxed parser or automatic command replay was added.

Character interleaving can also split a credential-derived string so that it no
longer matches literal redaction. The affected private log contains fragmented
identity text despite ordinary whole-string filtering. Keep raw logs private;
this note intentionally includes no interleaved line or identity fragments.

## Distinct successful retry

With `loglink on` enabled, serial run `20260927T181111Z-7c325f0e` completed the
HTTP check while P4's real G2 Client role remained initialized and idle. The
runner validated 19 responses totaling 849,727 body bytes, completed its
bidirectional encrypted mesh callback, checked a subsequent API response, then
verified logout and unauthenticated API denial. The complete run passed without
the earlier completion-barrier failure.

This is successful retry evidence under the recorded conditions, not a claim
that arbitrary serial interleaving or credential redaction is solved. The
earlier run remains failed and its raw log remains private. See
[RESULTS.md](RESULTS.md) and [AFTER-CLEANUP.json](AFTER-CLEANUP.json) for the
board-only qualification and sanitized measurements; G2/R1 accessory traffic and
Android UI qualification are outside this retry.
