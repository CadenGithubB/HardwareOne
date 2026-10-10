// The owner and real registry precede this file. Expectations describe the
// public menu contract; no private row catalog is duplicated here.
static SetupWizardPage expectedGroup(const FeatureEntry& feature) {
  if (!std::strcmp(feature.id, "input")) return WIZARD_PAGE_DISPLAY;
  switch (feature.category) {
    case FEATURE_CAT_NETWORK: return WIZARD_PAGE_CONNECTIONS;
    case FEATURE_CAT_CORE:
    case FEATURE_CAT_DISPLAY: return WIZARD_PAGE_DISPLAY;
    case FEATURE_CAT_SENSOR: return WIZARD_PAGE_SENSORS;
    case FEATURE_CAT_SYSTEM: return WIZARD_PAGE_SYSTEM;
  }
  assert(false); return WIZARD_PAGE_COUNT;
}
static const WizardMenuItem* findRow(const char* id, SetupWizardPage& group, int& index) {
  for (int page = 0; page < WIZARD_PAGE_COUNT; ++page) {
    group = static_cast<SetupWizardPage>(page);
    for (size_t i = 0; i < getWizardMenuItemCount(group); ++i) {
      const auto* row = getWizardMenuItem(group, static_cast<int>(i));
      if (row && !std::strcmp(row->id, id)) { index = static_cast<int>(i); return row; }
    }
  }
  return nullptr;
}
static void verifyRowsAndActions() {
  reset(); initSetupWizard();
  std::vector<std::string> ids;
  for (int page = 0; page < WIZARD_PAGE_COUNT; ++page) {
    const auto group = static_cast<SetupWizardPage>(page);
    std::string previousFeatureLabel;
    const auto count = getWizardMenuItemCount(group);
    check(getWizardMenuItem(group, -1) == nullptr);
    check(getWizardMenuItem(group, static_cast<int>(count)) == nullptr);
    if (count) check(wizardIsGroupPage(group));
    for (size_t index = 0; index < count; ++index) {
      const auto* row = getWizardMenuItem(group, static_cast<int>(index));
      check(row && row->id && row->label);
      check(std::find(ids.begin(), ids.end(), row->id) == ids.end());
      ids.emplace_back(row->id);
      char label[80], value[80];
      check(wizardFormatMenuItem(group, static_cast<int>(index), label, sizeof(label), value, sizeof(value)));
      check(std::string(label) == row->label);
      const auto* feature = getFeatureById(row->id);
      if (!feature) continue;
      check(isFeatureCompiled(feature));
      check(feature->enabledSetting && (feature->flags & FEATURE_FLAG_RUNTIME_TOGGLE));
      check(group == expectedGroup(*feature));
      String ordered = row->label; ordered.toLowerCase();
      check(previousFeatureLabel.empty() || previousFeatureLabel <= ordered);
      previousFeatureLabel = ordered;
      check(row->setting && row->setting != feature->enabledSetting);
      const bool startup = (feature->category == FEATURE_CAT_SENSOR && std::strcmp(feature->id, "edgeimpulse")) ||
          !std::strcmp(feature->id, "http") || !std::strcmp(feature->id, "bluetooth") ||
          !std::strcmp(feature->id, "led") || !std::strcmp(feature->id, "llm");
      check(row->type == (startup ? WIZARD_ITEM_STARTUP : WIZARD_ITEM_TOGGLE));
      const std::string before = value;
      if (startup) check(before.find("Boot") != std::string::npos);
      else check(before.find("Boot") == std::string::npos);
      const bool live = *feature->enabledSetting, prior = *row->setting;
      setWizardCurrentPage(group); setWizardCurrentSelection(static_cast<int>(index));
      check(wizardActivateCurrentItem());
      check(*row->setting != prior && *feature->enabledSetting == live);
      check(wizardFeatureEnabled(feature->id) == *row->setting);
      check(wizardFormatMenuItem(group, static_cast<int>(index), label, sizeof(label), value, sizeof(value)));
      check(before != value);
      check(wizardActivateCurrentItem() && *row->setting == prior);
    }
  }
  size_t toggles = 0;
  for (size_t index = 0; index < getFeatureCount(); ++index) {
    const auto* feature = getFeatureByIndex(index);
    const bool eligible = isFeatureCompiled(feature) && feature->enabledSetting &&
                          (feature->flags & FEATURE_FLAG_RUNTIME_TOGGLE);
    const auto seen = std::count(ids.begin(), ids.end(), feature->id);
    check(seen == (eligible ? 1 : 0));
    if (eligible) ++toggles;
  }
  check(changed.empty() && operations.empty());
  std::printf("setup menu layout (%s): %zu compiled feature rows\n", TEST_PROFILE, toggles);
}
static void verifyExtraAdjacency() {
  for (const auto& pair : {std::pair{"wifi", "wifi_autoconnect"}, {"espnow", "espnow_mesh"},
                           {"mqtt", "mqtt_autostart"}, {"led", "led_effect"}}) {
    SetupWizardPage parentGroup{}, extraGroup{}; int parentIndex = -1, extraIndex = -1;
    const auto* parent = findRow(pair.first, parentGroup, parentIndex);
    const auto* extra = findRow(pair.second, extraGroup, extraIndex);
    check((parent != nullptr) == (extra != nullptr));
    if (!parent) continue;
    check(parentGroup == extraGroup && extraIndex == parentIndex + 1);
    if (!std::strcmp(pair.second, "led_effect")) check(extra->type == WIZARD_ITEM_LED_EFFECT);
    else {
      check(extra->setting != nullptr);
      const bool value = *extra->setting;
      setWizardCurrentPage(extraGroup); setWizardCurrentSelection(extraIndex);
      check(wizardActivateCurrentItem() && *extra->setting != value);
      check(wizardActivateCurrentItem() && *extra->setting == value);
    }
  }
}
static void verifyNavigation() {
  reset(); initSetupWizard();
  // Enable available network features to expose every optional middle page.
  for (const char* id : {"wifi", "http", "bluetooth", "espnow", "mqtt"})
    if (isFeatureCompiled(getFeatureById(id))) wizardSetFeatureEnabled(id, true);
  if (isFeatureCompiled(getFeatureById("mqtt"))) wizardSetMQTTAutoStart(true);
  std::vector<SetupWizardPage> visible;
  for (int page = 0; page < WIZARD_PAGE_COUNT; ++page)
    if (wizardIsPageVisible(static_cast<SetupWizardPage>(page))) visible.push_back(static_cast<SetupWizardPage>(page));
  check(!visible.empty());
  check(visible.back() == WIZARD_PAGE_SYSTEM);
  setWizardCurrentPage(visible.front());
  check(wizardIsFirstPage());
  SetupWizardResult result{};
  for (size_t index = 1; index < visible.size(); ++index) {
    check(wizardNextPage(result));
    check(!result.completed && getWizardCurrentPage() == visible[index]);
    check(!wizardIsFirstPage());
  }
  for (size_t index = visible.size() - 1; index > 0; --index) {
    check(wizardPrevPage());
    check(getWizardCurrentPage() == visible[index - 1]);
  }
  check(wizardIsFirstPage());
  check(!wizardPrevPage());
#if ENABLE_WIFI
  setWizardCurrentPage(WIZARD_PAGE_WIFI);
  check(wizardNextPage(result) && !result.completed);
  check(getWizardCurrentPage() > WIZARD_PAGE_WIFI && getWizardCurrentPage() < WIZARD_PAGE_COUNT);
  check(wizardPrevPage() && getWizardCurrentPage() == WIZARD_PAGE_WIFI);
#endif
#if TEST_MINIMAL
  check(visible.size() == 1 && visible.front() == WIZARD_PAGE_SYSTEM);
  check(!wizardIsPageVisible(WIZARD_PAGE_CONNECTIONS));
  check(!wizardIsPageVisible(WIZARD_PAGE_DISPLAY));
  check(!wizardIsPageVisible(WIZARD_PAGE_SENSORS));
  check(getWizardMenuItemCount(WIZARD_PAGE_SYSTEM) == 3); // NTP has no meaning without Wi-Fi.
#endif
  setWizardCurrentPage(WIZARD_PAGE_SYSTEM);
  check(!wizardNextPage(result) && result.completed);
  check(changed.empty() && operations.empty());
  discardSetupWizard();
}
static void verifyPresentation() {
  reset(); initSetupWizard();
  for (const auto group : {WIZARD_PAGE_CONNECTIONS, WIZARD_PAGE_DISPLAY, WIZARD_PAGE_SENSORS, WIZARD_PAGE_SYSTEM}) {
    if (!wizardIsPageVisible(group)) continue;
    output.clear(); setWizardCurrentPage(group); printSerialPageStatus();
    check(output.size() >= getWizardMenuItemCount(group) + 6);
    check(output[2].find(getWizardPageTitle(group)) != std::string::npos);
    for (size_t index = 0; index < getWizardMenuItemCount(group); ++index) {
      char label[48], value[80];
      check(wizardFormatMenuItem(group, static_cast<int>(index), label, sizeof(label), value, sizeof(value)));
      check(output[index + 3].find(std::string(label) + ": " + value) != std::string::npos);
    }
    const auto hint = std::find_if(output.begin(), output.end(), [](const auto& text) { return text.find("Serial:") == 0; });
    check(hint != output.end());
    check(hint->find(group == WIZARD_PAGE_SYSTEM ? "'n' finish" : "'n' next") != std::string::npos);
    check(hint->find(wizardIsFirstPage() ? "'b' cancel" : "'b' back") != std::string::npos);
  }
  SetupWizardPage group{}; int index = -1;
  check(findRow("timezone", group, index));
  char label[48], value[80];
  for (size_t i = 0; i < getTimezoneCount(); ++i) {
    setWizardTimezoneSelection(static_cast<int>(i));
    check(wizardFormatMenuItem(group, index, label, sizeof(label), value, sizeof(value)));
    check(std::strlen(value) <= 12); // Value still distinguishes offset on the OLED.
    check(std::strchr(value, ':') != nullptr);
  }
  reset(); gSettings.tzOffsetMinutes = -2147483647 - 1; initSetupWizard();
  check(findRow("timezone", group, index));
  check(wizardFormatMenuItem(group, index, label, sizeof(label), value, sizeof(value)));
  check(std::string(value).find("UTC-") == 0);
}
static CLIModeInputResult cli(const String& text) {
  char response[160]{}; return wizardMode_onInput(text, nullptr, response, sizeof(response));
}
static void verifyCliActions() {
  reset(); initSetupWizard(); sWizard = {}; sWizard.subMode = WizardSubMode::PAGE_GROUP;
  for (const auto group : {WIZARD_PAGE_CONNECTIONS, WIZARD_PAGE_DISPLAY, WIZARD_PAGE_SENSORS, WIZARD_PAGE_SYSTEM}) {
    for (size_t i = 0; i < getWizardMenuItemCount(group); ++i) {
      const auto* row = getWizardMenuItem(group, static_cast<int>(i));
      if (!row->setting) continue;
      setWizardCurrentPage(group); sWizard.subMode = WizardSubMode::PAGE_GROUP;
      const bool before = *row->setting;
      check(cli(String(static_cast<long>(i + 1))) == CLI_MODE_HANDLED);
      check(*row->setting != before && getWizardCurrentSelection() == static_cast<int>(i));
      check(cli(String(static_cast<long>(i + 1))) == CLI_MODE_HANDLED && *row->setting == before);
    }
  }
  setWizardCurrentPage(WIZARD_PAGE_SYSTEM); setWizardCurrentSelection(0);
  for (const char* bad : {"0", "-1", "-2147483648", "-9223372036854775808", "999999999999999999999999",
                          "1junk", "1.0", "+1", "1 2"}) {
    const auto values = getWizardDraftValues();
    std::vector<bool> beforeFlags;
    for (size_t i = 0; i < getFeatureCount(); ++i)
      beforeFlags.push_back(wizardFeatureEnabled(getFeatureByIndex(i)->id));
    check(cli(bad) == CLI_MODE_HANDLED && !sWizard.result.completed);
    check(sWizard.subMode == WizardSubMode::PAGE_GROUP);
    check(getWizardCurrentSelection() == 0 && getWizardDraftValues().timezoneOffset == values.timezoneOffset);
    for (size_t i = 0; i < getFeatureCount(); ++i)
      check(wizardFeatureEnabled(getFeatureByIndex(i)->id) == beforeFlags[i]);
  }
  SetupWizardPage group{}; int index = -1;
  check(findRow("device_name", group, index));
  check(cli(String(static_cast<long>(index + 1))) == CLI_MODE_HANDLED);
  check(sWizard.subMode == WizardSubMode::PAGE_DEVICE_NAME);
  check(cli("renamed") == CLI_MODE_HANDLED && sWizard.subMode == WizardSubMode::PAGE_GROUP);
  check(std::string(getWizardDeviceName()) == "renamed");
  check(cli("n") == CLI_MODE_HANDLED_AND_EXIT && sWizard.result.completed);
  check(getWizardDraftValues().deviceName == "renamed" && gSettings.espnowDeviceName == "old-device");
  reset(); failReserve = true; initSetupWizard(); failReserve = false;
  setWizardCurrentPage(WIZARD_PAGE_SYSTEM); sWizard = {}; sWizard.subMode = WizardSubMode::PAGE_GROUP;
  check(cli("n") == CLI_MODE_HANDLED_AND_EXIT && !sWizard.result.completed);
  sWizard.subMode = WizardSubMode::DONE;
  check(cli("anything") == CLI_MODE_HANDLED_AND_EXIT && !sWizard.result.completed);
  reset(); initSetupWizard(); sWizard = {}; sWizard.subMode = WizardSubMode::PAGE_GROUP;
  check(wizardIsFirstPage());
  check(cli("b") == CLI_MODE_HANDLED_AND_EXIT && cancelRequested && !sWizard.result.completed);
  check(changed.empty() && operations.empty());
}
int main() {
  verifyRowsAndActions(); verifyExtraAdjacency(); verifyNavigation(); verifyPresentation(); verifyCliActions();
  std::printf("setup menu layout (%s): %u checks passed\n", TEST_PROFILE, checks);
}
