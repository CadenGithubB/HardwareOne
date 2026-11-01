#include "System_BuildConfig.h"
#if ENABLE_SPEAKER
#include "HAL_Speaker.h"
#include "System_Command.h"
#include "System_Utils.h"
#include <cstdlib>

namespace {
const char *tone(const String &args) {
  CommandArgs parsed(args);
  if (parsed.count() > 3)
    return "Usage: speakertone [100..4000 Hz] [50..2000 ms] [0..60 volume]";
  uint32_t values[] = {660, 400, 25};
  const uint32_t min[] = {100, 50, 0}, max[] = {4000, 2000, 60};
  for (int i = 0; i < parsed.count(); ++i) {
    String value = parsed.arg(i);
    if (value.length() == 0)
      return "Error: missing tone argument";
    for (unsigned j = 0; j < value.length(); ++j)
      if (value[j] < '0' || value[j] > '9')
        return "Error: tone arguments must be unsigned integers";
    if (value.length() > 4)
      return "Error: tone argument out of range";
    values[i] = strtoul(value.c_str(), nullptr, 10);
    if (values[i] < min[i] || values[i] > max[i])
      return "Error: tone argument out of range";
  }
  RETURN_VALID_IF_VALIDATE_CSTR();
  const esp_err_t result = speakerPlayTone(values[0], values[1], values[2]);
  return result == ESP_OK ? "Speaker tone completed" : esp_err_to_name(result);
}
} // namespace
extern const CommandEntry speakerCommands[] = {
    {"speakertone", "Play a short speaker test tone.", false, tone,
     "Usage: speakertone [100..4000 Hz] [50..2000 ms] [0..60 volume]"}};
extern const size_t speakerCommandsCount =
    sizeof(speakerCommands) / sizeof(speakerCommands[0]);
#endif
