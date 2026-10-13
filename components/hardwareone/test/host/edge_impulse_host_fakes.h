#pragma once
// Host doubles for compiling the whole of System_EdgeImpulse.cpp (and its
// header) in the Edge Impulse suites. The runners strip the module's #include
// lines and insert its text after this header, so every dependency the module
// names is declared here: a fake TFLite Micro interpreter driven by a test
// model spec, FreeRTOS semaphores and tasks on std::thread, an in-memory file
// store with the File/VFS calls the module makes, the camera capture call,
// settings, command and httpd types, and the HAL_JPEG backends (the real
// HAL_JPEG.cpp front end is linked; only the codec backend is faked).
//
// The doubles cover the dependencies of both the current module and the
// f4cc542 base (fmt2rgb888, executeCommandThroughRegistry, the inputSize
// setting), so the same scenarios run against either source.
//
// Not modelled: real TFLite kernels, tensor-arena placement, FreeRTOS
// priorities or scheduling, the real filesystem, camera hardware, httpd.

#define ARDUINOJSON_ENABLE_ARDUINO_STRING 1
#define ARDUINOJSON_ENABLE_ARDUINO_STREAM 0
#define ARDUINOJSON_ENABLE_ARDUINO_PRINT 0
#define ARDUINOJSON_ENABLE_PROGMEM 0
#include <Arduino.h>
#include <ArduinoJson.h>

#include "HAL_JPEG.h"
#include "HAL_JPEG_Backend.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <strings.h>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Test reporting
// ---------------------------------------------------------------------------
inline void eiCheck(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "CHECK FAILED: %s\n", what);
    std::fflush(stderr);
    std::_Exit(1);  // other threads may be parked inside the fake interpreter
  }
}

// ---------------------------------------------------------------------------
// Platform odds and ends
// ---------------------------------------------------------------------------
template <typename... Args>
inline void eiTestLogSink(const char*, const Args&...) {}
#define DEBUG_SYSTEMF(...) eiTestLogSink(__VA_ARGS__)
#define INFO_SYSTEMF(...) eiTestLogSink(__VA_ARGS__)
#define ERROR_SYSTEMF(...) eiTestLogSink(__VA_ARGS__)
#define EXT_RAM_BSS_ATTR
#define RETURN_VALID_IF_VALIDATE_CSTR() ((void)0)

struct FakeEsp {
  uint32_t getFreeHeap() const { return 200000; }
  uint32_t getFreePsram() const { return 4000000; }
};
inline FakeEsp ESP;
inline bool psramFound() { return true; }
inline int xPortGetCoreID() { return 1; }

inline std::atomic<uint32_t> gFakeMillis{1000};
inline uint32_t millis() { return gFakeMillis.load(); }

enum class AllocPref : uint8_t {
  PreferPSRAM = 0, PreferInternal = 1, RequirePSRAM = 2, RequireInternal = 3, DefaultHeap = 4
};
inline std::atomic<int> gFakePsAllocs{0};
inline void* ps_alloc(size_t size, AllocPref, const char*) {
  ++gFakePsAllocs;
  return std::malloc(size);
}
#define PSRAM_JSON_DOC(name) JsonDocument name

// Arduino min/max as used by older module code (after ArduinoJson so its own
// helpers are unaffected).
template <typename T>
inline T max(T a, T b) { return a < b ? b : a; }

// ---------------------------------------------------------------------------
// FreeRTOS: semaphores and tasks
// ---------------------------------------------------------------------------
using TickType_t = uint32_t;
using BaseType_t = int;
using UBaseType_t = unsigned;
using TaskHandle_t = void*;
constexpr BaseType_t pdTRUE = 1;
constexpr BaseType_t pdFALSE = 0;
constexpr BaseType_t pdPASS = 1;
constexpr TickType_t portMAX_DELAY = 0xffffffffu;
#define pdMS_TO_TICKS(ms) (static_cast<TickType_t>(ms))

