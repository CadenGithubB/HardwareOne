// FileBrowser_FileKind.h - what the on-device file browsers can do with a file
//
// The OLED file browser and the G2 lens Files page both classify a file with
// fileKindForName, so the same file gets the same viewer on both. The kind
// comes from the extension: the text after the last '.' of the final path
// segment, compared ASCII case-insensitively. No extension (or a display name
// cut before it) is FileKind::Other.
//
// Dependency-free and header-only: host tests compile it directly.
#pragma once

#include <cstddef>

enum class FileKind : unsigned char {
  Other = 0,  // no on-device viewer: Info / Rename / Delete only
  Text,       // plain text (logs, notes, config, source): raw paged view
  Json,       // JSON: Pretty View (System_JsonPretty.h) or the raw JSON View
  ImageBmp,   // BMP: lens image viewer
  ImageJpeg,  // JPEG (.jpg / .jpeg): lens image viewer (decoded to BMP)
};

// Images a device viewer decodes. Other image formats are FileKind::Other.
inline bool fileKindIsImage(FileKind kind) {
  return kind == FileKind::ImageBmp || kind == FileKind::ImageJpeg;
}

// Kinds that read as text (the OLED viewer, the lens' raw view).
inline bool fileKindIsReadableText(FileKind kind) {
  return kind == FileKind::Text || kind == FileKind::Json;
}

// `ext` (`len` bytes, any case) equals the lower-case `lower`.
inline bool fileKindExtEquals(const char* ext, size_t len, const char* lower) {
  size_t i = 0;
  for (; i < len; ++i) {
    char c = ext[i];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (lower[i] == '\0' || c != lower[i]) return false;
  }
  return lower[i] == '\0';
}

inline FileKind fileKindForName(const char* name) {
  if (!name) return FileKind::Other;
  const char* dot = nullptr;
  for (const char* p = name; *p; ++p) {
    if (*p == '/') dot = nullptr;       // a dot in a folder name is not an extension
    else if (*p == '.') dot = p;
  }
  if (!dot || dot[1] == '\0') return FileKind::Other;
  const char* ext = dot + 1;
  size_t len = 0;
  while (ext[len]) ++len;

  struct Entry { const char* ext; FileKind kind; };
  static constexpr Entry kTable[] = {
      {"bmp", FileKind::ImageBmp},
      {"jpg", FileKind::ImageJpeg}, {"jpeg", FileKind::ImageJpeg},
      {"json", FileKind::Json},
      {"txt", FileKind::Text},  {"csv", FileKind::Text},  {"log", FileKind::Text},
      {"md", FileKind::Text},   {"ini", FileKind::Text},  {"cfg", FileKind::Text},
      {"conf", FileKind::Text}, {"yaml", FileKind::Text}, {"yml", FileKind::Text},
      {"xml", FileKind::Text},  {"html", FileKind::Text}, {"js", FileKind::Text},
      {"c", FileKind::Text},    {"h", FileKind::Text},    {"cpp", FileKind::Text},
  };
  for (const Entry& entry : kTable) {
    if (fileKindExtEquals(ext, len, entry.ext)) return entry.kind;
  }
  return FileKind::Other;
}
