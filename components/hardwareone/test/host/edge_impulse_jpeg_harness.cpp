// Edge Impulse suite driven by test_edge_impulse_jpeg.py. One source, three
// builds:
//   EI_DECODE_UNIT  the module's JPEG boundary (decodeEdgeImpulseJpeg) with
//                   the real HAL_JPEG front end and a counting fake codec;
//   EI_LINK_CHECK   the whole module with ENABLE_HTTP_SERVER=0: every entry
//                   in the command table must compile and link;
//   (default)       the whole module, one scenario per run (argv[2]): input
//                   geometry from the input tensor, per-detection labels, one
//                   pipeline for camera and stored file, web detect JSON.
// The runner inserts the production text at the INSERT markers.
#include "edge_impulse_host_fakes.h"

static std::string gFixtures;

#ifdef EI_DECODE_UNIT
// INSERT_EDGE_IMPULSE_DECODE

static void decodeAccepts(const std::vector<uint8_t>& jpeg, uint32_t width, uint32_t height) {
  hwjpeg::Image image;
  const char* error = "stale";
  const int decodes = gFakeSoftwareDecodes.load();
  const int allocations = gFakePsAllocs.load();
  eiCheck(decodeEdgeImpulseJpeg(jpeg.data(), jpeg.size(), image, &error), "an in-limit JPEG decodes");
  eiCheck(error == nullptr, "success clears the error");
  eiCheck(image.width == width && image.height == height, "decoded geometry is the JPEG's own");
  eiCheck(image.size == size_t(width) * height * 3 && image.pixels, "image owns width*height*3 bytes");
  eiCheck(gFakeSoftwareDecodes.load() == decodes + 1, "exactly one codec call");
  eiCheck(gFakePsAllocs.load() == allocations + 1, "pixels come from the module's PSRAM allocator");
  eiCheck(image.pixels[0] == gFakeDecodeRgb[0] && image.pixels[image.size - 1] == gFakeDecodeRgb[2],
          "pixels are the codec output");
}

static void decodeRejects(const uint8_t* jpeg, size_t length) {
  hwjpeg::Image image;
  const char* error = nullptr;
  const int decodes = gFakeSoftwareDecodes.load();
  eiCheck(!decodeEdgeImpulseJpeg(jpeg, length, image, &error), "rejected input does not decode");
  eiCheck(error != nullptr, "a rejection names its reason");
  eiCheck(!image.pixels && image.width == 0 && image.height == 0, "a rejection leaves the image empty");
  eiCheck(gFakeSoftwareDecodes.load() == decodes, "rejected before the codec runs");
}

static size_t marker(const std::vector<uint8_t>& jpeg, uint8_t value) {
  for (size_t i = 2; i + 1 < jpeg.size(); ++i)
    if (jpeg[i] == 0xff && jpeg[i + 1] == value) return i;
  eiCheck(false, "fixture marker present");
  return 0;
}

int main(int argc, char** argv) {
  eiCheck(argc == 2, "usage: harness <fixtures>");
  gFixtures = argv[1];
  const auto small = readFixture(gFixtures + "/rgb420_17x19.jpg");
  const auto vga = readFixture(gFixtures + "/solid_640x480.jpg");
  const auto hd = readFixture(gFixtures + "/solid_1280x720.jpg");
  decodeAccepts(small, 17, 19);
  decodeAccepts(vga, 640, 480);
  // The module keeps its VGA cap whatever the decoder's own defaults are.
  decodeRejects(hd.data(), hd.size());
  decodeRejects(nullptr, small.size());
  const uint8_t oneByte[] = {0xff};
  decodeRejects(oneByte, sizeof(oneByte));
  decodeRejects(oneByte, 0);
  decodeRejects(oneByte, static_cast<size_t>(UINT32_MAX) + 1);
  const size_t sof = marker(small, 0xc0), sos = marker(small, 0xda);
  for (size_t length = 0; length < sos + 4; ++length) {
    // Exact-size allocation detects any short-input overread under ASan.
    const std::vector<uint8_t> truncated(small.begin(), small.begin() + length);
    decodeRejects(truncated.data(), truncated.size());
  }
  auto invalid = small; invalid[0] = 0; decodeRejects(invalid.data(), invalid.size());
  invalid = small; invalid.pop_back(); decodeRejects(invalid.data(), invalid.size());
  invalid = small; invalid[sof + 2] = 0; invalid[sof + 3] = 1;
  decodeRejects(invalid.data(), invalid.size());
  invalid = small; invalid[sof + 7] = 2; invalid[sof + 8] = 129;  // width 641
  decodeRejects(invalid.data(), invalid.size());
  invalid = small; invalid[sof + 5] = 1; invalid[sof + 6] = 225;  // height 481
  decodeRejects(invalid.data(), invalid.size());
  // A codec failure after inspection leaves no stale geometry or pixels.
  gFakeDecodeFails = true;
  hwjpeg::Image image;
  const char* error = nullptr;
  const int decodes = gFakeSoftwareDecodes.load();
  eiCheck(!decodeEdgeImpulseJpeg(small.data(), small.size(), image, &error), "codec failure fails");
  eiCheck(error != nullptr && !image.pixels && image.width == 0, "codec failure leaves the image empty");
  eiCheck(gFakeSoftwareDecodes.load() == decodes + 1, "codec failure is the codec's");
  std::puts("EDGE_IMPULSE_DECODE PASS");
}

