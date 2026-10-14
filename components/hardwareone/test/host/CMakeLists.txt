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

set(LLM_HTTP_TEST_ARGS)
if(HW1_SANITIZE)
  list(APPEND LLM_HTTP_TEST_ARGS --sanitize)
endif()
foreach(_http_test llm_http llm_http_runtime llm_http_integration llm_http_settings llm_guided_source llm_turn_serving)
  add_test(NAME ${_http_test}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_${_http_test}.py
            ${LLM_HTTP_TEST_ARGS})
endforeach()

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

# Bond JSON buffers (README "Bond manifest and schema buffers"): the manifest and schema
# are serialized, received, cached and read back through one checked buffer, never a String.
set(ESPNOW_BOND_JSON_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND ESPNOW_BOND_JSON_TEST_ARGS --sanitize)
endif()
add_test(
  NAME espnow_bond_json
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_espnow_bond_json.py
          ${ESPNOW_BOND_JSON_TEST_ARGS})
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
# Full-width reply pagination, real wire framing, reader lifecycle and optional
# microphone entry compile the production code against bounded host fakes.
set(G2_LLM_READER_TEST_ARGS)
if(HW1_SANITIZE)
  list(APPEND G2_LLM_READER_TEST_ARGS --sanitize)
endif()
foreach(reader_suite g2_evenai_local g2_llm_reader g2_llm_reader_runtime g2_text_entry_start)
  add_test(NAME ${reader_suite}
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_LIST_DIR}/test_${reader_suite}.py
            ${G2_LLM_READER_TEST_ARGS})
endforeach()
# Host-side Terminal projection includes the namespaced ask_user regression.
find_program(HW1_NODE_EXECUTABLE NAMES node)
if(HW1_NODE_EXECUTABLE)
  add_test(NAME openclaw_terminal
    COMMAND ${HW1_NODE_EXECUTABLE} --test
            ${CMAKE_CURRENT_LIST_DIR}/../../../../integrations/openclaw/terminal/test_provider.mjs
            ${CMAKE_CURRENT_LIST_DIR}/../../../../integrations/openclaw/terminal/tests/device-api.test.mjs)
else()
  message(STATUS "OpenClaw Terminal host tests need Node.js; set HW1_NODE_EXECUTABLE to enable them")
endif()

# This regression compiles SDK internals, so the intended SDK must be selected
# explicitly instead of silently using an older installed IDF_PATH.
set(HW1_ESP_HOSTED_TEST_IDF "" CACHE PATH "ESP-IDF 5.5.5 sources for the Hosted DMA regression")
if(HW1_ESP_HOSTED_TEST_IDF)
  add_test(NAME esp_hosted_mempool
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_LIST_DIR}/../../../../tools/p4/test_esp_hosted_mempool.py
            --idf ${HW1_ESP_HOSTED_TEST_IDF} --cc ${CMAKE_C_COMPILER}
            ${G2_LLM_READER_TEST_ARGS})
else()
  message(STATUS "Hosted DMA regression needs HW1_ESP_HOSTED_TEST_IDF pointing to ESP-IDF 5.5.5 sources")
endif()
foreach(terminal_suite g2_terminal_protocol g2_native_terminal)
  add_test(NAME ${terminal_suite}
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_LIST_DIR}/test_${terminal_suite}.py
            ${G2_LLM_READER_TEST_ARGS})
endforeach()
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

# BLE and display login lockout (README "BLE and display login lockout"): the real
# bleLoginDeferred and loginTransport against the real attempt table; the sixth
# attempt is refused before any password check, replies stay on the requesting
# session, and a lost bind, an unpublished session or an unavailable store is not a guess.
set(BLE_DISPLAY_LOGIN_LOCKOUT_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND BLE_DISPLAY_LOGIN_LOCKOUT_TEST_ARGS --sanitize)
endif()
add_test(
  NAME ble_display_login_lockout
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_ble_display_login_lockout.py
          ${BLE_DISPLAY_LOGIN_LOCKOUT_TEST_ARGS})

# ESP-NOW passwords and USER_SYNC accounts (README "ESP-NOW passwords and USER_SYNC
# accounts"): password remote commands and USER_SYNC's admin check share ESP-NOW's
# lockout key (the bond-token path stays outside); USER_SYNC creates accounts through
# createAccount on the shared fake VFS (validation, reserved names, id high-water mark,
# credential written before users.json).
set(ESPNOW_USER_CREDENTIALS_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND ESPNOW_USER_CREDENTIALS_TEST_ARGS --sanitize)
endif()
add_test(
  NAME espnow_user_credentials
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_espnow_user_credentials.py
          ${ESPNOW_USER_CREDENTIALS_TEST_ARGS})

