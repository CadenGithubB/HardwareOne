// ArduinoJson - https://arduinojson.org
// Copyright © 2014-2025, Benoit BLANCHON
// MIT License

#pragma once

#include <stdint.h>
#include <string.h>  // for strlen
#include <stdio.h>   // HardwareOne defensive patch: printf for NULL-guard diagnostics

#include <ArduinoJson/Json/EscapeSequence.hpp>
#include <ArduinoJson/Numbers/FloatParts.hpp>
#include <ArduinoJson/Numbers/JsonInteger.hpp>
#include <ArduinoJson/Polyfills/assert.hpp>
#include <ArduinoJson/Polyfills/attributes.hpp>
#include <ArduinoJson/Polyfills/type_traits.hpp>
#include <ArduinoJson/Serialization/CountingDecorator.hpp>

ARDUINOJSON_BEGIN_PRIVATE_NAMESPACE

template <typename TWriter>
class TextFormatter {
 public:
  explicit TextFormatter(TWriter writer) : writer_(writer) {}

  TextFormatter& operator=(const TextFormatter&) = delete;

  // Returns the number of bytes sent to the TWriter implementation.
  size_t bytesWritten() const {
    return writer_.count();
  }

  void writeBoolean(bool value) {
    if (value)
      writeRaw("true");
    else
      writeRaw("false");
  }

  void writeString(const char* value) {
    // HardwareOne defensive patch (2026-05-18): handle NULL gracefully
    // instead of crashing in *value/strlen. The hardwareone codebase has
    // at least one path that stores a NULL const char* into a JsonVariant
    // (root cause still being chased); without this guard the serializer
    // crashes with LoadProhibited at strlen(NULL) inside writeRaw.
    ARDUINOJSON_ASSERT(value != NULL);
    if (!value) {
      writeRaw("null");  // emit JSON null instead of crashing
      printf("\n[ArduinoJson] writeString(const char*) got NULL — emitting 'null'");
      return;
    }
    writeRaw('\"');
    while (*value)
      writeChar(*value++);
    writeRaw('\"');
  }

  void writeString(const char* value, size_t n) {
    // HardwareOne defensive patch (2026-05-18): handle NULL gracefully.
    ARDUINOJSON_ASSERT(value != NULL);
    if (!value) {
      writeRaw("null");
      printf("\n[ArduinoJson] writeString(const char*, %zu) got NULL — emitting 'null'", n);
      return;
    }
    writeRaw('\"');
    while (n--)
      writeChar(*value++);
    writeRaw('\"');
  }

  void writeChar(char c) {
    char specialChar = EscapeSequence::escapeChar(c);
    if (specialChar) {
      writeRaw('\\');
      writeRaw(specialChar);
    } else if (c) {
      writeRaw(c);
    } else {
      writeRaw("\\u0000");
    }
  }

  template <typename T>
  void writeFloat(T value) {
    writeFloat(JsonFloat(value), sizeof(T) >= 8 ? 9 : 6);
  }

  void writeFloat(JsonFloat value, int8_t decimalPlaces) {
    if (isnan(value))
      return writeRaw(ARDUINOJSON_ENABLE_NAN ? "NaN" : "null");

#if ARDUINOJSON_ENABLE_INFINITY
    if (value < 0.0) {
      writeRaw('-');
      value = -value;
    }

    if (isinf(value))
      return writeRaw("Infinity");
#else
    if (isinf(value))
      return writeRaw("null");

    if (value < 0.0) {
      writeRaw('-');
      value = -value;
    }
#endif

    auto parts = decomposeFloat(value, decimalPlaces);

    writeInteger(parts.integral);
    if (parts.decimalPlaces)
      writeDecimals(parts.decimal, parts.decimalPlaces);

    if (parts.exponent) {
      writeRaw('e');
      writeInteger(parts.exponent);
    }
  }

  template <typename T>
  enable_if_t<is_signed<T>::value> writeInteger(T value) {
    using unsigned_type = make_unsigned_t<T>;
    unsigned_type unsigned_value;
    if (value < 0) {
      writeRaw('-');
      unsigned_value = unsigned_type(unsigned_type(~value) + 1);
    } else {
      unsigned_value = unsigned_type(value);
    }
    writeInteger(unsigned_value);
  }

  template <typename T>
  enable_if_t<is_unsigned<T>::value> writeInteger(T value) {
    char buffer[22];
    char* end = buffer + sizeof(buffer);
    char* begin = end;

    // write the string in reverse order
    do {
      *--begin = char(value % 10 + '0');
      value = T(value / 10);
    } while (value);

    // and dump it in the right order
    writeRaw(begin, end);
  }

  void writeDecimals(uint32_t value, int8_t width) {
    // buffer should be big enough for all digits and the dot
    char buffer[16];
    char* end = buffer + sizeof(buffer);
    char* begin = end;

    // write the string in reverse order
    while (width--) {
      *--begin = char(value % 10 + '0');
      value /= 10;
    }
    *--begin = '.';

    // and dump it in the right order
    writeRaw(begin, end);
  }

  void writeRaw(const char* s) {
    // HardwareOne defensive patch (2026-05-18): NULL-check before strlen.
    // This is the exact crash site decoded from the 2026-05-18 field-test
    // panic (LoadProhibited, EXCVADDR=0, PC inside strlen). A NULL const
    // char* was reaching this overload through writeChar's '\\u0000' literal
    // path or a JsonVariant with stored NULL string. Skipping the write
    // produces no output (caller's responsibility to never pass NULL).
    if (!s) {
      printf("\n[ArduinoJson] writeRaw(NULL) — skipping");
      return;
    }
    writer_.write(reinterpret_cast<const uint8_t*>(s), strlen(s));
  }

  void writeRaw(const char* s, size_t n) {
    if (!s && n > 0) {
      printf("\n[ArduinoJson] writeRaw(NULL, %zu) — skipping", n);
      return;
    }
    writer_.write(reinterpret_cast<const uint8_t*>(s), n);
  }

  void writeRaw(const char* begin, const char* end) {
    writer_.write(reinterpret_cast<const uint8_t*>(begin),
                  static_cast<size_t>(end - begin));
  }

  template <size_t N>
  void writeRaw(const char (&s)[N]) {
    writer_.write(reinterpret_cast<const uint8_t*>(s), N - 1);
  }
  void writeRaw(char c) {
    writer_.write(static_cast<uint8_t>(c));
  }

 protected:
  CountingDecorator<TWriter> writer_;
};

ARDUINOJSON_END_PRIVATE_NAMESPACE