struct StaticSemaphore_t {
  std::mutex m;
  std::condition_variable cv;
  int count = 0;
  int max = 1;
};
using SemaphoreHandle_t = StaticSemaphore_t*;
inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* s) {
  s->count = 1; s->max = 1; return s;
}
inline SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t* s) {
  s->count = 0; s->max = 1; return s;
}
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait) {
  std::unique_lock<std::mutex> lock(s->m);
  auto ready = [s] { return s->count > 0; };
  if (wait == portMAX_DELAY) {
    s->cv.wait(lock, ready);
  } else if (!s->cv.wait_for(lock, std::chrono::milliseconds(wait), ready)) {
    return pdFALSE;
  }
  --s->count;
  return pdTRUE;
}
inline BaseType_t xSemaphoreGive(SemaphoreHandle_t s) {
  std::lock_guard<std::mutex> lock(s->m);
  if (s->count >= s->max) return pdFALSE;
  ++s->count;
  s->cv.notify_all();
  return pdTRUE;
}

// Each fake task is a std::thread with its own id (0 = not a fake task).
inline thread_local int tFakeTaskId = 0;
inline std::mutex gFakeTaskMutex;
inline std::vector<std::thread> gFakeTaskThreads;
inline std::atomic<int> gFakeTasksCreated{0};
inline std::atomic<int> gFakeTasksRunning{0};
constexpr uint32_t EI_CONTINUOUS_STACK_WORDS = 8192;
constexpr BaseType_t APP_CORE = 1;
inline void taskStackRecord(const char*, uint32_t) {}
inline BaseType_t xTaskCreatePinnedToCore(void (*fn)(void*), const char*, uint32_t, void* arg,
                                          UBaseType_t, TaskHandle_t* handle, BaseType_t) {
  const int id = ++gFakeTasksCreated;
  if (handle) *handle = reinterpret_cast<TaskHandle_t>(static_cast<uintptr_t>(id));
  ++gFakeTasksRunning;
  std::lock_guard<std::mutex> lock(gFakeTaskMutex);
  gFakeTaskThreads.emplace_back([fn, arg, id] {
    tFakeTaskId = id;
    fn(arg);
    --gFakeTasksRunning;
  });
  return pdPASS;
}
inline void vTaskDelete(TaskHandle_t) {}
inline void vTaskDelay(TickType_t ticks) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ticks));
}
inline void joinFakeTasks() {
  std::vector<std::thread> threads;
  {
    std::lock_guard<std::mutex> lock(gFakeTaskMutex);
    threads.swap(gFakeTaskThreads);
  }
  for (auto& thread : threads) thread.join();
}
inline void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
// Polls `condition` for up to `ms`; returns whether it became true.
inline bool waitFor(const std::function<bool()>& condition, int ms) {
  for (int waited = 0; waited < ms; waited += 5) {
    if (condition()) return true;
    sleepMs(5);
  }
  return condition();
}

// ---------------------------------------------------------------------------
// Settings, commands, events, auth
// ---------------------------------------------------------------------------
struct Settings {
  bool eiAutoStart = true;
  bool eiEnabled = true;
  bool edgeImpulseRequireLabels = true;
  float edgeImpulseMinConfidence = 0.6f;
  int edgeImpulseMaxDetections = 5;
  int edgeImpulseInputSize = 96;
  bool edgeImpulseContinuous = false;
  int edgeImpulseIntervalMs = 1000;
};
inline Settings gSettings;
inline std::atomic<int> gFakeSettingWrites{0};
template <typename T, typename U>
inline void setSetting(T& field, U value) {
  field = static_cast<T>(value);
  ++gFakeSettingWrites;
}
enum SettingType { SETTING_BOOL, SETTING_INT, SETTING_FLOAT, SETTING_STRING };
struct SettingEntry {
  const char* jsonKey;
  SettingType type;
  void* valuePtr;
  int intDefault;
  float floatDefault;
  const char* stringDefault;
  int minVal;
  int maxVal;
  const char* label;
  const char* const* options;
  bool isSecret;
  const char* group;
  const char* cmdKey;
};
using ConnectionCheckFunc = bool (*)();
struct SettingsModule {
  const char* name;
  const char* jsonSection;
  const SettingEntry* entries;
  size_t count;
  ConnectionCheckFunc isConnected;
  const char* description;
};
struct CommandEntry {
  const char* name;
  const char* help;
  bool requiresAdmin;
  const char* (*handler)(const String& argsInput);
  const char* usage;
  bool requiresSuperAdmin;
};
// Base f4cc542 dispatched `eicontinuous 0`/`eimodelunload` through the
// registry; each harness defines this over edgeImpulseCommands.
String executeCommandThroughRegistry(const String& line);