# executeCommand runs only on the command executor (README "Command executor
# admission"): the real admission predicate and the opening of executeCommand
# admit cmd_exec and the early-boot path, refuse any other task before a side
# effect; the ESP-NOW status endpoint submits its eight status commands.
set(COMMAND_EXECUTOR_ADMISSION_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND COMMAND_EXECUTOR_ADMISSION_TEST_ARGS --sanitize)
endif()
add_test(
  NAME command_executor_admission
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_command_executor_admission.py
          ${COMMAND_EXECUTOR_ADMISSION_TEST_ARGS})

# Settings schema setters and the bonded remote-edit policy (README "Settings
# schema setters and the bonded remote-edit policy"): the schema carries cmdKey
# only for a registered setter; BondedPeer narrows the worker schema the web
# bonded view receives; the settings page keeps no policy copy and never derives
# a command from a key.
set(SETTINGS_SCHEMA_SETTERS_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND SETTINGS_SCHEMA_SETTERS_TEST_ARGS --sanitize)
endif()
add_test(
  NAME settings_schema_setters
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_settings_schema_setters.py
          ${SETTINGS_SCHEMA_SETTERS_TEST_ARGS})

# remote:/@ authority (README "remote:/@ commands and the bonded peer's
# authority"): the peer runs every bonded command as bond-admin (super), so the
# routing in executeCommand needs a local administrator except for the peer
# sensor start/stop lines the bond page and the OLED bond screen send; admin
# replies (msgId), nested refusal, guest gate and redacted audit unchanged.
set(REMOTE_COMMAND_AUTHORITY_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND REMOTE_COMMAND_AUTHORITY_TEST_ARGS --sanitize)
endif()
add_test(
  NAME remote_command_authority
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_remote_command_authority.py
          ${REMOTE_COMMAND_AUTHORITY_TEST_ARGS})

# Continuous STT broker (System_STT.cpp): the production default trims each
# closing phrase to its voiced end + 500 ms with speculative finals; the
# full-phrase variant builds the -DHW1_STT_TRIM_SILENCE=0 opt-out.
set(STT_CONTINUOUS_TEST_ARGS)
if(HW1_SANITIZE)
  list(APPEND STT_CONTINUOUS_TEST_ARGS --sanitize)
endif()
add_test(
  NAME stt_continuous
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_stt_continuous.py
          ${STT_CONTINUOUS_TEST_ARGS})
add_test(
  NAME stt_continuous_full_phrase
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_stt_continuous.py --full-phrase
          ${STT_CONTINUOUS_TEST_ARGS})

# STT result privacy and the targeted BLE command result (README "STT result privacy
# and the targeted BLE reply"): the private-STT rule, and since D1 the BLE reply is
# fenced on the command's session epoch with no console-mirror fallback.
# test_stt_privacy.py existed on main but was never registered; it now carries D1.
set(STT_PRIVACY_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND STT_PRIVACY_TEST_ARGS --sanitize)
endif()
add_test(
  NAME stt_privacy
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_stt_privacy.py
          ${STT_PRIVACY_TEST_ARGS})

# BLE console mirror recipients (README "BLE console mirror recipients"): the drain
# buffers only while sendBLEResponse has a recipient; `outble` reports the count.
set(BLE_MIRROR_RECIPIENTS_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND BLE_MIRROR_RECIPIENTS_TEST_ARGS --sanitize)
endif()
add_test(
  NAME ble_mirror_recipients
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_ble_mirror_recipients.py
          ${BLE_MIRROR_RECIPIENTS_TEST_ARGS})

# C6 companion BLE shutdown (README "C6 companion BLE shutdown"): the companion reset
# stops the BLE server only through deinitBluetooth(), retried a bounded number of
# times, and never forces the host down under a busy Ring owner.
set(COMPANION_BLE_SHUTDOWN_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND COMPANION_BLE_SHUTDOWN_TEST_ARGS --sanitize)
endif()
add_test(
  NAME companion_ble_shutdown
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_companion_ble_shutdown.py
          ${COMPANION_BLE_SHUTDOWN_TEST_ARGS})

