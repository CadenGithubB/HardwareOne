// Host harness for test_oled_dead_paths.py: the OLED mode slugs after the dead
// unified actions mode is gone.
//
// The suite pastes the production `enum OLEDMode` (OLED_Display.h, with a
// harness sentinel appended) and modeFromSlug / slugFromMode (OLED_Utils.cpp)
// into the markers below. Checks: `oledmode unified` names no mode, no mode's
// slug resolves to a different mode, and the IMU actions aliases still work.

#include <cstdio>
#include <cstring>
#include <string>

class String {
 public:
  String() = default;
  String(const char* text) : text_(text ? text : "") {}
  bool operator==(const char* other) const { return other && text_ == other; }
  const char* c_str() const { return text_.c_str(); }

 private:
  std::string text_;
};

// INSERT_ENUM

// INSERT_SLUGS

static int gChecks = 0;
static int gFailures = 0;

static void check(bool ok, const char* what) {
  ++gChecks;
  if (!ok) {
    ++gFailures;
    std::printf("FAIL: %s\n", what);
  }
}

int main() {
  check((int)modeFromSlug(String("unified")) == -1,
        "`oledmode unified` must not open a mode (the unified actions mode is gone)");

  for (int value = 0; value < (int)OLED_HARNESS_MODE_END; ++value) {
    const OLEDMode mode = (OLEDMode)value;
    const char* slug = slugFromMode(mode);
    if (!slug) continue;
    const int back = (int)modeFromSlug(String(slug));
    ++gChecks;
    if (back != -1 && back != value) {
      ++gFailures;
      std::printf("FAIL: slugFromMode(mode %d) = \"%s\", which modeFromSlug resolves to mode %d\n",
                  value, slug, back);
    }
  }

  check(modeFromSlug(String("actions")) == OLED_IMU_ACTIONS,
        "\"actions\" still opens IMU actions");
  check(modeFromSlug(String("imuactions")) == OLED_IMU_ACTIONS,
        "\"imuactions\" still opens IMU actions");
  check(std::strcmp(slugFromMode(OLED_IMU_ACTIONS), "imuactions") == 0,
        "IMU actions keeps its primary slug");

  std::printf("oled_dead_paths: %d checks, %d failed\n", gChecks, gFailures);
  return gFailures == 0 ? 0 : 1;
}
