#pragma once
// The HTTPS server key pair on the device filesystem: the one definition of
// both paths. HTTPS start (webLoadHttpsCertPair, WebServer_Utils.cpp) reads
// them; certgen and certinfo (System_WiFi.cpp) write and read them and quote
// them in their replies, built from these macros by literal concatenation so
// the reply texts stay one string. Both live under VFS::Scopes::CERTS
// (System_VFS.h), the scope every access passes to systemAuth().
// Macros, not constants, so a reply text can splice them in at compile time.
// The web settings page script (WebPage_Settings.h) still spells them in its
// own raw literal.
#define HW1_HTTPS_CERT_PATH "/system/certs/https_server.crt"
#define HW1_HTTPS_KEY_PATH "/system/certs/https_server.key"  // a file path, not key material. secret-guard: allow
