cmake_minimum_required(VERSION 3.16)
project(hardwareone_host_tests C CXX)

find_package(Threads REQUIRED)

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

# Allocation-free grammar and list mutation semantics used by personal
# notification preferences in every UI.
add_executable(notification_kind_list_tests test_notification_kind_list.cpp)
set_target_properties(notification_kind_list_tests PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(notification_kind_list_tests PRIVATE
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
  target_compile_options(memutil_tests PRIVATE
    -fsanitize=address,undefined -g)
  target_link_options(memutil_tests PRIVATE
    -fsanitize=address,undefined)
  target_compile_options(memtracker_core_tests PRIVATE
    -fsanitize=address,undefined -g)
  target_link_options(memtracker_core_tests PRIVATE
    -fsanitize=address,undefined)
  target_compile_options(map_viewport_core_tests PRIVATE
    -fsanitize=address,undefined -g)
  target_link_options(map_viewport_core_tests PRIVATE
    -fsanitize=address,undefined)
  target_compile_options(event_kind_mask_tests PRIVATE
    -fsanitize=address,undefined -g)
  target_link_options(event_kind_mask_tests PRIVATE
    -fsanitize=address,undefined)
  target_compile_options(command_limit_tests PRIVATE
    -fsanitize=address,undefined -g)
  target_link_options(command_limit_tests PRIVATE
    -fsanitize=address,undefined)
  target_compile_options(notification_kind_list_tests PRIVATE
    -fsanitize=address,undefined -g)
  target_link_options(notification_kind_list_tests PRIVATE
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
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

enable_testing()
add_test(NAME llm_utf8_tests COMMAND llm_utf8_tests)
add_test(NAME dictation_policy_tests COMMAND dictation_policy_tests)
add_test(NAME memutil_tests COMMAND memutil_tests)
add_test(NAME memtracker_core_tests COMMAND memtracker_core_tests)
add_test(NAME map_viewport_core_tests COMMAND map_viewport_core_tests)
add_test(NAME event_kind_mask_tests COMMAND event_kind_mask_tests)
add_test(NAME command_limit_tests COMMAND command_limit_tests)
add_test(NAME notification_kind_list_tests COMMAND notification_kind_list_tests)
add_test(NAME event_catalog_tests COMMAND event_catalog_tests)
add_test(NAME event_catalog_json_tests COMMAND event_catalog_json_tests)
add_test(NAME event_catalog_text_tests COMMAND event_catalog_text_tests)
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
  NAME user_settings_transaction_guards
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_user_settings_transaction_guards.py)
add_test(
  NAME notification_integration_guards
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/test_notification_integration_guards.py)
add_test(
  NAME raw_allocation_inventory
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_LIST_DIR}/../../../../tools/check_raw_allocations.py
          # Ratchet, not a target: the ps_alloc/AllocPolicy unification retired six
          # raw sites (17 -> 11, malloc 12 -> 6). Lower it when sites go away;
          # a rise means someone reached past System_MemUtil again.
          --expect 11 --expect-malloc 6)