#else  // whole module

// INSERT_EDGE_IMPULSE_MODULE

String executeCommandThroughRegistry(const String& line) {
  const std::string text = line.c_str();
  const size_t space = text.find(' ');
  const std::string name = text.substr(0, space);
  const std::string args = space == std::string::npos ? std::string() : text.substr(space + 1);
  for (size_t i = 0; i < edgeImpulseCommandsCount; ++i) {
    if (name == edgeImpulseCommands[i].name) return String(edgeImpulseCommands[i].handler(String(args)));
  }
  return String("Error: unknown command");
}

#ifdef EI_LINK_CHECK
int main() {
  for (size_t i = 0; i < edgeImpulseCommandsCount; ++i) {
    eiCheck(edgeImpulseCommands[i].handler != nullptr, "every command has its handler");
  }
  eiCheck(std::strstr(cmd_ei_status(String("")), "Edge Impulse Status") != nullptr,
          "eistatus runs without the web server");
  std::puts("EDGE_IMPULSE_LINK PASS");
}
#else

static void setModel(TfLiteType inputType, std::vector<int> inputDims,
                     TfLiteType outputType, std::vector<int> outputDims) {
  gFakeModel = FakeModelSpec{};
  gFakeModel.input.type = inputType;
  gFakeModel.input.dims = std::move(inputDims);
  gFakeModel.output.type = outputType;
  gFakeModel.output.dims = std::move(outputDims);
  gFakeModel.outputValues.assign(fakeElements(gFakeModel.output.dims), 0.0f);
}

// FOMO output cell (gridY, gridX) scores `confidence` for class `cls`.
static void setFomoCell(int gridY, int gridX, int cls, float confidence) {
  const auto& d = gFakeModel.output.dims;
  gFakeModel.outputValues[static_cast<size_t>((gridY * d[2] + gridX) * d[3] + cls)] = confidence;
}

static void loadFakeModel(const std::string& path, const std::vector<std::string>& labels) {
  installModel(path, labels);
  initEdgeImpulse();
  eiCheck(loadModelFromFile(path.c_str()), "fake model loads");
}

static std::vector<uint8_t> currentInputBytes() {
  eiCheck(gInterpreter != nullptr, "interpreter present");
  return gInterpreter->inputBytes();
}

static std::string label(const EIResults& r, int i) { return std::string(r.detections[i].label); }

static void expectBox(const EIResults& r, int i, int x, int y, int w, int h, const char* what) {
  eiCheck(r.detections[i].x == x && r.detections[i].y == y &&
          r.detections[i].width == w && r.detections[i].height == h, what);
}

// A 96x96 grayscale FOMO model: the fill writes 96*96 luma bytes, exactly the
// tensor, and boxes use the model's own geometry.
static void scenarioGray96() {
  setModel(kTfLiteUInt8, {1, 96, 96, 1}, kTfLiteFloat32, {1, 12, 12, 2});
  setFomoCell(3, 4, 1, 0.9f);
  loadFakeModel("/EI Models/gray/gray.tflite", {"background", "object"});
  gFakeCameraJpeg = readFixture(gFixtures + "/solid_640x480.jpg");
  const EIResults r = runEdgeImpulseInference();
  eiCheck(!gFakeGuardBroken, "input fill stays inside the 96x96x1 tensor");
  eiCheck(r.success, "grayscale detect succeeds");
  const auto input = currentInputBytes();
  eiCheck(input.size() == 96u * 96u, "tensor is 96x96x1");
  eiCheck(std::all_of(input.begin(), input.end(), [](uint8_t v) { return v == 124; }),
          "every input byte is the frame's luma");
  eiCheck(r.detectionCount == 1 && label(r, 0) == "object", "one FOMO detection");
  expectBox(r, 0, 32, 24, 8, 8, "FOMO box in model pixels");
  unloadModel();
}