enum { SYSEVT_EI_DETECTED = 1, SYSEVT_EI_LOST, SYSEVT_EI_CONTINUOUS_STARTED };
inline std::atomic<int> gFakeEvents{0};
inline void systemEventPost(uint8_t, const char* = nullptr, const char* = nullptr) { ++gFakeEvents; }
inline void sensorStatusBumpWith(const char*) {}

struct AuthContext { int transport = 0; };
inline const AuthContext& currentAuthContext() {
  static AuthContext context;
  return context;
}

class CommandArgs {
 public:
  explicit CommandArgs(const String& line) {
    const std::string s = line.c_str();
    size_t i = 0;
    while (i < s.size()) {
      while (i < s.size() && s[i] == ' ') ++i;
      if (i >= s.size()) break;
      std::string token;
      if (s[i] == '"') {
        const size_t end = s.find('"', i + 1);
        token = s.substr(i, end == std::string::npos ? std::string::npos : end - i + 1);
        i = end == std::string::npos ? s.size() : end + 1;
      } else {
        const size_t end = s.find(' ', i);
        token = s.substr(i, end == std::string::npos ? std::string::npos : end - i);
        i = end == std::string::npos ? s.size() : end;
      }
      tokens.push_back(token);
    }
  }
  bool has(int index) const { return index >= 0 && static_cast<size_t>(index) < tokens.size(); }
  std::vector<std::string> tokens;
};
inline const char* requireQuotedToken(const CommandArgs& a, int index, String& out) {
  if (!a.has(index)) return "Error: missing argument";
  const std::string& token = a.tokens[static_cast<size_t>(index)];
  if (token.size() < 2 || token.front() != '"' || token.back() != '"') return "Error: quote it";
  out = String(token.substr(1, token.size() - 2));
  return nullptr;
}
inline const char* requireQuotedPath(const CommandArgs& a, int index, String& out) {
  return requireQuotedToken(a, index, out);
}

// ---------------------------------------------------------------------------
// In-memory files: File, FsLockGuard and the VFS calls the module makes
// ---------------------------------------------------------------------------
inline std::mutex gFakeFsMutex;
inline std::map<std::string, std::string> gFakeFiles;
inline std::set<std::string> gFakeDirs{"/"};
inline void fakePutFile(const std::string& path, const std::string& bytes) {
  std::lock_guard<std::mutex> lock(gFakeFsMutex);
  gFakeFiles[path] = bytes;
  for (size_t slash = path.find('/', 1); slash != std::string::npos; slash = path.find('/', slash + 1)) {
    gFakeDirs.insert(path.substr(0, slash));
  }
}

class File {
 public:
  File() = default;
  static File openFile(const std::string& path, const std::string& data) {
    File f; f.valid_ = true; f.path_ = path; f.data_ = data; return f;
  }
  static File openDir(const std::string& path, std::vector<std::string> children) {
    File f; f.valid_ = true; f.dir_ = true; f.path_ = path; f.children_ = std::move(children); return f;
  }
  explicit operator bool() const { return valid_; }
  size_t size() const { return data_.size(); }
  size_t read(uint8_t* buffer, size_t length) {
    const size_t n = std::min(length, data_.size() - pos_);
    std::memcpy(buffer, data_.data() + pos_, n);
    pos_ += n;
    return n;
  }
  int available() const { return static_cast<int>(data_.size() - pos_); }
  String readStringUntil(char terminator) {
    const size_t end = data_.find(terminator, pos_);
    std::string line = data_.substr(pos_, end == std::string::npos ? std::string::npos : end - pos_);
    pos_ = end == std::string::npos ? data_.size() : end + 1;
    return String(line);
  }
  void close() { valid_ = false; }
  bool isDirectory() const { return dir_; }
  const char* name() const { return path_.c_str(); }
  File openNextFile();

