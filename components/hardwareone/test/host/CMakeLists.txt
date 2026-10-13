cmake_minimum_required(VERSION 3.16)
project(hardwareone_host_tests C CXX)

find_package(Threads REQUIRED)

add_executable(ht16k33_geometry_tests test_ht16k33_geometry.cpp)
set_target_properties(ht16k33_geometry_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_compile_options(ht16k33_geometry_tests PRIVATE -Wall -Wextra -Werror -pedantic)

# Compiles the REAL header from the firmware tree, not a copy of it — the same
# rule updater/test/host/CMakeLists.txt states. System_LLMUtf8.h is kept
# dependency-free so this is possible; if it ever gains an Arduino or ESP-IDF
# include, this target stops building and the coverage is gone.
add_executable(llm_utf8_tests test_llm_utf8.c)
set_target_properties(llm_utf8_tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(llm_utf8_tests PRIVATE -Wall -Wextra -Werror -pedantic)

# Exact-session CM5 capability and post-IDLE request-publication gates used by
# System_Dictation.cpp. This is the production dependency-free header, not a
# test-local model of it.
add_executable(dictation_policy_tests test_dictation_policy.cpp)
set_target_properties(dictation_policy_tests PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(dictation_policy_tests PRIVATE
  -Wall -Wextra -Werror -pedantic)

# Long-press (hold) press/release pairing and route policy used by
# G2_Glasses.cpp. Production dependency-free header; the firmware owns the
# portMUX and the clock.
add_executable(g2_hold_gesture_tests test_g2_hold_gesture.cpp)
set_target_properties(g2_hold_gesture_tests PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(g2_hold_gesture_tests PRIVATE
  -Wall -Wextra -Werror -pedantic)

# QuickNote lifecycle FSM, title/junk heuristics, v1 header, file names and
# private notes path. Production dependency-free header (with the transcript
# path policy it builds on); QuickNote_Store.cpp does the I/O.
add_executable(quicknote_policy_tests test_quicknote_policy.cpp)
set_target_properties(quicknote_policy_tests PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(quicknote_policy_tests PRIVATE
  -Wall -Wextra -Werror -pedantic)

# The packed atomic command-admission/lifecycle gate is the production header
# used by System_MQTT.cpp. Exercise its deterministic transitions and the
# command-vs-stop race without an Arduino or ESP-IDF shim.
add_executable(mqtt_lifecycle_gate_tests test_mqtt_lifecycle_gate.cpp)
set_target_properties(mqtt_lifecycle_gate_tests PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_link_libraries(mqtt_lifecycle_gate_tests PRIVATE Threads::Threads)
target_compile_options(mqtt_lifecycle_gate_tests PRIVATE
  -Wall -Wextra -Werror -pedantic)

# Compile the real allocator implementation against a deterministic mock of
# the ESP-IDF heap-capability backend. This covers routing order and the C
# malloc-family edge contracts without maintaining a test-local allocator copy.
add_executable(memutil_tests
  test_memutil.cpp
  ../../System_MemUtil.cpp)
set_target_properties(memutil_tests PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(memutil_tests PRIVATE
  memutil_stubs
  ../..)
target_compile_definitions(memutil_tests PRIVATE BOARD_HAS_PSRAM=1)
target_compile_options(memutil_tests PRIVATE
  -Wall -Wextra -Werror -pedantic)

# The same production allocator must also work when the board is compiled
# without any PSRAM capability macro, not just when an external heap is full.
add_executable(memutil_no_psram_tests
  test_memutil.cpp
  ../../System_MemUtil.cpp)
set_target_properties(memutil_no_psram_tests PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(memutil_no_psram_tests PRIVATE
  memutil_stubs
  ../..)
target_compile_options(memutil_no_psram_tests PRIVATE
  -Wall -Wextra -Werror -pedantic)

# The production tracker wraps this dependency-light registry with FreeRTOS
# synchronization. Exercise the actual hash/aggregate/top-K state machine.
add_executable(memtracker_core_tests test_memtracker_core.cpp)
set_target_properties(memtracker_core_tests PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(memtracker_core_tests PRIVATE ../..)
target_compile_options(memtracker_core_tests PRIVATE
  -Wall -Wextra -Werror -pedantic)

# Projection and pan math shared by MapCore::renderMap and the G2 map page.
# The production header is dependency-light so geometry can be verified here
# without Arduino, ESP-IDF, filesystem, or BLE mocks.
add_executable(map_viewport_core_tests test_map_viewport_core.cpp)
set_target_properties(map_viewport_core_tests PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(map_viewport_core_tests PRIVATE
  -Wall -Wextra -Werror -pedantic)

# Live-series timestamp identity and open-right frame policy used by the G2
# Health bitmap renderer. The 70 -> 80 DAILY fixture is the former right-edge
# vertical-spike regression.
add_executable(g2_health_graph_core_tests test_g2_health_graph_core.cpp)
set_target_properties(g2_health_graph_core_tests PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(g2_health_graph_core_tests PRIVATE
  -Wall -Wextra -Werror -pedantic)

# Bounds-safe bit operations shared by the notification policy and OLED. The
# real production header derives capacity from each owning array reference.
add_executable(event_kind_mask_tests test_event_kind_mask.cpp)
set_target_properties(event_kind_mask_tests PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(event_kind_mask_tests PRIVATE
  -Wall -Wextra -Werror -pedantic)

# Shared executor/transport input and output budgets. Boundary behavior is
# tested without Arduino or ESP-IDF dependencies.
add_executable(command_limit_tests test_command_limits.cpp)
set_target_properties(command_limit_tests PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(command_limit_tests PRIVATE
  -Wall -Wextra -Werror -pedantic)

# Allocation-free longest-prefix matching shared by every command transport.
# The test compiles the real dependency-free core, differentially fuzzes it
# against the legacy grammar, and counts global new/new[] calls.
add_executable(command_lookup_core_tests test_command_lookup_core.cpp)
set_target_properties(command_lookup_core_tests PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(command_lookup_core_tests PRIVATE
  -Wall -Wextra -Werror -pedantic)

# Allocation-free help page-token parsing and two-budget group packing. The
# firmware adapter will provide formatted line costs and consume placements;
# this target pins the transport-independent planning core.
add_executable(help_pager_core_tests test_help_pager_core.cpp)
set_target_properties(help_pager_core_tests PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(help_pager_core_tests PRIVATE
  -Wall -Wextra -Werror -pedantic)

# Owner-scoped, allocation-free settings write batching. The firmware adapter
# supplies synchronization and persistence; this exact production core decides
# deferral, nesting, independent file dirtiness, and failed-flush restoration.
add_executable(settings_write_batch_core_tests
  test_settings_write_batch_core.cpp)
set_target_properties(settings_write_batch_core_tests PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(settings_write_batch_core_tests PRIVATE
  -Wall -Wextra -Werror -pedantic)

# Allocation-free grammar and list mutation semantics used by personal
# notification preferences in every UI.
add_executable(notification_kind_list_tests test_notification_kind_list.cpp)
set_target_properties(notification_kind_list_tests PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(notification_kind_list_tests PRIVATE
  -Wall -Wextra -Werror -pedantic)

# Execute the real I2C manager transaction template against deterministic host
# stubs. This pins the distinction between registry identity/health and the
# clock + bus-lock wait policy supplied by each individual call.
add_executable(i2c_transaction_options_tests test_i2c_transaction_options.cpp)
set_target_properties(i2c_transaction_options_tests PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(i2c_transaction_options_tests PRIVATE
  i2c_stubs
  ../..)
target_compile_options(i2c_transaction_options_tests PRIVATE
  -Wall -Wextra -Werror -pedantic
  $<$<CXX_COMPILER_ID:AppleClang,Clang>:-Wno-keyword-macro>)

# Generation/expiry core used by VFS's opt-in presentation capacity snapshots.
# Synchronization stays in System_VFS.cpp; this dependency-free production
# header pins the state transitions that run inside its short critical section.
add_executable(vfs_capacity_cache_tests test_vfs_capacity_cache.cpp)
set_target_properties(vfs_capacity_cache_tests PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(vfs_capacity_cache_tests PRIVATE ../..)
target_compile_options(vfs_capacity_cache_tests PRIVATE
  -Wall -Wextra -Werror -pedantic)

# Compile the real immutable catalog implementation against its public API and
# dependency-light validation/index core. The test also stresses concurrent
# reads and counts global new/new[] calls across repeated provider operations.
add_executable(event_catalog_tests
  test_event_catalog.cpp
  ../../System_EventCatalog.cpp)
set_target_properties(event_catalog_tests PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(event_catalog_tests PRIVATE ../..)
target_link_libraries(event_catalog_tests PRIVATE Threads::Threads)
target_compile_options(event_catalog_tests PRIVATE
  -Wall -Wextra -Werror -pedantic)

# Transport-neutral JSON projection over the real typed provider. The same
# executable also exposes deterministic JSON/typed dumps to the Python parser
# parity test below.
add_executable(event_catalog_json_tests
  test_event_catalog_json.cpp
  ../../System_EventCatalog.cpp
  ../../System_EventCatalogJson.cpp)
set_target_properties(event_catalog_json_tests PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(event_catalog_json_tests PRIVATE ../..)
target_compile_options(event_catalog_json_tests PRIVATE
  -Wall -Wextra -Werror -pedantic)

# Checked, allocation-free human catalog formatter used by `events kinds`,
# exercised against both hostile fixtures and the real typed provider.
add_executable(event_catalog_text_tests
  test_event_catalog_text.cpp
  ../../System_EventCatalog.cpp)
set_target_properties(event_catalog_text_tests PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(event_catalog_text_tests PRIVATE ../..)
target_compile_options(event_catalog_text_tests PRIVATE
  -Wall -Wextra -Werror -pedantic)

# HW1LM1 word n-gram LM view, custom words and CTC prefix beam decoder
# (stt/stt_lm.cpp, host SHA-256/allocation path) against test-written LMs, and
# randomised cases against the independent map-based reference decoder in
# stt_lm_testlib.h.
add_executable(stt_lm_tests test_stt_lm.cpp ../../stt/stt_lm.cpp)
set_target_properties(stt_lm_tests PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(stt_lm_tests PRIVATE ../..)
target_compile_options(stt_lm_tests PRIVATE
  -Wall -Wextra -Werror -pedantic)

# CLI over the same decoder plus the production greedy CTC for fixture parity
# (test_stt_lm_parity.py) and `stt_lm_tool bench`. Optimised and never
# sanitised so its timings mean something.
add_executable(stt_lm_tool stt_lm_tool.cpp ../../stt/stt_lm.cpp ../../stt/quartznet_frontend.cpp)
set_target_properties(stt_lm_tool PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(stt_lm_tool PRIVATE ../..)
target_compile_options(stt_lm_tool PRIVATE
  -O2 -Wall -Wextra -Werror -pedantic)

# Sanitizers are the point of test 1: the walk-back reads backwards through the
# buffer, and an earlier draft ran off the front. Without ASan that is invisible.
option(HW1_SANITIZE "Build the host tests with ASan/UBSan" ON)
if(HW1_SANITIZE)
  target_compile_options(llm_utf8_tests PRIVATE -fsanitize=address,undefined -g)
  target_link_options(llm_utf8_tests PRIVATE -fsanitize=address,undefined)
  target_compile_options(dictation_policy_tests PRIVATE
    -fsanitize=address,undefined -g)
  target_link_options(dictation_policy_tests PRIVATE
    -fsanitize=address,undefined)
  target_compile_options(g2_hold_gesture_tests PRIVATE
    -fsanitize=address,undefined -g)
  target_link_options(g2_hold_gesture_tests PRIVATE
    -fsanitize=address,undefined)
  target_compile_options(quicknote_policy_tests PRIVATE
    -fsanitize=address,undefined -g)
  target_link_options(quicknote_policy_tests PRIVATE
    -fsanitize=address,undefined)
  target_compile_options(mqtt_lifecycle_gate_tests PRIVATE
    -fsanitize=address,undefined -g)
  target_link_options(mqtt_lifecycle_gate_tests PRIVATE
    -fsanitize=address,undefined)
  target_compile_options(memutil_tests PRIVATE
    -fsanitize=address,undefined -g)
  target_link_options(memutil_tests PRIVATE
    -fsanitize=address,undefined)
  target_compile_options(memutil_no_psram_tests PRIVATE
    -fsanitize=address,undefined -g)
  target_link_options(memutil_no_psram_tests PRIVATE
    -fsanitize=address,undefined)
  target_compile_options(memtracker_core_tests PRIVATE
    -fsanitize=address,undefined -g)
  target_link_options(memtracker_core_tests PRIVATE
    -fsanitize=address,undefined)
  target_compile_options(map_viewport_core_tests PRIVATE
    -fsanitize=address,undefined -g)
  target_link_options(map_viewport_core_tests PRIVATE
    -fsanitize=address,undefined)
  target_compile_options(g2_health_graph_core_tests PRIVATE
    -fsanitize=address,undefined -g)
  target_link_options(g2_health_graph_core_tests PRIVATE
    -fsanitize=address,undefined)
  target_compile_options(event_kind_mask_tests PRIVATE
    -fsanitize=address,undefined -g)
  target_link_options(event_kind_mask_tests PRIVATE
    -fsanitize=address,undefined)
  target_compile_options(command_limit_tests PRIVATE
    -fsanitize=address,undefined -g)
  target_link_options(command_limit_tests PRIVATE
    -fsanitize=address,undefined)
  target_compile_options(command_lookup_core_tests PRIVATE
    -fsanitize=address,undefined -g)
  target_link_options(command_lookup_core_tests PRIVATE
    -fsanitize=address,undefined)
  target_compile_options(help_pager_core_tests PRIVATE
    -fsanitize=address,undefined -g)
  target_link_options(help_pager_core_tests PRIVATE
    -fsanitize=address,undefined)
  target_compile_options(settings_write_batch_core_tests PRIVATE
    -fsanitize=address,undefined -g)
  target_link_options(settings_write_batch_core_tests PRIVATE
    -fsanitize=address,undefined)
  target_compile_options(notification_kind_list_tests PRIVATE
    -fsanitize=address,undefined -g)
  target_link_options(notification_kind_list_tests PRIVATE
    -fsanitize=address,undefined)
  target_compile_options(i2c_transaction_options_tests PRIVATE
    -fsanitize=address,undefined -g)
  target_link_options(i2c_transaction_options_tests PRIVATE
    -fsanitize=address,undefined)
  target_compile_options(vfs_capacity_cache_tests PRIVATE
    -fsanitize=address,undefined -g)
  target_link_options(vfs_capacity_cache_tests PRIVATE
    -fsanitize=address,undefined)
  target_compile_options(event_catalog_tests PRIVATE
    -fsanitize=address,undefined -g)
  target_link_options(event_catalog_tests PRIVATE
    -fsanitize=address,undefined)
  target_compile_options(event_catalog_json_tests PRIVATE
    -fsanitize=address,undefined -g)
  target_link_options(event_catalog_json_tests PRIVATE
    -fsanitize=address,undefined)
  target_compile_options(event_catalog_text_tests PRIVATE
    -fsanitize=address,undefined -g)
  target_link_options(event_catalog_text_tests PRIVATE
    -fsanitize=address,undefined)
  target_compile_options(stt_lm_tests PRIVATE
    -fsanitize=address,undefined -g)
  target_link_options(stt_lm_tests PRIVATE
    -fsanitize=address,undefined)
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(G2_CONVERSATE_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND G2_CONVERSATE_TEST_ARGS --sanitize)
endif()
add_test(NAME g2_conversate
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_g2_conversate.py
          ${G2_CONVERSATE_TEST_ARGS})

add_executable(p4eye_display_pixels_tests test_p4eye_display_pixels.cpp)
add_executable(p4eye_display_lifecycle_tests
  test_p4eye_display_lifecycle.cpp ../../HAL_Display_P4Eye.cpp)
set_target_properties(p4eye_display_lifecycle_tests PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(p4eye_display_lifecycle_tests PRIVATE p4eye_display_stubs)
target_compile_definitions(p4eye_display_lifecycle_tests PRIVATE
  SYSTEM_BUILDCONFIG_H DISPLAY_TYPE=4 DISPLAY_TYPE_ST7789_P4_EYE=4
  CONFIG_IDF_TARGET_ESP32P4=1)
target_compile_options(p4eye_display_lifecycle_tests PRIVATE
  -Wall -Wextra -Werror -pedantic)
set_target_properties(p4eye_display_pixels_tests PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(p4eye_display_pixels_tests PRIVATE
  -Wall -Wextra -Werror -pedantic)
if(HW1_SANITIZE)
  target_compile_options(p4eye_display_pixels_tests PRIVATE -fsanitize=address,undefined -g)
  target_link_options(p4eye_display_pixels_tests PRIVATE -fsanitize=address,undefined)
  target_compile_options(p4eye_display_lifecycle_tests PRIVATE -fsanitize=address,undefined -g)
  target_link_options(p4eye_display_lifecycle_tests PRIVATE -fsanitize=address,undefined)
endif()

# Wire-protocol decoders shared by the P4 host and the C6 companion bridge
# (components/esp_now_hosted/include/esp_now_hosted_rpc.h). Checks sizes,
# exact bounds, truncation and trailing bytes, versions, magic and sequences.
# Standalone C; no ESP-IDF.
add_executable(esp_now_hosted_protocol_tests
  ../../../esp_now_hosted/test/test_protocol.c)
set_target_properties(esp_now_hosted_protocol_tests PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_include_directories(esp_now_hosted_protocol_tests PRIVATE
  ../../../esp_now_hosted/include)
target_compile_options(esp_now_hosted_protocol_tests PRIVATE
  -Wall -Wextra -Werror)
if(HW1_SANITIZE)
  target_compile_options(esp_now_hosted_protocol_tests PRIVATE -fsanitize=address,undefined -g)
  target_link_options(esp_now_hosted_protocol_tests PRIVATE -fsanitize=address,undefined)
endif()

# Health, retry and recovery policy for the P4X-EYE's C6 radio companion
# (Radio_CompanionCore.h): boot retries with backoff, the heartbeat watchdog,
# soft-recovery escalation to a reboot, idle hold and the sleep pause.
add_executable(radio_companion_core_tests test_radio_companion_core.cpp)
set_target_properties(radio_companion_core_tests PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(radio_companion_core_tests PRIVATE
  -Wall -Wextra -Werror -pedantic)
if(HW1_SANITIZE)
  target_compile_options(radio_companion_core_tests PRIVATE -fsanitize=address,undefined -g)
  target_link_options(radio_companion_core_tests PRIVATE -fsanitize=address,undefined)
endif()

enable_testing()

add_test(NAME esp_now_hosted_protocol COMMAND esp_now_hosted_protocol_tests)
add_test(NAME radio_companion_core COMMAND radio_companion_core_tests)
# The real HAL_Bluetooth.cpp under native and Hosted (P4 + C6) controller
# substitutes; see test_hal_bluetooth.py for what it does and does not prove.
set(HAL_BLUETOOTH_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND HAL_BLUETOOTH_TEST_ARGS --sanitize)
endif()
add_test(NAME hal_bluetooth
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_hal_bluetooth.py
          ${HAL_BLUETOOTH_TEST_ARGS})

# Exercise real JPEG dispatch/software/P4 code against controlled SDK boundaries.
# Supplying installed component paths additionally checks exact old/new software
# pixels using the actual TJpgDec and esp32-camera converter sources.
set(HW1_JPEG_COMPONENT "" CACHE PATH "Installed espressif__esp_jpeg component for codec parity")
set(HW1_CAMERA_COMPONENT "" CACHE PATH "Installed espressif__esp32-camera component for codec parity")
set(JPEG_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER} --cc ${CMAKE_C_COMPILER})
if(HW1_SANITIZE)
  list(APPEND JPEG_TEST_ARGS --sanitize)
endif()
if(HW1_JPEG_COMPONENT AND HW1_CAMERA_COMPONENT)
  list(APPEND JPEG_TEST_ARGS --jpeg-component ${HW1_JPEG_COMPONENT}
                            --camera-component ${HW1_CAMERA_COMPONENT})
elseif(HW1_JPEG_COMPONENT OR HW1_CAMERA_COMPONENT)
  message(FATAL_ERROR "JPEG corpus parity requires both HW1_JPEG_COMPONENT and HW1_CAMERA_COMPONENT")
endif()
add_test(NAME jpeg_portable
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_jpeg.py ${JPEG_TEST_ARGS})

# Camera family: the shared lifecycle core from System_Camera_DVP.cpp, the DVP
# and P4 backends, the camera/sensor I2C reservation and the whole G2 camera
# settings page, each compiled against host fault boundaries (README "Shared
# camera controls and consumers" and "Camera / sensor I2C ownership").
set(CAMERA_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND CAMERA_TEST_ARGS --sanitize)
endif()
foreach(test_name IN ITEMS camera_core camera_dvp camera_p4 camera_i2c_reservation
                           camera_consumers)
  add_test(
    NAME ${test_name}
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_LIST_DIR}/test_${test_name}.py
            ${CAMERA_TEST_ARGS})
endforeach()

# ESP-NOW peer metadata: the metadata text policy, receive path and CLI setters
# (C++), and the shipping ESP-NOW page renderers on a host JavaScript engine
# (README "ESP-NOW peer metadata text and page rendering").
set(ESPNOW_METADATA_TEXT_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND ESPNOW_METADATA_TEXT_TEST_ARGS --sanitize)
endif()
add_test(
  NAME espnow_metadata_text
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_espnow_metadata_text.py
          ${ESPNOW_METADATA_TEXT_TEST_ARGS})
add_test(
  NAME espnow_page_render
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_espnow_page_render.py)
# Exit 77 = source contracts passed but no JavaScript engine was found (reported as skipped).
set_tests_properties(espnow_page_render PROPERTIES SKIP_RETURN_CODE 77)

# ESP-NOW session handshake freshness (README "ESP-NOW session handshake freshness"):
# production SESSION_OPEN/CONFIRM/REKEY handlers and initiators between two modelled
# devices; re-delivered handshake frames must not re-key or reset a live session.
set(ESPNOW_SESSION_REKEY_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND ESPNOW_SESSION_REKEY_TEST_ARGS --sanitize)
endif()
add_test(
  NAME espnow_session_rekey
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_espnow_session_rekey.py
          ${ESPNOW_SESSION_REKEY_TEST_ARGS})

# ESP-NOW session replay window across a REKEY (README "ESP-NOW session replay
# window"): production wrap/unwrap, rekey-apply, REKEYING/abort and prev-key sweep
# driven by a scripted peer; the previous key is judged by its own replay window.
set(ESPNOW_SESSION_REPLAY_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND ESPNOW_SESSION_REPLAY_TEST_ARGS --sanitize)
endif()
add_test(
  NAME espnow_session_replay
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_espnow_session_replay.py
          ${ESPNOW_SESSION_REPLAY_TEST_ARGS})

# ESP-NOW session rekey retry (README "ESP-NOW session rekey retry"): production REKEY
# initiator/responder/retry sweep and session lifecycle against modelled peers.
set(ESPNOW_REKEY_RETRY_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND ESPNOW_REKEY_RETRY_TEST_ARGS --sanitize)
endif()
add_test(
  NAME espnow_rekey_retry
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_espnow_rekey_retry.py
          ${ESPNOW_REKEY_RETRY_TEST_ARGS})

# ESP-NOW V4 dispatch policy: the real opcode table, flag constants, lookup and
# dispatcher with generated handler doubles, compiled with ENABLE_BONDED_MODE 1
# and 0 (README "ESP-NOW" area, espnow_dispatch_policy paragraph).
set(ESPNOW_DISPATCH_POLICY_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND ESPNOW_DISPATCH_POLICY_TEST_ARGS --sanitize)
endif()
add_test(
  NAME espnow_dispatch_policy
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_espnow_dispatch_policy.py
          ${ESPNOW_DISPATCH_POLICY_TEST_ARGS})

# ESP-NOW V4 fragment reassembler: v4_reasm_accept / v4_reasm_release / dispatch cleanup on
# crafted post-unwrap fragments, plus a run with no reassembly table.
set(ESPNOW_REASSEMBLY_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND ESPNOW_REASSEMBLY_TEST_ARGS --sanitize)
endif()
add_test(
  NAME espnow_reassembly
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_espnow_reassembly.py
          ${ESPNOW_REASSEMBLY_TEST_ARGS})

# ESP-NOW deadlines across the millis() wrap (README "ESP-NOW deadlines across the millis() wrap"):
# production prev-key window, sensor-lease and session pre-warm cooldown owners driven
# by an injectable clock on both sides of the 32-bit rollover.
set(ESPNOW_DEADLINE_WRAP_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND ESPNOW_DEADLINE_WRAP_TEST_ARGS --sanitize)
endif()
add_test(
  NAME espnow_deadline_wrap
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_espnow_deadline_wrap.py
          ${ESPNOW_DEADLINE_WRAP_TEST_ARGS})

# ESP-NOW peer message store (README "ESP-NOW peer message store"): the production
# slot table, rings, merged/collapsed reads and copy accessor with FreeRTOS mutex,
# allocation and clock doubles; refs survive peer growth, reads never allocate.
set(ESPNOW_MESSAGE_STORE_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND ESPNOW_MESSAGE_STORE_TEST_ARGS --sanitize)
endif()
add_test(
  NAME espnow_message_store
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_espnow_message_store.py
          ${ESPNOW_MESSAGE_STORE_TEST_ARGS})

# ESP-NOW FS RPC lock discipline: client sends off-lock, the cmd_exec runner
# always releases the deferred slot, capture answers TOO_BUSY on a lock miss
# (README "ESP-NOW FS RPC locking").
set(ESPNOW_FSLIST_LOCKING_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND ESPNOW_FSLIST_LOCKING_TEST_ARGS --sanitize)
endif()
add_test(
  NAME espnow_fslist_locking
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_espnow_fslist_locking.py
          ${ESPNOW_FSLIST_LOCKING_TEST_ARGS})

# ESP-NOW multi-frame sends: a synchronous payload over one frame runs its
# fragment/ACK loop on the submitting task, never on the espnow_tx dispatcher,
# and STREAM lines are clamped to one frame for their target.
set(ESPNOW_TX_MULTIFRAME_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND ESPNOW_TX_MULTIFRAME_TEST_ARGS --sanitize)
endif()
add_test(
  NAME espnow_tx_multiframe
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_espnow_tx_multiframe.py
          ${ESPNOW_TX_MULTIFRAME_TEST_ARGS})

# ESP-NOW metadata push: local room/zone/tags/name/stationary changes reach
# session-active peers as METADATA_PUSH (README "ESP-NOW metadata push").
set(ESPNOW_METADATA_PUSH_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND ESPNOW_METADATA_PUSH_TEST_ARGS --sanitize)
endif()
add_test(
  NAME espnow_metadata_push
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_espnow_metadata_push.py
          ${ESPNOW_METADATA_PUSH_TEST_ARGS})

# OLED ESP-NOW unpair and discovery ownership (README "OLED ESP-NOW unpair and
# discovery ownership"): the production OLED unpair adapter and discovery input
# arm with the command executor, toast and ESP-NOW owner as recorded doubles.
set(OLED_ESPNOW_OWNERSHIP_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND OLED_ESPNOW_OWNERSHIP_TEST_ARGS --sanitize)
endif()
add_test(
  NAME oled_espnow_ownership
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_oled_espnow_ownership.py
          ${OLED_ESPNOW_OWNERSHIP_TEST_ARGS})

set(OLED_REMOTE_SETTINGS_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND OLED_REMOTE_SETTINGS_TEST_ARGS --sanitize)
endif()
add_test(NAME oled_remote_settings
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_oled_remote_settings.py
          ${OLED_REMOTE_SETTINGS_TEST_ARGS})

set(STT_BLE_PRIVACY_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND STT_BLE_PRIVACY_TEST_ARGS --sanitize)
endif()
add_test(
  NAME stt_ble_privacy
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_stt_ble_privacy.py
          ${STT_BLE_PRIVACY_TEST_ARGS})

set(BLE_SC_DIRECTED_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND BLE_SC_DIRECTED_TEST_ARGS --sanitize)
endif()
add_test(
  NAME ble_sc_directed
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_ble_sc_directed.py
          ${BLE_SC_DIRECTED_TEST_ARGS})

set(RING_DAILY_ACK_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND RING_DAILY_ACK_TEST_ARGS --sanitize)
endif()
add_test(
  NAME ring_daily_ack
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_ring_daily_ack.py
          ${RING_DAILY_ACK_TEST_ARGS})

# BLE server stop: deinitBluetooth() reports deferred/failed stops; closeble and
# blemode client keep bleMode and the app sessions (README "Bluetooth server stop").
set(BLE_SERVER_STOP_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND BLE_SERVER_STOP_TEST_ARGS --sanitize)
endif()
add_test(
  NAME ble_server_stop
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_ble_server_stop.py
          ${BLE_SERVER_STOP_TEST_ARGS})

# BLE Secure Channel policy: the required-mode refusal reaches the plaintext writer,
# file verbs run only when the command arrived encrypted, and the posture
# commands share the superadmin tier (README "BLE Secure Channel policy").
set(BLE_SC_POLICY_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND BLE_SC_POLICY_TEST_ARGS --sanitize)
endif()
add_test(
  NAME ble_sc_policy
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_ble_sc_policy.py
          ${BLE_SC_POLICY_TEST_ARGS})

set(BLE_AREA_CLEANUP_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND BLE_AREA_CLEANUP_TEST_ARGS --sanitize)
endif()
add_test(
  NAME ble_area_cleanup
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_ble_area_cleanup.py
          ${BLE_AREA_CLEANUP_TEST_ARGS})

set(RING_FORWARDED_TELEMETRY_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND RING_FORWARDED_TELEMETRY_TEST_ARGS --sanitize)
endif()
add_test(NAME ring_forwarded_telemetry
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_ring_forwarded_telemetry.py
          ${RING_FORWARDED_TELEMETRY_TEST_ARGS})

set(G2_NETWORK_ASYNC_REDRAW_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND G2_NETWORK_ASYNC_REDRAW_TEST_ARGS --sanitize)
endif()
add_test(
  NAME g2_network_async_redraw
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_g2_network_async_redraw.py
          ${G2_NETWORK_ASYNC_REDRAW_TEST_ARGS})

# Sensors completions (cam_pwr post-hook, openmic/closemic on cmd_exec) queue a
# generation-checked Redraw instead of rendering on their own task (README
# "G2 Sensors completion redraws").
set(G2_SENSORS_REDRAW_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND G2_SENSORS_REDRAW_TEST_ARGS --sanitize)
endif()
add_test(
  NAME g2_sensors_redraw
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_g2_sensors_redraw.py
          ${G2_SENSORS_REDRAW_TEST_ARGS})

# G2 list pages publish their mode / row map only after the lens accepted the
# rows (README "G2 list pages under refused renders"): Files chooser, delete
# confirm and info overlay, Settings levels and row maps, LED sub-levels.
set(G2_PAGE_PUBLISH_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND G2_PAGE_PUBLISH_TEST_ARGS --sanitize)
endif()
add_test(
  NAME g2_page_publish
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_g2_page_publish.py
          ${G2_PAGE_PUBLISH_TEST_ARGS})

# G2 page API (README "G2 page and hijack-bridge API contract"):
# no page, bridge or FSM entry point without a caller, enabled or disabled.
set(G2_PAGE_API_CONTRACT_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND G2_PAGE_API_CONTRACT_TEST_ARGS --sanitize)
endif()
add_test(
  NAME g2_page_api_contract
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_g2_page_api_contract.py
          ${G2_PAGE_API_CONTRACT_TEST_ARGS})

# File browsers (README "File browser classification and rename/delete authority"):
# one classifier for the G2 lens and OLED browsers; OLED rename/delete through the executor.
set(FILE_KIND_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND FILE_KIND_TEST_ARGS --sanitize)
endif()
add_test(
  NAME file_kind
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_file_kind.py
          ${FILE_KIND_TEST_ARGS})

# Login policy ownership (README "Display login policy ownership"): every settings row and
# command that writes serial/UART/display RequireAuth is super-admin, one row per policy.
set(DISPLAY_AUTH_POLICY_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND DISPLAY_AUTH_POLICY_TEST_ARGS --sanitize)
endif()
add_test(
  NAME display_auth_policy
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_display_auth_policy.py
          ${DISPLAY_AUTH_POLICY_TEST_ARGS})
# settings_mutation is already registered; its extension needs no new snippet.

# OLED session role (README "OLED session role cache"): the launcher, guest gate and Users
# row read users.json at most once per session and identity generation, fail closed.
set(OLED_SESSION_ROLE_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND OLED_SESSION_ROLE_TEST_ARGS --sanitize)
endif()
add_test(
  NAME oled_session_role
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_oled_session_role.py
          ${OLED_SESSION_ROLE_TEST_ARGS})

set(OLED_OWNER_REQUESTS_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND OLED_OWNER_REQUESTS_TEST_ARGS --sanitize)
endif()
add_test(NAME oled_owner_requests
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_oled_owner_requests.py
    ${OLED_OWNER_REQUESTS_TEST_ARGS})

# OLED mode availability (README "OLED mode availability"): getMenuAvailability and the
# production mode registry with the real availFuncs of CLI Input, Gamepad PW, Logout,
# Actions, GPS and thermal.
set(OLED_MODE_AVAILABILITY_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND OLED_MODE_AVAILABILITY_TEST_ARGS --sanitize)
endif()
add_test(NAME oled_mode_availability
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_oled_mode_availability.py
    ${OLED_MODE_AVAILABILITY_TEST_ARGS})

# Dead OLED code paths (README "Dead OLED code paths"): a source contract over the
# component, plus the production OLED mode slug functions.
set(OLED_DEAD_PATHS_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND OLED_DEAD_PATHS_TEST_ARGS --sanitize)
endif()
add_test(NAME oled_dead_paths
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_oled_dead_paths.py
    ${OLED_DEAD_PATHS_TEST_ARGS})

# OLED action feedback (README "OLED action feedback"): a refused OLED action toasts the
# command's reply; success texts only on success; no dropped replies on the listed screens.
set(OLED_COMMAND_FEEDBACK_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND OLED_COMMAND_FEEDBACK_TEST_ARGS --sanitize)
endif()
add_test(
  NAME oled_command_feedback
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_oled_command_feedback.py
          ${OLED_COMMAND_FEEDBACK_TEST_ARGS})

# OLED Power Saving row (README "OLED Power Saving row"): the row submits the admin-only
# powersave command; no OLED_Mode_*.cpp writes settings directly.
set(OLED_POWER_SETTINGS_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND OLED_POWER_SETTINGS_TEST_ARGS --sanitize)
endif()
add_test(
  NAME oled_power_settings
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_oled_power_settings.py
          ${OLED_POWER_SETTINGS_TEST_ARGS})

# OLED automation wizard (README "OLED automation wizard"): names and commands up to the
# keyboard cap reach automationadd whole; the confirm screen shows the whole command.
set(OLED_AUTOMATION_WIZARD_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND OLED_AUTOMATION_WIZARD_TEST_ARGS --sanitize)
endif()
add_test(
  NAME oled_automation_wizard
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_oled_automation_wizard.py
          ${OLED_AUTOMATION_WIZARD_TEST_ARGS})

# OLED keyboard masking and dictation delivery (README "OLED keyboard masking and dictation UI"):
# shared OLED/G2 dictation delivery, and a masked (secret) keyboard field never shows its text.
set(DICTATION_UI_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND DICTATION_UI_TEST_ARGS --sanitize)
endif()
add_test(
  NAME dictation_ui
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_dictation_ui.py
          ${DICTATION_UI_TEST_ARGS})

# OLED pair-request prompt (README "OLED pair-request prompt"): the dialog opens once per
# request, only for a logged-in admin off the Login screen, and names the shown sender.
set(OLED_PAIR_PROMPT_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND OLED_PAIR_PROMPT_TEST_ARGS --sanitize)
endif()
add_test(
  NAME oled_pair_prompt
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_oled_pair_prompt.py
          ${OLED_PAIR_PROMPT_TEST_ARGS})

# OLED list navigation (README "OLED list navigation"): Automations and Files move one row per
# nav event; the settings editor's scroll windows are per module.
set(OLED_LIST_NAV_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND OLED_LIST_NAV_TEST_ARGS --sanitize)
endif()
add_test(
  NAME oled_list_nav
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_oled_list_nav.py
          ${OLED_LIST_NAV_TEST_ARGS})
# test_sensorlog_ui.py is already registered (sensorlog_ui); its extension needs no snippet.

set(OLED_MAP_ACTIONS_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND OLED_MAP_ACTIONS_TEST_ARGS --sanitize)
endif()
add_test(
  NAME oled_map_actions
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_oled_map_actions.py
          ${OLED_MAP_ACTIONS_TEST_ARGS})

set(OLED_MAP_CONTEXT_BAR_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND OLED_MAP_CONTEXT_BAR_TEST_ARGS --sanitize)
endif()
add_test(
  NAME oled_map_context_bar
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_oled_map_context_bar.py
          ${OLED_MAP_CONTEXT_BAR_TEST_ARGS})
add_test(NAME p4eye_display_pixels COMMAND p4eye_display_pixels_tests)
add_test(NAME p4eye_display_lifecycle COMMAND p4eye_display_lifecycle_tests)
add_test(NAME rotary_core
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_rotary_core.py)
add_test(NAME gpio_buttons
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_gpio_buttons.py)
add_test(NAME gpio_encoder_integration
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_gpio_encoder_integration.py)
add_test(NAME input_button_snapshot
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_input_button_snapshot.py)
add_test(NAME optional_peripheral_config
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_optional_peripheral_config.py)
add_test(NAME saved_build_config
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_saved_build_config.py)
add_test(NAME build_config_workflow
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_build_config_workflow.py)
add_test(NAME build_config_wizard
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_build_config_wizard.py)
add_test(NAME xiao_peripheral_config
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_xiao_peripheral_config.py)
add_test(NAME sdkconfig_policy
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_sdkconfig_policy.py)
add_test(NAME command_header_boundary
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_command_header_boundary.py
    --cxx ${CMAKE_CXX_COMPILER})
set(COMMAND_CATALOGUE_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND COMMAND_CATALOGUE_TEST_ARGS --sanitize)
endif()
add_test(NAME command_catalogue
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_command_catalogue.py
    ${COMMAND_CATALOGUE_TEST_ARGS})
set(NTP_TRANSPORT_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND NTP_TRANSPORT_TEST_ARGS --sanitize)
endif()
add_test(NAME ntp_transport
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_ntp_transport.py
    ${NTP_TRANSPORT_TEST_ARGS})
set(WIFI_NETWORKS_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND WIFI_NETWORKS_TEST_ARGS --sanitize)
endif()
add_test(NAME wifi_networks
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_wifi_networks.py
    ${WIFI_NETWORKS_TEST_ARGS})
add_test(NAME wifi_network_consumers
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_wifi_network_consumers.py)
add_test(NAME wifi_saved_ui
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_wifi_saved_ui.py
    ${WIFI_NETWORKS_TEST_ARGS})
set(GPS_SNAPSHOT_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND GPS_SNAPSHOT_TEST_ARGS --sanitize)
endif()
add_test(NAME gps_snapshot
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_gps_snapshot.py
    ${GPS_SNAPSHOT_TEST_ARGS})
set(PRESENCE_SNAPSHOT_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND PRESENCE_SNAPSHOT_TEST_ARGS --sanitize)
endif()
add_test(NAME presence_snapshot
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_presence_snapshot.py
    ${PRESENCE_SNAPSHOT_TEST_ARGS})
set(SENSOR_READING_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND SENSOR_READING_TEST_ARGS --sanitize)
endif()
add_test(NAME small_sensor_snapshots
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_small_sensor_snapshots.py
    ${SENSOR_READING_TEST_ARGS})
add_test(NAME apds_read_commands
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_apds_read_commands.py
    ${SENSOR_READING_TEST_ARGS})
add_test(NAME apds_gesture_driver
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_apds_gesture_driver.py
    ${SENSOR_READING_TEST_ARGS})
add_test(NAME tof_read_command
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_tof_read_command.py
    ${SENSOR_READING_TEST_ARGS})
add_test(NAME thermal_snapshot
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_thermal_snapshot.py
    ${SENSOR_READING_TEST_ARGS})
add_test(NAME fm_radio_snapshot
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_fm_radio_snapshot.py
    ${SENSOR_READING_TEST_ARGS})
add_test(NAME servo_command_outcomes
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_servo_command_outcomes.py
    ${SENSOR_READING_TEST_ARGS})
add_test(NAME sensor_reading_consumers
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_sensor_reading_consumers.py
    ${SENSOR_READING_TEST_ARGS})
add_test(NAME g2_disabled_header
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_g2_disabled_header.py
    ${SENSOR_READING_TEST_ARGS})
add_test(NAME oled_without_input
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_oled_without_input.py
    --cxx ${CMAKE_CXX_COMPILER})
set(DISPLAY_LIFECYCLE_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND DISPLAY_LIFECYCLE_TEST_ARGS --sanitize)
endif()
add_test(NAME display_lifecycle
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_display_lifecycle.py
    ${DISPLAY_LIFECYCLE_TEST_ARGS})
add_test(NAME mqtt_feature_config
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_mqtt_feature_config.py
          --cxx ${CMAKE_CXX_COMPILER})
add_test(NAME network_build_selection
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_network_build_selection.py
          --cxx ${CMAKE_CXX_COMPILER} --cmake ${CMAKE_COMMAND})
set(ESPNOW_INTERFACE_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND ESPNOW_INTERFACE_TEST_ARGS --sanitize)
endif()
add_test(NAME espnow_disabled_interface
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_espnow_disabled_interface.py
          ${ESPNOW_INTERFACE_TEST_ARGS})
set(SDMMC_TEST_ARGS)
if(HW1_SANITIZE)
  list(APPEND SDMMC_TEST_ARGS --sanitize)
endif()
add_test(NAME sdmmc_lifecycle
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/test_sdmmc.py ${SDMMC_TEST_ARGS})
add_test(NAME ht16k33_geometry_tests COMMAND ht16k33_geometry_tests)
add_test(NAME llm_utf8_tests COMMAND llm_utf8_tests)
add_test(NAME dictation_policy_tests COMMAND dictation_policy_tests)
add_test(NAME g2_hold_gesture_tests COMMAND g2_hold_gesture_tests)
add_test(NAME quicknote_policy_tests COMMAND quicknote_policy_tests)
add_test(NAME mqtt_lifecycle_gate_tests COMMAND mqtt_lifecycle_gate_tests)
add_test(NAME memutil_tests COMMAND memutil_tests)
add_test(NAME memutil_no_psram COMMAND memutil_no_psram_tests)
add_test(NAME memtracker_core_tests COMMAND memtracker_core_tests)
add_test(NAME map_viewport_core_tests COMMAND map_viewport_core_tests)
add_test(NAME g2_health_graph_core_tests COMMAND g2_health_graph_core_tests)
add_test(NAME event_kind_mask_tests COMMAND event_kind_mask_tests)
add_test(NAME command_limit_tests COMMAND command_limit_tests)
add_test(NAME command_lookup_core_tests COMMAND command_lookup_core_tests)
add_test(NAME help_pager_core_tests COMMAND help_pager_core_tests)
add_test(NAME settings_write_batch_core_tests
         COMMAND settings_write_batch_core_tests)
add_test(NAME notification_kind_list_tests COMMAND notification_kind_list_tests)
add_test(NAME i2c_transaction_options_tests COMMAND i2c_transaction_options_tests)
add_test(NAME vfs_capacity_cache_tests COMMAND vfs_capacity_cache_tests)
add_test(NAME event_catalog_tests COMMAND event_catalog_tests)
add_test(NAME event_catalog_json_tests COMMAND event_catalog_json_tests)
add_test(NAME event_catalog_text_tests COMMAND event_catalog_text_tests)
add_test(NAME stt_lm_tests COMMAND stt_lm_tests)
add_test(
  NAME stt_lm_parity
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_stt_lm_parity.py
          --tool $<TARGET_FILE:stt_lm_tool>
          --fixtures ${CMAKE_CURRENT_LIST_DIR}/fixtures/stt_lm)
# No manifest yet (the host builder writes it): reported as skipped, not passed.
set_tests_properties(stt_lm_parity PROPERTIES SKIP_RETURN_CODE 77)
# The actual System_STTLocal.cpp LM load/cache/fallback policy, flag on and off.
set(STT_LOCAL_LM_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND STT_LOCAL_LM_TEST_ARGS --sanitize)
endif()
add_test(
  NAME stt_local_lm
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_stt_local_lm.py
          ${STT_LOCAL_LM_TEST_ARGS})
add_test(
  NAME event_catalog_json_parse
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_event_catalog_json_parse.py
          $<TARGET_FILE:event_catalog_json_tests>)
add_test(
  NAME event_catalog_fixture
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_event_catalog_fixture.py)
add_test(
  NAME event_catalog_structure
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_event_catalog_structure.py)
add_test(
  NAME command_ingress_guards
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_command_ingress_guards.py)
add_test(
  NAME help_pagination_contract
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_help_pagination_contract.py)
add_test(
  NAME settings_write_batch_contract
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_settings_write_batch_contract.py)
set(SETTINGS_MUTATION_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND SETTINGS_MUTATION_TEST_ARGS --sanitize)
endif()
add_test(
  NAME settings_mutation
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_settings_mutation.py
          ${SETTINGS_MUTATION_TEST_ARGS})
add_test(
  NAME settings_write_scope
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_settings_write_scope.py
          ${SETTINGS_MUTATION_TEST_ARGS})
add_test(
  NAME settings_file_writes
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_settings_file_writes.py
          ${SETTINGS_MUTATION_TEST_ARGS})
set(WEB_BATCH_HANDLER_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND WEB_BATCH_HANDLER_TEST_ARGS --sanitize)
endif()
add_test(
  NAME web_batch_handlers
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_web_batch_handlers.py
          ${WEB_BATCH_HANDLER_TEST_ARGS})
add_test(
  NAME web_file_response_policy
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_web_file_response_policy.py
          ${WEB_BATCH_HANDLER_TEST_ARGS})
add_test(
  NAME settings_downstream_persistence_contract
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_settings_downstream_persistence_contract.py)
add_test(
  NAME command_resolution_contract
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_command_resolution_contract.py)
add_test(
  NAME mqtt_lifecycle_contract
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_mqtt_lifecycle_contract.py)
set(SETUP_MQTT_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND SETUP_MQTT_TEST_ARGS --sanitize)
endif()
add_test(
  NAME setup_mqtt_application
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_setup_mqtt_application.py
          ${SETUP_MQTT_TEST_ARGS})
add_test(
  NAME mqtt_boolean_commands
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_mqtt_boolean_commands.py
          ${SETUP_MQTT_TEST_ARGS})
add_test(
  NAME ble_info_status
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_ble_info_status.py
          ${SETUP_MQTT_TEST_ARGS})
add_test(
  NAME ring_info_status
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_ring_info_status.py
          ${SETUP_MQTT_TEST_ARGS})
add_test(
  NAME g2_connection_status
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_g2_connection_status.py
          ${SETUP_MQTT_TEST_ARGS})
add_test(
  NAME g2_client_lifecycle
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_g2_client_lifecycle.py
          ${SETUP_MQTT_TEST_ARGS})
add_test(
  NAME g2_redraw
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_g2_redraw.py
          ${SETUP_MQTT_TEST_ARGS})
add_test(
  NAME g2_power_completion
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_g2_power_completion.py
          ${SETUP_MQTT_TEST_ARGS})
add_test(
  NAME g2_mic_completion
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_g2_mic_completion.py
          ${SETUP_MQTT_TEST_ARGS})
add_test(
  NAME sensorlog_selection
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_sensorlog_selection.py
          ${SETUP_MQTT_TEST_ARGS})
add_test(
  NAME sensorlog_ui
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_sensorlog_ui.py
          ${SETUP_MQTT_TEST_ARGS})
add_test(
  NAME sensorlog_settings
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_sensorlog_settings.py
          ${SETUP_MQTT_TEST_ARGS})
add_test(
  NAME gpslog_start
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_gpslog_start.py
          ${SETUP_MQTT_TEST_ARGS})
add_test(
  NAME map_live_tracking
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_map_live_tracking.py
          ${SETUP_MQTT_TEST_ARGS})
add_test(
  NAME map_live_ui
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_map_live_ui.py
          ${SETUP_MQTT_TEST_ARGS})
add_test(
  NAME health_logging_ui
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_health_logging_ui.py
          ${SETUP_MQTT_TEST_ARGS})
add_test(
  NAME g2_reopen
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_g2_reopen.py
          ${SETUP_MQTT_TEST_ARGS})
# The browser suites are always registered, so `ctest -N` lists them and a run
# without Node.js reports them as not run (disabled) instead of omitting them.
find_program(HW1_NODE_EXECUTABLE NAMES node nodejs)
set(HW1_NODE_CMD "${HW1_NODE_EXECUTABLE}")
if(NOT HW1_NODE_EXECUTABLE)
  set(HW1_NODE_CMD node)
endif()
add_test(
  NAME map_live_browser
  COMMAND ${HW1_NODE_CMD}
          ${CMAKE_CURRENT_LIST_DIR}/map_live_browser_test.js)
add_test(
  NAME g2_status_browser
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_g2_status_browser.py
          --node ${HW1_NODE_CMD})
add_test(
  NAME sensor_mic_card
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_sensor_mic_card.py
          --node ${HW1_NODE_CMD})
if(NOT HW1_NODE_EXECUTABLE)
  set_tests_properties(map_live_browser g2_status_browser sensor_mic_card
                       PROPERTIES DISABLED TRUE)
  message(WARNING "Node.js not found: map_live_browser, g2_status_browser and sensor_mic_card "
                  "are registered but disabled; pass -DHW1_NODE_EXECUTABLE=/path/to/node")
endif()
add_test(
  NAME setup_espnow_application
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_setup_espnow_application.py
          ${SETUP_MQTT_TEST_ARGS})
add_test(
  NAME setup_completion
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_setup_completion.py
          ${SETUP_MQTT_TEST_ARGS})
foreach(test_name IN ITEMS setup_draft setup_display_cancel climode_completion_owner initial_admin_setup
                           setup_mode_choices setup_menu_layout setup_wifi_navigation)
  add_test(
    NAME ${test_name}
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_LIST_DIR}/test_${test_name}.py
            ${SETUP_MQTT_TEST_ARGS})
endforeach()
set(POWER_CPU_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND POWER_CPU_TEST_ARGS --sanitize)
endif()
foreach(test_name IN ITEMS power_cpu_frequency power_save_tick)
  add_test(
    NAME ${test_name}
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_LIST_DIR}/test_${test_name}.py
            ${POWER_CPU_TEST_ARGS})
endforeach()
set(ICON_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND ICON_TEST_ARGS --sanitize)
endif()
add_test(
  NAME icon_ownership
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_icon_ownership.py
          ${ICON_TEST_ARGS})
add_test(
  NAME user_settings_transaction_guards
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_user_settings_transaction_guards.py)
add_test(
  NAME filesystem_listing_permissions_contract
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_filesystem_listing_permissions_contract.py)
set(FILESYSTEM_PSRAM_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND FILESYSTEM_PSRAM_TEST_ARGS --sanitize)
endif()
foreach(test_name IN ITEMS fileread_psram g2_files_psram filesystem_psram_listing
                           filemanager_directory espnow_directory checked_text_writes
                           upload_lifecycle storage_stats espnow_fs_transcript_privacy
                           image_storage image_writes capture_stitching capped_log_writes
                           log_row_writes r1_history_store mic_wav_completion boot_temp_cleanup)
  add_test(
    NAME ${test_name}
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_LIST_DIR}/test_${test_name}.py
            ${FILESYSTEM_PSRAM_TEST_ARGS})
endforeach()
add_test(
  NAME http_feature_gates
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_http_feature_gates.py
          --cxx ${CMAKE_CXX_COMPILER})
add_test(
  NAME notification_integration_guards
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_notification_integration_guards.py)
# Production osEventTypeName() names 0-10 and the G2 hold (long-press) RX hook
# order and producer/dispatcher guards in G2_Glasses.cpp. Uses the xcrun c++
# shim (clang++) so it needs no SDKROOT.
add_test(
  NAME g2_hold_decode
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_g2_hold_decode.py)
# The production G2 hold section (pairing, gates, g2_tap_disp routing, page
# side table, events, aborts, sim) and the SysEvent exit tail of
# handleDevEvent run against fakes. Same xcrun shim note.
add_test(
  NAME g2_hold_dispatch
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_g2_hold_dispatch.py)
# The production QuickNote store (create/list/read/delete with tmp+rename and
# the free-space reserve) against a fake guarded VFS whose owner decision is
# the production transcript path policy. Same xcrun shim note.
set(QUICKNOTE_STORE_TEST_ARGS)
if(HW1_SANITIZE)
  list(APPEND QUICKNOTE_STORE_TEST_ARGS --sanitize)
endif()
add_test(
  NAME quicknote_store
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_quicknote_store.py
          ${QUICKNOTE_STORE_TEST_ARGS})
# The production QuickNote service and `note` CLI (authority matrix, one note
# at a time, save/discard/fail, revocation, limits, private replies) against
# fakes, once per transcription backend, plus its event/settings/CLI wiring.
# Same xcrun shim note.
add_test(
  NAME quicknote_service
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_quicknote_service.py
          ${QUICKNOTE_STORE_TEST_ARGS})
# The production Notes lens page (views, holds on the page and through the
# global hold action, no container rebuild while a note records, stale replies,
# refusal without navigation, privacy) against fakes, plus its Apps row,
# watchdog keep-alive, container-ready mic re-arm, unlogged row labels and
# registration in G2_Glasses.cpp. Same xcrun shim note.
add_test(
  NAME g2_notes_page
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_g2_notes_page.py
          ${QUICKNOTE_STORE_TEST_ARGS})
add_test(
  NAME i2c_transaction_contract
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_i2c_transaction_contract.py)
set(I2C_PROBE_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND I2C_PROBE_TEST_ARGS --sanitize)
endif()
add_test(
  NAME i2c_probe
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_i2c_probe.py
          ${I2C_PROBE_TEST_ARGS})
add_test(
  NAME i2c_device_queue
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_i2c_device_queue.py
          ${I2C_PROBE_TEST_ARGS})
add_test(
  NAME sensor_lifecycle_requests
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_sensor_lifecycle_requests.py
          ${I2C_PROBE_TEST_ARGS})
add_test(
  NAME sensor_start_outcomes
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_sensor_start_outcomes.py
          ${I2C_PROBE_TEST_ARGS})
set(TOF_PSRAM_LIFECYCLE_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND TOF_PSRAM_LIFECYCLE_TEST_ARGS --sanitize)
endif()
add_test(
  NAME tof_psram_lifecycle
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_tof_psram_lifecycle.py
          ${TOF_PSRAM_LIFECYCLE_TEST_ARGS})
add_test(
  NAME tof_poll_outcomes
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_tof_poll_outcomes.py
          ${TOF_PSRAM_LIFECYCLE_TEST_ARGS})
set(SENSOR_PARENT_LIFECYCLE_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND SENSOR_PARENT_LIFECYCLE_TEST_ARGS --sanitize)
endif()
add_test(
  NAME sensor_parent_lifecycle
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_sensor_parent_lifecycle.py
          ${SENSOR_PARENT_LIFECYCLE_TEST_ARGS})

set(SENSOR_TASK_RESTART_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND SENSOR_TASK_RESTART_TEST_ARGS --sanitize)
endif()
add_test(
  NAME sensor_task_restart
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_sensor_task_restart.py
          ${SENSOR_TASK_RESTART_TEST_ARGS})

set(OTA_PSRAM_REPLIES_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND OTA_PSRAM_REPLIES_TEST_ARGS --sanitize)
endif()
add_test(
  NAME live_audio
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_live_audio.py
          ${OTA_PSRAM_REPLIES_TEST_ARGS})
add_test(
  NAME ota_psram_replies
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_ota_psram_replies.py
          ${OTA_PSRAM_REPLIES_TEST_ARGS})
add_test(
  NAME vfs_capacity_snapshot_contract
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_vfs_capacity_snapshot_contract.py)
add_test(
  NAME automation_scheduler_contract
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_automation_scheduler_contract.py)
add_test(
  NAME automation_scheduler
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_automation_scheduler.py
          ${SETUP_MQTT_TEST_ARGS})
add_test(
  NAME automation_log
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_automation_log.py
          ${SETUP_MQTT_TEST_ARGS})
add_test(
  NAME session_snapshot
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_session_snapshot.py
          ${SETUP_MQTT_TEST_ARGS})
add_test(
  NAME task_reporting
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_task_reporting.py
          ${SETUP_MQTT_TEST_ARGS})
# The .3 sharing milestones: log-level owner and its CLI/boot callers, the
# system-log status/flag formatters, and the shared command-origin prefix.
foreach(test_name IN ITEMS log_level_application system_log_status command_feed)
  add_test(
    NAME ${test_name}
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_LIST_DIR}/test_${test_name}.py
            ${SETUP_MQTT_TEST_ARGS})
endforeach()
add_test(
  NAME raw_allocation_inventory
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/../../../../tools/check_raw_allocations.py
          # Ratchet, not a target: the ps_alloc/AllocPolicy unification retired six
          # raw sites (17 -> 11). Lower it when sites go away; a rise means
          # someone reached past System_MemUtil again. The malloc/calloc split
          # (8/3) is what the tree has carried since 0.99.94; the earlier 6 was
          # recorded before two calloc sites became malloc and never updated.
          --expect 11 --expect-malloc 8)