// The old eiinputsize setting no longer reaches the pipeline: a changed value
// after load neither overruns the resize buffer nor the input tensor.
static void scenarioChangedSetting() {
  setModel(kTfLiteUInt8, {1, 96, 96, 3}, kTfLiteFloat32, {1, 4});
  gFakeModel.outputValues = {0.1f, 0.9f, 0.2f, 0.3f};
  loadFakeModel("/EI Models/rgb/rgb.tflite", {"a", "b", "c", "d"});
  gSettings.edgeImpulseInputSize = 320;  // what `eiinputsize 320` used to set
  gFakeCameraJpeg = readFixture(gFixtures + "/solid_640x480.jpg");
  const EIResults r = runEdgeImpulseInference();
  eiCheck(!gFakeGuardBroken, "input fill stays inside the 96x96x3 tensor");
  eiCheck(r.success && r.detectionCount == 1 && label(r, 0) == "b", "classification still works");
  expectBox(r, 0, 0, 0, 96, 96, "classification box is the model input");
  const auto input = currentInputBytes();
  for (size_t i = 0; i < input.size(); ++i) {
    if (input[i] != gFakeDecodeRgb[i % 3]) eiCheck(false, "RGB input copied unchanged");
  }
  String json;
  buildDetectionJson(r, json);
  JsonDocument doc;
  eiCheck(!deserializeJson(doc, json.c_str()), "detection JSON parses");
  eiCheck(doc["modelInputSize"].as<int>() == 96, "reported input size is the model's");
  unloadModel();
}

// A non-square model (64 wide, 48 high): exact fill and per-axis FOMO cells.
static void scenarioNonSquare() {
  setModel(kTfLiteUInt8, {1, 48, 64, 3}, kTfLiteFloat32, {1, 6, 8, 3});
  setFomoCell(2, 5, 2, 0.95f);
  loadFakeModel("/EI Models/wide/wide.tflite", {"bg", "one", "two"});
  gFakeCameraJpeg = readFixture(gFixtures + "/rgb420_320x240.jpg");
  const EIResults r = runEdgeImpulseInference();
  eiCheck(!gFakeGuardBroken, "input fill stays inside the 64x48x3 tensor");
  eiCheck(r.success && r.detectionCount == 1 && label(r, 0) == "two", "non-square detect succeeds");
  expectBox(r, 0, 40, 16, 8, 8, "cell size per axis");
  eiCheck(currentInputBytes().size() == 64u * 48u * 3u, "tensor is 64x48x3");
  unloadModel();
}

// Float32 RGB and int8 grayscale fills use their own element conversions.
static void scenarioFillTypes() {
  setModel(kTfLiteFloat32, {1, 8, 8, 3}, kTfLiteFloat32, {1, 2});
  loadFakeModel("/EI Models/f32/f32.tflite", {"x", "y"});
  gFakeCameraJpeg = readFixture(gFixtures + "/rgb420_24x24.jpg");
  EIResults r = runEdgeImpulseInference();
  eiCheck(r.success && !gFakeGuardBroken, "float32 detect stays inside the tensor");
  auto bytes = currentInputBytes();
  eiCheck(bytes.size() == 8u * 8u * 3u * 4u, "float tensor size");
  const float* f = reinterpret_cast<const float*>(bytes.data());
  for (size_t i = 0; i < 8u * 8u * 3u; ++i) {
    if (std::fabs(f[i] - gFakeDecodeRgb[i % 3] / 255.0f) > 1e-6f) eiCheck(false, "float input is value/255");
  }
  unloadModel();

  setModel(kTfLiteInt8, {1, 8, 8, 1}, kTfLiteInt8, {1, 2});
  gFakeModel.output.scale = 1.0f / 256.0f;
  gFakeModel.output.zeroPoint = -128;
  loadFakeModel("/EI Models/i8/i8.tflite", {"x", "y"});
  r = runEdgeImpulseInference();
  eiCheck(r.success && !gFakeGuardBroken, "int8 grayscale detect stays inside the tensor");
  bytes = currentInputBytes();
  eiCheck(bytes.size() == 64u, "int8 tensor is 8x8x1");
  for (uint8_t b : bytes) {
    if (static_cast<int8_t>(b) != 124 - 128) eiCheck(false, "int8 input is luma-128");
  }
  unloadModel();
}