 private:
  bool valid_ = false;
  bool dir_ = false;
  std::string path_;
  std::string data_;
  size_t pos_ = 0;
  std::vector<std::string> children_;
  size_t next_ = 0;
};

struct FsLockGuard {
  explicit FsLockGuard(const char* = nullptr) {}
  ~FsLockGuard() {}
  FsLockGuard(const FsLockGuard&) = delete;
  FsLockGuard& operator=(const FsLockGuard&) = delete;
};

namespace VFS {
inline AuthContext systemAuth(const char*) { return AuthContext{}; }
inline bool existsGuarded(const String& path, const AuthContext&) {
  std::lock_guard<std::mutex> lock(gFakeFsMutex);
  return gFakeFiles.count(path.c_str()) != 0 || gFakeDirs.count(path.c_str()) != 0;
}
inline bool mkdirGuarded(const String& path, const AuthContext&) {
  std::lock_guard<std::mutex> lock(gFakeFsMutex);
  gFakeDirs.insert(path.c_str());
  return true;
}
inline bool renameGuarded(const String& from, const String& to, const AuthContext&) {
  std::lock_guard<std::mutex> lock(gFakeFsMutex);
  auto it = gFakeFiles.find(from.c_str());
  if (it == gFakeFiles.end()) return false;
  gFakeFiles[to.c_str()] = it->second;
  gFakeFiles.erase(it);
  return true;
}
inline File openGuarded(const String& path, const char*, const AuthContext&) {
  std::lock_guard<std::mutex> lock(gFakeFsMutex);
  const std::string p = path.c_str();
  auto it = gFakeFiles.find(p);
  if (it != gFakeFiles.end()) return File::openFile(p, it->second);
  if (gFakeDirs.count(p)) {
    std::vector<std::string> children;
    const std::string prefix = p == "/" ? "/" : p + "/";
    for (const auto& entry : gFakeFiles) {
      if (entry.first.rfind(prefix, 0) == 0 && entry.first.find('/', prefix.size()) == std::string::npos) {
        children.push_back(entry.first);
      }
    }
    for (const auto& dir : gFakeDirs) {
      if (dir != p && dir.rfind(prefix, 0) == 0 && dir.find('/', prefix.size()) == std::string::npos) {
        children.push_back(dir);
      }
    }
    return File::openDir(p, children);
  }
  return File();
}
}  // namespace VFS

inline File File::openNextFile() {
  if (!dir_ || next_ >= children_.size()) return File();
  return VFS::openGuarded(String(children_[next_++]), "r", AuthContext{});
}

// ---------------------------------------------------------------------------
// Camera
// ---------------------------------------------------------------------------
inline bool cameraConnected = true;
inline bool gCameraRunning = true;
inline std::vector<uint8_t> gFakeCameraJpeg;
inline std::atomic<int> gFakeCaptures{0};
inline uint8_t* captureFrame(size_t* outLen) {
  ++gFakeCaptures;
  *outLen = gFakeCameraJpeg.size();
  if (gFakeCameraJpeg.empty()) return nullptr;
  auto* copy = static_cast<uint8_t*>(std::malloc(gFakeCameraJpeg.size()));
  std::memcpy(copy, gFakeCameraJpeg.data(), gFakeCameraJpeg.size());
  return copy;
}

