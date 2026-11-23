#pragma once
// Host double of the reader side of System_CaptureCrypto.h for harnesses that
// splice a reader of sealed files (a track probe, a track loader, a file
// view): captureCryptoRevealFileLine over the production line rule
// (System_SealedLogCore.h; add the component to the include path).
//
// The toy seal: a row is "ENC1:" + its text. "ENC1:!" is a row this device
// cannot open. "ENC1:@<class>@<text>" is a row sealed for one class
// (sealedlog::classOf: "capture", "syslog", "stt|u7", ...): it opens only
// when read at a path of that class, as every real row does. A plain line
// where only sealed rows belong becomes "[unsealed line]".
//
// Include it after the harness's String double. sealed_reveal_host::calls
// counts the lines revealed and lastPath is the path the last one was read at.
#include <string>

#include "System_SealedLogCore.h"

namespace sealed_reveal_host {
inline unsigned calls = 0;
inline std::string lastPath;
}

template <class StringType>
sealedlog::Line captureCryptoRevealFileLine(const char* path, sealedlog::LineState& state, StringType& line) {
  ++sealed_reveal_host::calls;
  sealed_reveal_host::lastPath = path ? path : "";
  const std::string text = line.c_str();
  const sealedlog::Line kind = sealedlog::classifyLine(state, text.data(), text.size());
  if (kind == sealedlog::Line::Unsealed) {
    line = StringType(sealedlog::kUnsealedLine);
  } else if (kind == sealedlog::Line::Row) {
    std::string body = text.substr(5);
    if (!body.empty() && body.back() == '\r') body.pop_back();
    bool opens = body != "!";
    if (opens && body[0] == '@') {
      const size_t end = body.find('@', 1);
      char cls[sealedlog::kClassMax];
      opens = end != std::string::npos && sealedlog::classOf(path, cls, sizeof(cls)) && body.substr(1, end - 1) == cls;
      if (opens) body.erase(0, end + 1);
    }
    line = StringType(opens ? body.c_str() : "[undecryptable row]");
  }
  return kind;
}