// Twelve classes: every class has its own label slot and every detection its
// own label text, including classes the labels file does not name.
static void scenarioTwelveLabels() {
  std::vector<std::string> names;
  for (int i = 0; i < 12; ++i) names.push_back("label" + std::to_string(i));
  gFakeCameraJpeg = readFixture(gFixtures + "/rgb420_24x24.jpg");

  setModel(kTfLiteUInt8, {1, 16, 16, 3}, kTfLiteFloat32, {1, 12});
  gFakeModel.outputValues[10] = 0.9f;
  gFakeModel.outputValues[11] = 0.8f;
  loadFakeModel("/EI Models/c12/c12.tflite", names);
  EIResults r = runEdgeImpulseInference();
  eiCheck(r.success && r.detectionCount == 2, "two classes above threshold");
  eiCheck(label(r, 0) == "label10", "class 10 keeps its name");
  eiCheck(label(r, 1) == "label11", "class 11 keeps its name");

  // FOMO, 12 classes: tracked objects copy the right labels too. Cells are
  // 24 px apart (beyond the tracker's 20 px match radius).
  cmd_ei_track_clear(String(""));
  setModel(kTfLiteUInt8, {1, 96, 96, 3}, kTfLiteFloat32, {1, 4, 4, 12});
  setFomoCell(0, 0, 10, 0.9f);
  setFomoCell(3, 3, 11, 0.8f);
  loadFakeModel("/EI Models/f12/f12.tflite", names);
  r = runEdgeImpulseInference();
  eiCheck(r.success && r.detectionCount == 2, "two FOMO cells");
  eiCheck(label(r, 0) == "label10" && label(r, 1) == "label11", "FOMO detections keep distinct names");
  const std::string tracked = cmd_ei_track_status(String(""));
  eiCheck(tracked.find("label10") != std::string::npos && tracked.find("label11") != std::string::npos,
          "tracked objects carry both labels");

  // A short labels file: unnamed classes get distinct generic names.
  setModel(kTfLiteUInt8, {1, 16, 16, 3}, kTfLiteFloat32, {1, 12});
  gFakeModel.outputValues[10] = 0.9f;
  gFakeModel.outputValues[11] = 0.8f;
  loadFakeModel("/EI Models/short/short.tflite", {"a", "b", "c"});
  r = runEdgeImpulseInference();
  eiCheck(r.success && r.detectionCount == 2, "two unnamed classes above threshold");
  eiCheck(label(r, 0) == "class_10" && label(r, 1) == "class_11", "generic names stay distinct");
  const EIResults kept = r;
  r = runEdgeImpulseInference();
  eiCheck(label(kept, 0) == "class_10", "a result keeps its labels after the next inference");

  // An over-long label is truncated inside the detection, not overrun.
  setModel(kTfLiteUInt8, {1, 16, 16, 3}, kTfLiteFloat32, {1, 2});
  gFakeModel.outputValues[1] = 0.9f;
  loadFakeModel("/EI Models/long/long.tflite", {"bg", std::string(40, 'L')});
  r = runEdgeImpulseInference();
  eiCheck(r.success && r.detectionCount == 1 && label(r, 0) == std::string(31, 'L'), "long label truncated");
  unloadModel();
}

static void expectSameResults(const EIResults& a, const EIResults& b) {
  eiCheck(a.success == b.success && a.detectionCount == b.detectionCount, "same outcome and count");
  for (int i = 0; i < a.detectionCount; ++i) {
    eiCheck(label(a, i) == label(b, i), "same label");
    eiCheck(a.detections[i].confidence == b.detections[i].confidence, "same confidence");
    expectBox(b, i, a.detections[i].x, a.detections[i].y, a.detections[i].width, a.detections[i].height,
              "same box");
  }
}