// ---------------------------------------------------------------------------
// JPEG: real HAL_JPEG.cpp front end, faked codec backends, and the base
// module's esp32-camera converter. Every decoded pixel is gFakeDecodeRgb.
// ---------------------------------------------------------------------------
inline uint8_t gFakeDecodeRgb[3] = {200, 100, 50};
inline std::atomic<int> gFakeSoftwareDecodes{0};
inline std::atomic<int> gFakeLegacyConversions{0};
inline bool gFakeDecodeFails = false;
inline void fakeFillRgb(uint8_t* rgb, size_t bytes) {
  for (size_t i = 0; i < bytes; ++i) rgb[i] = gFakeDecodeRgb[i % 3];
}
namespace hwjpeg { namespace detail {
bool decodeHardware(const uint8_t*, size_t, const Info&, Image& image, const char**) {
  image.reset();
  return false;  // no hardware codec on the host; Auto falls back to software
}
bool validateSoftwareEntropy(const uint8_t*, size_t, const Info&, const char** error) {
  if (error) *error = "not used";
  return false;
}
bool decodeSoftware(const uint8_t*, size_t, const Info& info, Image& image, const char** error,
                    OutputAllocator allocator) {
  ++gFakeSoftwareDecodes;
  image.reset();
  if (gFakeDecodeFails) {
    if (error) *error = "software JPEG decode failed";
    return false;
  }
  uint8_t* pixels = allocator ? allocator(info.bytes) : static_cast<uint8_t*>(std::malloc(info.bytes));
  if (!pixels) {
    if (error) *error = "out of memory decoding JPEG";
    return false;
  }
  fakeFillRgb(pixels, info.bytes);
  image.pixels = pixels;
  image.width = info.width;
  image.height = info.height;
  image.stride = size_t(info.width) * 3;
  image.size = info.bytes;
  image.backend = Backend::Software;
  return true;
}
} }  // namespace hwjpeg::detail

constexpr int PIXFORMAT_JPEG = 4;
inline bool fmt2rgb888(const uint8_t* jpeg, size_t length, int, uint8_t* rgb) {
  ++gFakeLegacyConversions;
  hwjpeg::Info info;
  if (!hwjpeg::inspect(jpeg, length, info)) return false;
  if (gFakeDecodeFails) return false;
  fakeFillRgb(rgb, info.bytes);  // no capacity argument, as in esp32-camera
  return true;
}

// ---------------------------------------------------------------------------
// TensorFlow Lite Micro
// ---------------------------------------------------------------------------
enum TfLiteType {
  kTfLiteNoType = 0, kTfLiteFloat32 = 1, kTfLiteInt32 = 2, kTfLiteUInt8 = 3,
  kTfLiteInt64 = 4, kTfLiteString = 5, kTfLiteBool = 6, kTfLiteInt16 = 7, kTfLiteInt8 = 9
};
enum TfLiteStatus { kTfLiteOk = 0, kTfLiteError = 1 };
struct TfLiteIntArray { int size; int data[6]; };
struct TfLiteQuantizationParams { float scale; int32_t zero_point; };
union TfLitePtrUnion {
  int32_t* i32;
  float* f;
  uint8_t* uint8;
  int8_t* int8;
  void* raw;
};
struct TfLiteTensor {
  TfLiteType type;
  TfLitePtrUnion data;
  TfLiteIntArray* dims;
  TfLiteQuantizationParams params;
  size_t bytes;
};
#define TFLITE_SCHEMA_VERSION (3u)

// What the next interpreter looks like and computes. Output values are given
// in dequantized form and quantized with the output scale/zero point.
struct FakeTensorSpec {
  TfLiteType type = kTfLiteUInt8;
  std::vector<int> dims;
  float scale = 1.0f / 255.0f;
  int zeroPoint = 0;
};
struct FakeModelSpec {
  FakeTensorSpec input;
  FakeTensorSpec output;
  std::vector<float> outputValues;  // one per output element
};
inline FakeModelSpec gFakeModel;

inline size_t fakeElementBytes(TfLiteType type) {
  return type == kTfLiteFloat32 ? 4 : 1;
}
inline size_t fakeElements(const std::vector<int>& dims) {
  size_t n = 1;
  for (int d : dims) n *= static_cast<size_t>(d);
  return n;
}

// Interpreter bookkeeping the scenarios assert on.
constexpr size_t kFakeGuardBytes = 64;
inline std::atomic<int> gFakeLiveInterpreters{0};
inline std::atomic<int> gFakeInvokeInflight{0};
inline std::atomic<int> gFakeInvokes{0};
inline std::atomic<bool> gFakeConcurrentInvoke{false};
inline std::atomic<bool> gFakeDestroyedDuringInvoke{false};
inline std::atomic<bool> gFakeGuardBroken{false};
inline std::atomic<bool> gFakeBlockNextInvoke{false};
inline std::atomic<bool> gFakeInvokeBlocked{false};
inline std::atomic<bool> gFakeReleaseInvoke{false};
inline std::mutex gFakeInvokeLog;
inline std::vector<int> gFakeInvokeTaskIds;          // tFakeTaskId per Invoke
inline std::vector<uint64_t> gFakeInvokeInputHashes;  // FNV-1a of input bytes

