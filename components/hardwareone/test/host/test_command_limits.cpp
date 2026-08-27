#include <assert.h>
#include <stdio.h>

#include "../../System_CommandLimits.h"

static_assert(CMD_INPUT_MAX == 2047, "wire/executor input contract changed");
static_assert(CMD_RESULT_MAX == 4096, "command result contract changed");

int main() {
  assert(!commandInputLengthAccepted(0));
  assert(commandInputLengthAccepted(1));
  assert(commandInputLengthAccepted(CMD_INPUT_MAX));
  assert(!commandInputLengthAccepted(CMD_INPUT_MAX + 1));

  char line[CMD_INPUT_MAX + 1]{};
  assert(sizeof(line) == 2048);
  char result[CMD_RESULT_MAX]{};
  assert(sizeof(result) == 4096);
  puts("command-limit tests passed");
  return 0;
}
