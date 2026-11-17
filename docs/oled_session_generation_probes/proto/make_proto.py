"""PROTOTYPE (not installed): derive a generation-fenced OledConsoleBuffer from
the shipping header and methods by targeted substitutions, so the prototype
differs from production only where the design differs."""
import sys
from pathlib import Path
HOST = Path.home() / "esp/hardwareone-idf/components/hardwareone/test/host"
sys.path.insert(0, str(HOST))
from test_oled_console_buffer import production_methods, COMPONENT  # noqa: E402

def sub(text, old, new, label):
    assert text.count(old) == 1, f"{label}: expected exactly one occurrence of {old!r}"
    return text.replace(old, new)

def proto_header() -> str:
    h = (COMPONENT / "OLED_ConsoleBuffer.h").read_text()
    h = sub(h, "  void append(const char* text, uint32_t timestampMs);",
            "  // `deliveryGeneration` is the value deliveryGeneration() returned when the\n"
            "  // envelope entered the output queue; a stale value is refused.\n"
            "  void append(const char* text, uint32_t timestampMs, uint32_t deliveryGeneration);", "append decl")
    h = sub(h, "  uint32_t fencedCount() const;\n",
            "  uint32_t fencedCount() const;\n"
            "  // Ownership period of pending OLED output: producers capture it at enqueue\n"
            "  // (relaxed atomic load, ISR-safe); clear() advances it under the mutex.\n"
            "  uint32_t deliveryGeneration() const;\n", "gen decl")
    h = sub(h, "  uint32_t fenceMs_;   // guarded by mutex_\n  bool fenceArmed_;    // guarded by mutex_\n",
            "  std::atomic<uint32_t> delivery_;  // advanced by clear() while mutex_ is held\n", "members")
    return h

def proto_methods() -> str:
    m = production_methods()
    m = sub(m, "dropped_(0), fenced_(0), fenceMs_(0), fenceArmed_(false) {}",
            "dropped_(0), fenced_(0), delivery_(0) {}", "ctor")
    m = sub(m, "void OledConsoleBuffer::append(const char* text, uint32_t timestampMs) {",
            "void OledConsoleBuffer::append(const char* text, uint32_t timestampMs, uint32_t deliveryGeneration) {", "append def")
    m = sub(m, "  if (fenceArmed_ && (int32_t)(timestampMs - fenceMs_) <= 0) {",
            "  if (deliveryGeneration != delivery_.load(std::memory_order_relaxed)) {", "fence")
    m = sub(m, "  fenceMs_ = millis();\n  fenceArmed_ = true;\n",
            "  delivery_.fetch_add(1, std::memory_order_release);\n", "clear")
    m += ("\n\nuint32_t OledConsoleBuffer::deliveryGeneration() const {\n"
          "  return delivery_.load(std::memory_order_relaxed);\n}\n")
    return m

if __name__ == "__main__":
    out = Path(sys.argv[1])
    (out / "OLED_ConsoleBuffer.h").write_text(proto_header())
    (out / "proto_methods.inc").write_text(proto_methods())
    print("prototype derived")