namespace tflite {
class MicroOpResolver {
 public:
  virtual ~MicroOpResolver() = default;
};
template <unsigned int N>
class MicroMutableOpResolver : public MicroOpResolver {
 public:
  TfLiteStatus AddConv2D() { return kTfLiteOk; }
  TfLiteStatus AddDepthwiseConv2D() { return kTfLiteOk; }
  TfLiteStatus AddFullyConnected() { return kTfLiteOk; }
  TfLiteStatus AddReshape() { return kTfLiteOk; }
  TfLiteStatus AddSoftmax() { return kTfLiteOk; }
  TfLiteStatus AddMaxPool2D() { return kTfLiteOk; }
  TfLiteStatus AddAveragePool2D() { return kTfLiteOk; }
  TfLiteStatus AddQuantize() { return kTfLiteOk; }
  TfLiteStatus AddDequantize() { return kTfLiteOk; }
  TfLiteStatus AddMean() { return kTfLiteOk; }
  TfLiteStatus AddPad() { return kTfLiteOk; }
  TfLiteStatus AddPadV2() { return kTfLiteOk; }
  TfLiteStatus AddAdd() { return kTfLiteOk; }
  TfLiteStatus AddMul() { return kTfLiteOk; }
  TfLiteStatus AddRelu() { return kTfLiteOk; }
  TfLiteStatus AddRelu6() { return kTfLiteOk; }
  TfLiteStatus AddLogistic() { return kTfLiteOk; }
  TfLiteStatus AddConcatenation() { return kTfLiteOk; }
  TfLiteStatus AddSplit() { return kTfLiteOk; }
  TfLiteStatus AddSplitV() { return kTfLiteOk; }
};
struct Model {
  uint32_t version() const { return TFLITE_SCHEMA_VERSION; }
};
inline const Model* GetModel(const void*) {
  static Model model;
  return &model;
}

class MicroInterpreter {
 public:
  MicroInterpreter(const Model*, const MicroOpResolver&, uint8_t*, size_t) {
    ++gFakeLiveInterpreters;
  }
  ~MicroInterpreter() {
    if (gFakeInvokeInflight.load() > 0) gFakeDestroyedDuringInvoke = true;
    checkGuards();
    std::free(inputData_);
    std::free(outputData_);
    --gFakeLiveInterpreters;
  }
  TfLiteStatus AllocateTensors() {
    setUp(input_, inputDims_, inputData_, gFakeModel.input);
    setUp(output_, outputDims_, outputData_, gFakeModel.output);
    return kTfLiteOk;
  }
  size_t arena_used_bytes() const { return 4096; }
  size_t inputs_size() const { return 1; }
  size_t outputs_size() const { return 1; }
  TfLiteTensor* input(size_t) { return &input_; }
  TfLiteTensor* output(size_t) { return &output_; }
  TfLiteStatus Invoke() {
    if (++gFakeInvokeInflight > 1) gFakeConcurrentInvoke = true;
    ++gFakeInvokes;
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < input_.bytes; ++i) hash = (hash ^ inputData_[i]) * 1099511628211ull;
    {
      std::lock_guard<std::mutex> lock(gFakeInvokeLog);
      gFakeInvokeTaskIds.push_back(tFakeTaskId);
      gFakeInvokeInputHashes.push_back(hash);
    }
    if (gFakeBlockNextInvoke.exchange(false)) {
      gFakeInvokeBlocked = true;
      while (!gFakeReleaseInvoke.load()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
      gFakeInvokeBlocked = false;
    }
    checkGuards();
    const size_t elements = fakeElements(gFakeModel.output.dims);
    for (size_t i = 0; i < elements; ++i) {
      const float v = i < gFakeModel.outputValues.size() ? gFakeModel.outputValues[i] : 0.0f;
      switch (output_.type) {
        case kTfLiteFloat32: output_.data.f[i] = v; break;
        case kTfLiteUInt8:
          output_.data.uint8[i] = static_cast<uint8_t>(v / output_.params.scale + output_.params.zero_point + 0.5f);
          break;
        case kTfLiteInt8:
          output_.data.int8[i] = static_cast<int8_t>(std::lround(v / output_.params.scale) + output_.params.zero_point);
          break;
        default: break;
      }
    }
    --gFakeInvokeInflight;
    return kTfLiteOk;
  }
  // Copy of the current input bytes (for checking what the module wrote).
  std::vector<uint8_t> inputBytes() const { return std::vector<uint8_t>(inputData_, inputData_ + input_.bytes); }

