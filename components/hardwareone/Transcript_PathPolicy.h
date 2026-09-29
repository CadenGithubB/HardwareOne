#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

// Private transcript paths, independent of Arduino/FS. Inputs are the guarded
// VFS's canonical paths. Owner IDs are persistent users.json IDs, not usernames:
// case-distinct usernames and FAT case folding must never share a directory.
namespace TranscriptPathPolicy {
enum class Kind : uint8_t { Outside, Root, Owner, Invalid };
struct Path { Kind kind = Kind::Outside; uint32_t owner = 0; };
inline constexpr uint8_t kRead = 0x01, kAll = 0x3f;
inline bool separator(char c, bool sd) { return c == '/' || (sd && c == '\\'); }
inline char lower(char c) { return c >= 'A' && c <= 'Z' ? char(c + ('a' - 'A')) : c; }
inline Path classify(const char* path) {
    if (!path || path[0] != '/') return {};
    const bool sd = std::strncmp(path, "/sd/", 4) == 0;
    const char* begin = path + (sd ? 4 : 1);
    bool canonical = true;
    // FatFs treats backslash as a separator and accepts dot components. These
    // aliases are classified as protected then denied, never passed to the
    // permissive ordinary-data rule. LittleFS treats backslash literally.
    if (sd) {
        for (;;) {
            while (separator(*begin, true)) { canonical = false; ++begin; }
            if (begin[0] == '.' && separator(begin[1], true)) {
                canonical = false; begin += 2; continue;
            }
            break;
        }
    }
    const char* end = begin;
    while (*end && !separator(*end, sd)) ++end;
    const char* trimmed = end;
    if (sd) while (trimmed > begin && (trimmed[-1] == '.' || trimmed[-1] == ' ')) --trimmed;
    if (trimmed - begin != 3 || (sd ? lower(begin[0]) : begin[0]) != 's'
        || (sd ? lower(begin[1]) : begin[1]) != 't'
        || (sd ? lower(begin[2]) : begin[2]) != 't') return {};
    canonical = canonical && end == trimmed && std::memcmp(begin, "stt", 3) == 0
        && (*end == '/' || *end == '\0');
    if (!canonical) return {Kind::Invalid, 0};
    if (!*end) return {Kind::Root, 0};
    const char* owner = end + 1;
    if (!*owner) return {Kind::Root, 0};
    // Exactly u<nonzero decimal ID>, no leading zero, trailing dot/space,
    // alternate case or 8.3 '~' alias. Long numeric IDs can have FAT aliases;
    // those are deliberately never an authorized owner spelling.
    if (*owner++ != 'u' || *owner < '1' || *owner > '9') return {Kind::Invalid, 0};
    uint32_t value = 0;
    while (*owner >= '0' && *owner <= '9') {
        const unsigned digit = unsigned(*owner++ - '0');
        if (value > (UINT32_MAX - digit) / 10) return {Kind::Invalid, 0};
        value = value * 10 + digit;
    }
    if (*owner && *owner != '/') return {Kind::Invalid, 0};
    return {Kind::Owner, value};
}
inline uint8_t permissionMask(Path path, uint32_t accountId, bool unrestricted) {
    if (path.kind == Kind::Outside || unrestricted) return kAll;
    if (!accountId) return 0; // synthetic/AuthBypass is not a transcript owner
    if (path.kind == Kind::Root) return kRead;
    return path.kind == Kind::Owner && path.owner == accountId ? kAll : 0;
}
} // namespace TranscriptPathPolicy