# Retired BLE code (README "Retired BLE code"): BLE_IDF.cpp/.h and their flag are gone,
# debugringbridge is retired without moving another debug flag, and the flag table
# compiles on the host with its static_asserts.
set(BLE_RETIRED_CODE_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND BLE_RETIRED_CODE_TEST_ARGS --sanitize)
endif()
add_test(
  NAME ble_retired_code
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_ble_retired_code.py
          ${BLE_RETIRED_CODE_TEST_ARGS})

# Module overview text (README "Module overview text") and repository tool pins
# (README "Repository tool pins"). Source text and Python only; nothing is compiled.
foreach(test_name IN ITEMS module_overview_text tool_pins)
  add_test(
    NAME ${test_name}
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_LIST_DIR}/test_${test_name}.py)
endforeach()

# OLED display-session writer windows (README "OLED display-session writer windows"):
# the frame commit and the command-result path validate under the display lifecycle
# writer, release it, push or copy, then re-check without opening a session.
set(OLED_COMMIT_WINDOW_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND OLED_COMMIT_WINDOW_TEST_ARGS --sanitize)
endif()
add_test(
  NAME oled_commit_window
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_oled_commit_window.py
          ${OLED_COMMIT_WINDOW_TEST_ARGS})

# OLED leftovers cleared after the merge (README "OLED merge leftovers"): the unified
# actions placeholder file, the display-off stubs, the failed-login counter comment
# and the file-verb comments. Source text only; nothing is compiled.
add_test(
  NAME oled_merge_leftovers
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_oled_merge_leftovers.py)

# ESP-NOW radio ownership (README "ESP-NOW radio ownership"): espnowOwnsRadio is
# initialized-or-closing with a compiled-out stub, and the five System_WiFi radio
# decisions ask it instead of isEspNowInitialized().
# Camera asynchronous power (README "Camera asynchronous power"): `opencamera async` /
# `closecamera async` are gated on cameraEnabled and admitted by cam_pwr; the G2
# Sensors page requests no camera power itself.
# Structured Add-WiFi (README "Structured Add-WiFi"): wifiAddCommandLine's line parses
# back through CommandArgs to the given fields, and cmd_wifiadd names a control-character
# refusal.
set(DEVICES_TEST_ARGS --cxx ${CMAKE_CXX_COMPILER})
if(HW1_SANITIZE)
  list(APPEND DEVICES_TEST_ARGS --sanitize)
endif()
foreach(test_name IN ITEMS espnow_radio_ownership camera_async_power wifi_structured_add)
  add_test(
    NAME ${test_name}
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_LIST_DIR}/test_${test_name}.py
            ${DEVICES_TEST_ARGS})
endforeach()
# Retired auth, session, SSE, help, registry and mutex helpers stay gone;
# tgRequireAuth admits web requests only and a revoke queues no notice.
foreach(test_name IN ITEMS dead_auth_helpers_removed)
  add_test(
    NAME ${test_name}
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_LIST_DIR}/test_${test_name}.py
            ${SETUP_MQTT_TEST_ARGS})
endforeach()
# `batterylog clear` reports a refused or failed remove instead of "cleared";
# retired camera auto-capture, power auto mode and eiinputsize are neither
# advertised in module descriptions or the command reference nor kept as a
# dead settings field.
foreach(test_name IN ITEMS battery_log_clear retired_feature_text)
  add_test(
    NAME ${test_name}
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_LIST_DIR}/test_${test_name}.py
            ${SETUP_MQTT_TEST_ARGS})
endforeach()
# Code that runs beside the command executor: a validate pass on cmd_exec
# drops only its own broadcasts, and the shared password helper answers in the
# caller's buffer instead of the shared debug buffer.
add_test(
  NAME command_task_isolation
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_command_task_isolation.py
          ${SETUP_MQTT_TEST_ARGS})
# Web login into a full session table: the session that expires first is
# signed out like any other logout, with the ESP32's 32-bit millis arithmetic.
add_test(
  NAME web_session_eviction
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_web_session_eviction.py
          ${SETUP_MQTT_TEST_ARGS})
# MQTT command replies from the cmd_exec completion: a command that stops
# MQTT (closemqtt sent over MQTT) still gets its reply before teardown deletes
# the client's outbox; other replies are queued for the client task.
add_test(
  NAME mqtt_command_reply
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_mqtt_command_reply.py
          ${SETUP_MQTT_TEST_ARGS})
