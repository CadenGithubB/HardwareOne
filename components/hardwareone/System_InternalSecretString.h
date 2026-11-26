#pragma once
#include "System_AllocPref.h"
#include <Arduino.h>
#include <cstring>
#include <utility>

// A String-compatible owning buffer for the settings registry. Returning it
// by move preserves its internal allocation; callers must also move on load.
// Empty values stay in the String object's small-string storage.
inline String makeInternalSecretString(const char* text,size_t length) {
  if(!text || !length || length>128)return String("");
  struct Owned : String {
    bool assign(const char* data,size_t size) {
      char* memory=static_cast<char*>(ps_alloc(size+1,AllocPref::RequireInternal,"secret.string"));
      if(!memory)return false;
      memcpy(memory,data,size);memory[size]=0;
      setSSO(false);setBuffer(memory);setCapacity(size);setLen(size);return true;
    }
  } value;
  if(!value.assign(text,length))return String("");
  return String(static_cast<String&&>(value));
}