 private:
  static void setUp(TfLiteTensor& tensor, TfLiteIntArray& dims, uint8_t*& data, const FakeTensorSpec& spec) {
    dims.size = static_cast<int>(spec.dims.size());
    for (size_t i = 0; i < spec.dims.size(); ++i) dims.data[i] = spec.dims[i];
    tensor.type = spec.type;
    tensor.dims = &dims;
    tensor.params.scale = spec.scale;
    tensor.params.zero_point = spec.zeroPoint;
    tensor.bytes = fakeElements(spec.dims) * fakeElementBytes(spec.type);
    // Exact tensor size plus a guard band: a write past `bytes` breaks the
    // guard (and, past the band, trips ASan).
    data = static_cast<uint8_t*>(std::malloc(tensor.bytes + kFakeGuardBytes));
    std::memset(data, 0x11, tensor.bytes);
    std::memset(data + tensor.bytes, 0xA5, kFakeGuardBytes);
    tensor.data.raw = data;
  }
  void checkGuards() const {
    for (size_t i = 0; i < kFakeGuardBytes; ++i) {
      if ((inputData_ && inputData_[input_.bytes + i] != 0xA5) ||
          (outputData_ && outputData_[output_.bytes + i] != 0xA5)) {
        gFakeGuardBroken = true;
      }
    }
  }
  TfLiteTensor input_{};
  TfLiteTensor output_{};
  TfLiteIntArray inputDims_{};
  TfLiteIntArray outputDims_{};
  uint8_t* inputData_ = nullptr;
  uint8_t* outputData_ = nullptr;
};
}  // namespace tflite

// ---------------------------------------------------------------------------
// httpd
// ---------------------------------------------------------------------------
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
using httpd_handle_t = void*;
constexpr long HTTPD_RESP_USE_STRLEN = -1;
struct httpd_req_t {
  std::string type;
  std::string body;
};
inline esp_err_t httpd_resp_set_type(httpd_req_t* req, const char* type) { req->type = type; return ESP_OK; }
inline esp_err_t httpd_resp_set_hdr(httpd_req_t*, const char*, const char*) { return ESP_OK; }
inline esp_err_t httpd_resp_send(httpd_req_t* req, const char* body, long length) {
  req->body.assign(body, length < 0 ? std::strlen(body) : static_cast<size_t>(length));
  return ESP_OK;
}
inline bool filesystemReady = true;
#define WEB_AUTH_OR_RETURN(req, ctx) \
  AuthContext ctx{};                 \
  (void)ctx;                         \
  (void)(req)

// ---------------------------------------------------------------------------
// Scenario helpers
// ---------------------------------------------------------------------------
inline std::vector<uint8_t> readFixture(const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  eiCheck(f != nullptr, "fixture opens");
  std::vector<uint8_t> bytes;
  int c;
  while ((c = std::fgetc(f)) != EOF) bytes.push_back(static_cast<uint8_t>(c));
  std::fclose(f);
  return bytes;
}
// A model file (any non-empty bytes; the fake parser ignores them) plus an
// optional labels file next to it.
inline void installModel(const std::string& path, const std::vector<std::string>& labels) {
  fakePutFile(path, std::string(64, 'm'));
  if (!labels.empty()) {
    std::string text;
    for (const auto& label : labels) text += label + "\n";
    std::string stem = path.substr(0, path.size() - 7);  // drop .tflite
    fakePutFile(stem + ".labels.txt", text);
  }
}