# Hand-built web JSON replies (recordings list, files/create errors, SSE
# notice) parse as strict JSON, and the automation export and icon debug
# header values live until httpd_resp_send.
add_test(
  NAME web_json_replies
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_web_json_replies.py
          ${SETUP_MQTT_TEST_ARGS})
# Video recording: the production AVI finalizer posts VIDEO_SAVED only for a
# complete clip (short index, failed seek or patch, flush error and an earlier
# frame failure post none), the header carries the measured timing and the
# tracked buffer/geometry fields, a failed start removes its file, and VFS
# refuses sdunmount/sdmount/sdformat while an SD-in-use registration is held.
add_test(
  NAME video_completion
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_video_completion.py
          ${SETUP_MQTT_TEST_ARGS})
# MQTT discovery builders skip a document that does not fit the 1 KB buffer
# (400-character base topic) and the external-sensor table copies esp-mqtt's
# length-delimited topic/payload with exactly topic_len/data_len bytes.
add_test(
  NAME mqtt_json_bounds
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_mqtt_json_bounds.py
          ${SETUP_MQTT_TEST_ARGS})
# Self-registration: /register/submit hands the decoded fields to
# submitAccountRequest byte-exact (spaced and quoted passphrases), owner
# refusals do not count toward the login lockout, anonymous `userrequest` is
# still refused by authorizeCommand, and validate-only `userrequest` stops
# before the owner.
add_test(
  NAME register_submit
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_register_submit.py
          ${SETUP_MQTT_TEST_ARGS})
# Edge Impulse: the whole module on a fake interpreter. edge_impulse_jpeg covers
# input geometry from the input tensor, per-detection labels, the one
# camera/file pipeline and the EI=1/HTTP=0 link; edge_impulse_lifecycle covers
# the inference lock and the continuous-task join.
foreach(test_name IN ITEMS edge_impulse_jpeg edge_impulse_lifecycle)
  add_test(
    NAME ${test_name}
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_LIST_DIR}/test_${test_name}.py
            ${SETUP_MQTT_TEST_ARGS})
endforeach()
# Factory reset: the production confirm callback on the shared fake VFS removes
# users.json, pending requests, user settings files and every /stt and /sd/stt
# account folder in that order, keeps everything else, and stops before the
# roster when account data cannot be removed.
add_test(
  NAME factory_reset_wipe
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_factory_reset_wipe.py
          ${SETUP_MQTT_TEST_ARGS})
# Command names: a source scan of every registered CommandEntry row finds no
# name registered twice, and initializeCommandSystem logs a CMDREG line for a
# shadowed row.
add_test(
  NAME command_name_uniqueness
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_command_name_uniqueness.py
          ${SETUP_MQTT_TEST_ARGS})
# Forced password change: the per-user settings merge refuses credentials and
# the flag, userMustChangePassword ignores it for guests and is cached, the web
# gate in tgRequireAuth admits only the change form and logout, and a guest
# cannot be created with a forced change.
add_test(
  NAME password_change_gate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_password_change_gate.py
          ${SETUP_MQTT_TEST_ARGS})
# Web toast fan-out: JSON toast payloads and the session-fenced broadcast /
# SSE dequeue raced from two threads.
add_test(
  NAME web_toast_fanout
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_web_toast_fanout.py
          ${SETUP_MQTT_TEST_ARGS})
# LLM load policy: web and OLED load/unload go through the admin llmload /
# llmunload commands; the backend applies llmMaxContext for every caller.
add_test(
  NAME llm_load_policy
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_llm_load_policy.py
          ${SETUP_MQTT_TEST_ARGS})
# Account authority in System_User: the reserved bond name is privileged only
# as the live ESP-NOW bond, unban/sessionrevoke/targeted logout follow the rank
# rule, account bans work without HTTP, list replies fit or fail explicitly,
# and the roster snapshot keeps full usernames for the display pages.
add_test(
  NAME user_roster_authority
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_user_roster_authority.py
          ${SETUP_MQTT_TEST_ARGS})
# IP ban list: checked saves that keep the previous file and the RAM ban, a
# locked httpd lookup, whole-or-error JSON listing. Migration restore: all
# files staged before any is replaced; backups stay within the restore limit.
# Camera web: one frame per request, no stream loop on the httpd task.
foreach(test_name IN ITEMS ip_ban_store migration_restore camera_web_frame_only)
  add_test(
    NAME ${test_name}
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_LIST_DIR}/test_${test_name}.py
            ${SETUP_MQTT_TEST_ARGS})
