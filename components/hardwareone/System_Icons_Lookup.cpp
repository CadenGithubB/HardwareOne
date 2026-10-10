// Handwritten icon lookup policy. Generated payloads stay in System_Icons.cpp.
#include "System_Icons.h"
#include <cstring>

bool iconExists(const char* name) {
  return name && name[0] && findEmbeddedIcon(name) != nullptr;
}

// Extension-to-icon mapping (shared logic)
static const struct {
  const char* ext;
  const char* icon;
} kExtIconMap[] = {
  {"json", "file_json"},
  {"txt", "file_text"},
  {"md", "file_text"},
  {"log", "file_text"},
  {"ino", "file_code"},
  {"cpp", "file_code"},
  {"h", "file_code"},
  {"hpp", "file_code"},
  {"c", "file_code"},
  {"py", "file_code"},
  {"js", "file_code"},
  {"html", "file_code"},
  {"css", "file_code"},
  {"xml", "file_code"},
  {"yaml", "file_code"},
  {"yml", "file_code"},
  {"jpg", "file_image"},
  {"jpeg", "file_image"},
  {"png", "file_image"},
  {"gif", "file_image"},
  {"bmp", "file_image"},
  {"svg", "file_image"},
  {"zip", "file_zip"},
  {"tar", "file_zip"},
  {"gz", "file_zip"},
  {"7z", "file_zip"},
  {"rar", "file_zip"},
  {"pdf", "file_pdf"},
  {"bin", "file_bin"},
  {"hex", "file_bin"},
  {"elf", "file_bin"},
  {nullptr, "file"}  // default fallback
};

const char* getIconNameForExtension(const char* ext) {
  if (!ext || !*ext) return "file";

  // Normalize: skip leading dot and lowercase
  const char* p = ext;
  if (*p == '.') p++;

  // Simple lowercase without locale
  char lower[16];
  size_t i = 0;
  for (; i < sizeof(lower)-1 && p[i]; ++i) {
    char c = p[i];
    if (c >= 'A' && c <= 'Z') c += 32;
    lower[i] = c;
  }
  lower[i] = '\0';

  for (size_t idx = 0; kExtIconMap[idx].ext; ++idx) {
    if (strcmp(lower, kExtIconMap[idx].ext) == 0) {
      return kExtIconMap[idx].icon;
    }
  }
  return "file";  // default
}