// Camera and stored-file inference share one pipeline: the same frame gives
// the same input tensor and results, and both apply the same checks. Only
// the camera updates tracked objects.
static void scenarioParity() {
  setModel(kTfLiteUInt8, {1, 96, 96, 3}, kTfLiteUInt8, {1, 4, 4, 3});
  setFomoCell(1, 2, 1, 0.9f);
  setFomoCell(3, 0, 2, 0.7f);
  loadFakeModel("/EI Models/par/par.tflite", {"bg", "cup", "pen"});
  gFakeCameraJpeg = readFixture(gFixtures + "/rgb420_320x240.jpg");
  fakePutFile("/images/frame.jpg", std::string(gFakeCameraJpeg.begin(), gFakeCameraJpeg.end()));

  const EIResults camera = runEdgeImpulseInference();
  const int trackedAfterCamera = gTrackedObjectCount;
  const EIResults file = runInferenceFromFile("/images/frame.jpg", VFS::systemAuth("test.caller"));
  eiCheck(camera.success && camera.detectionCount == 2, "camera detect");
  expectSameResults(camera, file);
  eiCheck(gFakeInvokeInputHashes.size() == 2 && gFakeInvokeInputHashes[0] == gFakeInvokeInputHashes[1],
          "both paths fill the same input tensor");
  eiCheck(trackedAfterCamera == 2 && gTrackedObjectCount == 2, "only the camera path tracks objects");

  gSettings.eiEnabled = false;
  const EIResults cameraOff = runEdgeImpulseInference();
  const EIResults fileOff = runInferenceFromFile("/images/frame.jpg", VFS::systemAuth("test.caller"));
  eiCheck(!cameraOff.success && !fileOff.success, "disabled refuses both paths");
  eiCheck(cameraOff.errorMessage && fileOff.errorMessage &&
          std::strcmp(cameraOff.errorMessage, fileOff.errorMessage) == 0, "same disabled error");
  gSettings.eiEnabled = true;

  unloadModel();
  const EIResults cameraNone = runEdgeImpulseInference();
  const EIResults fileNone = runInferenceFromFile("/images/frame.jpg", VFS::systemAuth("test.caller"));
  eiCheck(!cameraNone.success && !fileNone.success, "no model refuses both paths");
  eiCheck(cameraNone.errorMessage && fileNone.errorMessage &&
          std::strcmp(cameraNone.errorMessage, fileNone.errorMessage) == 0, "same no-model error");
}

// The web detect reply is built by the shared JSON builders: model geometry,
// detections and the full tracked-object fields.
static void scenarioWebJson() {
  setModel(kTfLiteUInt8, {1, 32, 32, 3}, kTfLiteFloat32, {1, 4, 4, 3});
  setFomoCell(1, 2, 1, 0.9f);
  loadFakeModel("/EI Models/web/web.tflite", {"bg", "cup", "pen"});
  gFakeCameraJpeg = readFixture(gFixtures + "/rgb420_320x240.jpg");
  httpd_req_t req;
  eiCheck(handleEdgeImpulseDetect(&req) == ESP_OK, "handler answers");
  JsonDocument doc;
  eiCheck(!deserializeJson(doc, req.body.c_str()), "reply parses");
  eiCheck(doc["success"].as<bool>(), "reply reports success");
  eiCheck(doc["modelInputSize"].as<int>() == 32, "reply carries modelInputSize");
  eiCheck(doc["detections"][0]["label"].as<std::string>() == "cup", "reply carries the label");
  eiCheck(!doc["trackedObjects"][0]["lastSeenMs"].isNull(), "tracked objects carry lastSeenMs");
  eiCheck(!doc["trackedObjects"][0]["stateChangeMs"].isNull(), "tracked objects carry stateChangeMs");
  unloadModel();
}

int main(int argc, char** argv) {
  eiCheck(argc == 3, "usage: harness <fixtures> <scenario>");
  gFixtures = argv[1];
  const std::string scenario = argv[2];
  const std::map<std::string, void (*)()> scenarios = {
    {"gray96", scenarioGray96},
    {"changed_setting", scenarioChangedSetting},
    {"non_square", scenarioNonSquare},
    {"fill_types", scenarioFillTypes},
    {"twelve_labels", scenarioTwelveLabels},
    {"parity", scenarioParity},
    {"web_json", scenarioWebJson},
  };
  auto it = scenarios.find(scenario);
  eiCheck(it != scenarios.end(), "known scenario");
  it->second();
  std::printf("EDGE_IMPULSE_JPEG %s PASS\n", scenario.c_str());
}
#endif  // EI_LINK_CHECK
#endif  // EI_DECODE_UNIT