endforeach()
# Capture-key loader: only a missing namespace or blob mints; other NVS errors
# fail closed. Tokenizer loader: malformed LLM1 tokenizer blobs fail the load.
# Setting readers: every power/camera setting has a consumer.
foreach(test_name IN ITEMS capture_crypto_key llm_tokenizer_loader setting_readers)
  add_test(
    NAME ${test_name}
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_LIST_DIR}/test_${test_name}.py
            ${SETUP_MQTT_TEST_ARGS})
endforeach()
# Credential attempt table in System_User: tiers, per-account success, locks
# that outlive the window and are never evicted, one reserved key each for
# Bluetooth and ESP-NOW, and checkCredentialAttempt; web, serial, UART, MQTT,
# targeted, BLE, display and ESP-NOW logins consult it before validating.
add_test(
  NAME credential_attempts
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_credential_attempts.py
          ${SETUP_MQTT_TEST_ARGS})
# OTA probation supervisor on a fake tick: the provisioning accept is not
# diagnosed as a loop stall however late first-time setup starts.
add_test(
  NAME ota_probation_supervisor
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_ota_probation_supervisor.py
          ${SETUP_MQTT_TEST_ARGS})
# OTA journal policy stays in hw1_ota_protocol; System_OTA's one emergency
# recovery arm journals a failed factory switch.
add_test(
  NAME ota_journal_policy
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_ota_journal_policy.py
          ${SETUP_MQTT_TEST_ARGS})
# Time-anchor registry on the shared fake VFS: checked prune rewrite, retried
# append, no row merged into an interrupted one.
add_test(
  NAME time_anchor_persistence
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_time_anchor_persistence.py
          ${SETUP_MQTT_TEST_ARGS})
# The C6 companion's reboot escalation restarts through rebootDevice().
add_test(
  NAME companion_reboot_contract
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_companion_reboot_contract.py
          ${SETUP_MQTT_TEST_ARGS})
# users.json and pending_users.json writers on the shared fake VFS: one locked
# parsed-document owner, credential-first account creation, whole-file resolve.
add_test(
  NAME user_store_mutations
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_user_store_mutations.py
          ${SETUP_MQTT_TEST_ARGS})
# Wi-Fi connect hunt: one deadline for the whole hunt, NO_SSID attempts end
# after a short grace, and only console-owned requests read the console.
add_test(
  NAME wifi_connect_budget
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_wifi_connect_budget.py
          ${SETUP_MQTT_TEST_ARGS})
# Web file streaming: the FS mutex and the sensor poll pause are never held
# across a chunk send, and every send loop stops at the first failed send.
add_test(
  NAME web_file_streaming
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_web_file_streaming.py
          ${SETUP_MQTT_TEST_ARGS})
# Shared in-memory fake VFS (fake_vfs.h) that new persistence suites build on;
# this self-test covers its knobs, faults, lock seam and forwarders.
add_test(
  NAME fake_vfs
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_fake_vfs.py
          ${SETUP_MQTT_TEST_ARGS})
# The shared bounded HTTP request-body reader and the LLM page's wrapper on it.
add_test(
  NAME web_body_reader
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_web_body_reader.py
          ${SETUP_MQTT_TEST_ARGS})
# Boot-time crash counters: brownouts stay out of the software-fault count the
# OTA crash-loop escape keys on, and a committed escape consumes the counters.
add_test(
  NAME crash_record_boot
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_crash_record_boot.py
          ${SETUP_MQTT_TEST_ARGS})
# filecreate refuses an existing name instead of truncating it.
add_test(
  NAME filecreate
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_filecreate.py
          ${SETUP_MQTT_TEST_ARGS})
add_test(
  NAME raw_allocation_inventory
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/../../../../tools/check_raw_allocations.py
          # Ratchet, not a target: the ps_alloc/AllocPolicy unification retired six
          # raw sites (17 -> 11). Lower it when sites go away; a rise means
          # someone reached past System_MemUtil again. The malloc/calloc split
          # (8/3) is what the tree has carried since 0.99.94; the earlier 6 was
          # recorded before two calloc sites became malloc and never updated.
          # The migration restore's raw malloc went with its staged rewrite (10/7).
          --expect 10 --expect-malloc 7)
