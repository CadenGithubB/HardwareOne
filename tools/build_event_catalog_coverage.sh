#!/usr/bin/env bash
# Build-matrix coverage for the shared System Event catalog provider.
#
# This script deliberately mutates System_BuildConfig.h for two temporary
# profiles.  Every row starts from the same byte-exact baseline, and the EXIT
# cleanup replaces the temporary FeatherS3/XIAO build products with ordinary
# fullclean builds before restoring the header's original mtime.
#
# Usage:
#   tools/build_event_catalog_coverage.sh
#   tools/build_event_catalog_coverage.sh --provider-only
#   tools/build_event_catalog_coverage.sh --profile xiao_consumers_off
#
# The default mode is the Phase 2+ contract and requires the JSON adapter.
# Phase 1 uses --provider-only and also proves that no JSON adapter object or
# public JSON adapter symbol leaked into the firmware.

set -euo pipefail

CATCOV_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CATCOV_CONFIG="$CATCOV_ROOT/components/hardwareone/System_BuildConfig.h"
CATCOV_PROVIDER_SOURCE="$CATCOV_ROOT/components/hardwareone/System_EventCatalog.cpp"
CATCOV_JSON_SOURCE="$CATCOV_ROOT/components/hardwareone/System_EventCatalogJson.cpp"
CATCOV_BUILD_BOARD="$CATCOV_ROOT/tools/build_board.sh"
CATCOV_MODE="full"
CATCOV_ONLY_PROFILE=""
CATCOV_TEMP_DIR=""
CATCOV_BACKUP=""
CATCOV_LOCK_DIR=""
CATCOV_ORIGINAL_MODE=""
CATCOV_MATRIX_STARTED=0
CATCOV_CURRENT_PROFILE="preflight"
CATCOV_CLEANUP_SIGNAL_STATUS=0

usage() {
  cat <<'EOF'
usage: tools/build_event_catalog_coverage.sh [--provider-only] [--profile NAME]

  --provider-only  Phase 1 provider coverage; reject JSON adapter artifacts
  --profile NAME   Run one diagnostic row, then both ordinary recovery builds
  -h, --help       Show this help

Profile names:
  feathers3_current
  feathers3_g2_no_automation
  xiao_current
  classic_current
  xiao_consumers_off

The ESP-IDF environment must already be exported (source $IDF_PATH/export.sh).
Without --profile, all seven fullclean/build pairs run serially: five matrix
rows followed by ordinary FeatherS3 and XIAO recovery builds for the two
temporary profiles.  A targeted run is diagnostic evidence, not acceptance of
the complete matrix.
EOF
}

fail() {
  echo "event catalog coverage: $*" >&2
  return 1
}

file_mode() {
  python3 - "$1" <<'PY'
import os
import stat
import sys

print(format(stat.S_IMODE(os.stat(sys.argv[1]).st_mode), "o"))
PY
}

same_mode_and_mtime() {
  python3 - "$1" "$2" <<'PY'
import os
import stat
import sys

left = os.stat(sys.argv[1])
right = os.stat(sys.argv[2])
same = (stat.S_IMODE(left.st_mode) == stat.S_IMODE(right.st_mode)
        and left.st_mtime_ns == right.st_mtime_ns)
raise SystemExit(0 if same else 1)
PY
}

restore_baseline_fresh() {
  cp "$CATCOV_BACKUP" "$CATCOV_CONFIG" || return $?
  chmod "$CATCOV_ORIGINAL_MODE" "$CATCOV_CONFIG" || return $?
  cmp -s "$CATCOV_BACKUP" "$CATCOV_CONFIG" || {
    fail "baseline byte restore failed"
    return 1
  }
  touch "$CATCOV_CONFIG" || return $?
}

restore_original_exact() {
  cp "$CATCOV_BACKUP" "$CATCOV_CONFIG" || return $?
  chmod "$CATCOV_ORIGINAL_MODE" "$CATCOV_CONFIG" || return $?
  touch -r "$CATCOV_BACKUP" "$CATCOV_CONFIG" || return $?
  cmp -s "$CATCOV_BACKUP" "$CATCOV_CONFIG" || {
    fail "final System_BuildConfig.h bytes do not match the initial snapshot"
    return 1
  }
  same_mode_and_mtime "$CATCOV_BACKUP" "$CATCOV_CONFIG" || {
    fail "final System_BuildConfig.h mode/mtime do not match the initial snapshot"
    return 1
  }
}

set_flag() {
  local name="$1"
  local value="$2"
  python3 - "$CATCOV_CONFIG" "$name" "$value" <<'PY'
import re
import sys

path, name, value = sys.argv[1:]
with open(path, "rb") as handle:
    original = handle.read()

name_bytes = name.encode("ascii")
pattern = re.compile(
    rb"(?m)^([ \t]*#[ \t]*define[ \t]+" + re.escape(name_bytes)
    + rb"[ \t]+)([0-9]+)"
)
updated, count = pattern.subn(
    lambda match: match.group(1) + value.encode("ascii"),
    original,
    count=1,
)
if count != 1:
    print("event catalog coverage: literal #define for %s was not found" % name,
          file=sys.stderr)
    raise SystemExit(1)
with open(path, "wb") as handle:
    handle.write(updated)
PY
}

literal_flag_value() {
  local name="$1"
  awk -v wanted="$name" '
    $1 == "#define" && $2 == wanted && $3 ~ /^[0-9]+$/ {
      print $3
      exit
    }
  ' "$CATCOV_CONFIG"
}

assert_literal_flag() {
  local name="$1"
  local expected="$2"
  local actual
  actual="$(literal_flag_value "$name")"
  if [[ "$actual" != "$expected" ]]; then
    fail "${name}=${actual:-missing}, expected literal ${expected}"
    return 1
  fi
  printf '    literal %-27s = %s\n' "$name" "$actual"
}

prepare_profile() {
  local expectation="$1"
  local assignments=()
  local index

  case "$expectation" in
    feather_current|xiao_current|classic_current)
      ;;
    feather_g2_no_automation)
      set_flag ENABLE_AUTOMATION 0 || return $?
      assert_literal_flag ENABLE_AUTOMATION 0 || return $?
      assert_literal_flag ENABLE_BLUETOOTH 1 || return $?
      assert_literal_flag ENABLE_G2_GLASSES 1 || return $?
      ;;
    xiao_consumers_off)
      assignments=(
        NETWORK_FEATURE_LEVEL 0
        WEB_FEATURE_LEVEL 0
        I2C_FEATURE_LEVEL 0
        DISPLAY_TYPE 0
        INPUT_DEVICE_TYPE 0
        ENABLE_BLUETOOTH 0
        ENABLE_G2_GLASSES 0
        ENABLE_R1_HEALTH 0
        ENABLE_AUTOMATION 0
        ENABLE_HTTPS 0
        ENABLE_BONDED_MODE 0
      )
      for ((index = 0; index < ${#assignments[@]}; index += 2)); do
        set_flag "${assignments[index]}" "${assignments[index + 1]}" ||
          return $?
      done
      for ((index = 0; index < ${#assignments[@]}; index += 2)); do
        assert_literal_flag \
          "${assignments[index]}" "${assignments[index + 1]}" || return $?
      done
      ;;
    *)
      fail "unknown expectation '$expectation'"
      return 1
      ;;
  esac

  # Make restored or edited content unambiguously newer than the previous
  # dependency graph.  Every row still fullcleans; this timestamp is an
  # additional guard against a restored older source revision.
  touch "$CATCOV_CONFIG"
}

manifest_flag_value() {
  local manifest="$1"
  local name="$2"
  awk -v wanted="$name" '
    /^\*\*Enabled \(/ { section = "enabled"; next }
    /^\*\*Disabled \(/ { section = "disabled"; next }
    /^## / { section = "" }
    section != "" && index($0, "`" wanted "`") > 0 {
      print (section == "enabled" ? 1 : 0)
      exit
    }
  ' "$manifest"
}

manifest_level_value() {
  local manifest="$1"
  local name="$2"
  awk -F '|' -v wanted="$name" '
    NF >= 4 {
      key = $2
      value = $3
      gsub(/[ `\t]/, "", key)
      gsub(/[ `\t]/, "", value)
      if (key == wanted) {
        print value
        exit
      }
    }
  ' "$manifest"
}

assert_manifest_flag() {
  local manifest="$1"
  local name="$2"
  local expected="$3"
  local actual
  actual="$(manifest_flag_value "$manifest" "$name")"
  if [[ "$actual" != "$expected" ]]; then
    fail "$(basename "$(dirname "$manifest")") manifest has ${name}=${actual:-missing}, expected ${expected}"
    return 1
  fi
  printf '    resolved %-26s = %s\n' "$name" "$actual"
}

assert_manifest_level() {
  local manifest="$1"
  local name="$2"
  local expected="$3"
  local actual
  actual="$(manifest_level_value "$manifest" "$name")"
  if [[ "$actual" != "$expected" ]]; then
    fail "$(basename "$(dirname "$manifest")") manifest has ${name}=${actual:-missing}, expected ${expected}"
    return 1
  fi
  printf '    resolved %-26s = %s\n' "$name" "$actual"
}

assert_manifest_flags() {
  local manifest="$1"
  shift
  while (( $# > 0 )); do
    assert_manifest_flag "$manifest" "$1" "$2" || return $?
    shift 2
  done
}

assert_manifest_levels() {
  local manifest="$1"
  shift
  while (( $# > 0 )); do
    assert_manifest_level "$manifest" "$1" "$2" || return $?
    shift 2
  done
}

compile_source_count() {
  local compile_commands="$1"
  local source="$2"
  python3 - "$compile_commands" "$source" <<'PY'
import json
import os
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    entries = json.load(handle)
wanted = os.path.realpath(sys.argv[2])
count = 0
for entry in entries:
    source = entry.get("file", "")
    if not os.path.isabs(source):
        source = os.path.join(entry.get("directory", ""), source)
    if os.path.realpath(source) == wanted:
        count += 1
print(count)
PY
}

compiler_for_source() {
  local compile_commands="$1"
  local source="$2"
  python3 - "$compile_commands" "$source" <<'PY'
import json
import os
import shlex
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    entries = json.load(handle)
wanted = os.path.realpath(sys.argv[2])
for entry in entries:
    source = entry.get("file", "")
    if not os.path.isabs(source):
        source = os.path.join(entry.get("directory", ""), source)
    if os.path.realpath(source) != wanted:
        continue
    argv = entry.get("arguments") or shlex.split(entry.get("command", ""))
    for token in argv:
        if token.endswith(("g++", "c++", "clang++")):
            print(token)
            raise SystemExit(0)
raise SystemExit(1)
PY
}

project_name_for_build() {
  python3 - "$1/project_description.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    print(json.load(handle)["project_name"])
PY
}

assert_newer_than() {
  local file="$1"
  local marker="$2"
  [[ -f "$file" ]] || {
    fail "missing regenerated artifact: $file"
    return 1
  }
  [[ "$file" -nt "$marker" ]] || {
    fail "artifact was not regenerated after row start: $file"
    return 1
  }
}

run_build_step_with_config_guard() {
  local label="$1"
  local phase="$2"
  local prepared_config="$3"
  local board="$4"
  local action="$5"
  local status=0

  # A CMake configure and its translation units must observe the same feature
  # header. Another build helper restoring System_BuildConfig.h mid-row can
  # otherwise produce a misleading link failure: CMake excludes a gated TU
  # while later compiles see the feature enabled and retain its callers.
  if "$CATCOV_BUILD_BOARD" "$board" "$action"; then
    status=0
  else
    status=$?
  fi

  if ! cmp -s "$CATCOV_CONFIG" "$prepared_config"; then
    fail "System_BuildConfig.h changed during ${label} ${phase}; another config-mutating build raced the matrix"
    return 1
  fi
  return "$status"
}

require_unit() {
  local compile_commands="$1"
  local object_dir="$2"
  local link_map="$3"
  local marker="$4"
  local stem="$5"
  local source="$CATCOV_ROOT/components/hardwareone/${stem}.cpp"
  local object="$object_dir/${stem}.cpp.obj"
  local count
  count="$(compile_source_count "$compile_commands" "$source")" || return $?
  [[ "$count" == "1" ]] || {
    fail "${stem}.cpp has ${count} compile entries, expected exactly one"
    return 1
  }
  assert_newer_than "$object" "$marker" || return $?
  grep -Fq "(${stem}.cpp.obj)" "$link_map" || {
    fail "${stem}.cpp.obj is missing from the application link map"
    return 1
  }
}

forbid_unit() {
  local compile_commands="$1"
  local object_dir="$2"
  local link_map="$3"
  local stem="$4"
  local source="$CATCOV_ROOT/components/hardwareone/${stem}.cpp"
  local object="$object_dir/${stem}.cpp.obj"
  local count
  count="$(compile_source_count "$compile_commands" "$source")" || return $?
  [[ "$count" == "0" ]] || {
    fail "${stem}.cpp unexpectedly has ${count} compile entries"
    return 1
  }
  [[ ! -e "$object" ]] || {
    fail "stale or unexpected gated object remains: $object"
    return 1
  }
  if grep -Fq "(${stem}.cpp.obj)" "$link_map"; then
    fail "${stem}.cpp.obj unexpectedly appears in the application link map"
    return 1
  fi
}

assert_single_owned_definition() {
  local nm_file="$1"
  local owner="$2"
  local symbol="$3"
  awk -v owner="$owner" -v symbol="$symbol" '
    index($0, symbol) > 0 && $0 ~ / [TtWw] / {
      count++
      if (index($0, owner) == 0) bad_owner = 1
    }
    END { exit !(count == 1 && bad_owner == 0) }
  ' "$nm_file" || {
    fail "expected one definition of '$symbol' owned by $owner"
    return 1
  }
}

assert_single_owned_data() {
  local nm_file="$1"
  local owner="$2"
  local symbol="$3"
  awk -v owner="$owner" -v symbol="$symbol" '
    index($0, symbol) > 0 && $0 ~ / [RrDdBbSs] / {
      count++
      if (owner != "" && index($0, owner) == 0) bad_owner = 1
    }
    END { exit !(count == 1 && bad_owner == 0) }
  ' "$nm_file" || {
    fail "expected one data owner for '$symbol' in $owner"
    return 1
  }
}

assert_elf_definition() {
  local nm_file="$1"
  local symbol="$2"
  awk -v symbol="$symbol" '
    index($0, symbol) > 0 && $0 ~ / [TtWw] / { count++ }
    END { exit !(count == 1) }
  ' "$nm_file" || {
    fail "linked ELF does not contain exactly one '$symbol' definition"
    return 1
  }
}

assert_nm_absent() {
  local nm_file="$1"
  local symbol="$2"
  if grep -Fq "$symbol" "$nm_file"; then
    fail "unexpected symbol '$symbol' found in $(basename "$nm_file")"
    return 1
  fi
}

# Prove that a particular archive member consumes a public operation. Checking
# only the final ELF definition is insufficient here: another command or web
# consumer can keep the same provider function live while an OLED/G2 adapter
# silently regresses to a private table or a different projection.
assert_owned_undefined_reference() {
  local nm_file="$1"
  local owner="$2"
  local symbol="$3"
  awk -v owner="$owner" -v symbol="$symbol" '
    index($0, owner) > 0 && index($0, symbol) > 0 && $0 ~ / U / { count++ }
    END { exit !(count == 1) }
  ' "$nm_file" || {
    fail "expected one undefined reference to '$symbol' from $owner"
    return 1
  }
}

assert_owner_symbol_absent() {
  local nm_file="$1"
  local owner="$2"
  local symbol="$3"
  if awk -v owner="$owner" -v symbol="$symbol" '
      index($0, owner) > 0 && index($0, symbol) > 0 { found = 1 }
      END { exit !found }
    ' "$nm_file"; then
    fail "unexpected symbol '$symbol' referenced or owned by $owner"
    return 1
  fi
}

verify_provider_ownership() {
  local archive_nm="$1"
  local object_nm="$2"
  local elf_nm="$3"
  local expectation="$4"
  local provider_owner="System_EventCatalog.cpp.obj:"
  local public_symbols=(
    'systemEventCatalogFamilyCount('
    'systemEventCatalogKindCount('
    'systemEventCatalogFamilyAt('
    'systemEventCatalogKindAt('
    'systemEventCatalogFamilyKindAt('
    'systemEventCatalogFindKind('
    'systemEventFamilyName('
    'systemEventKindFamily('
    'systemEventKindName('
    'systemEventKindFromName('
  )
  local data_symbols=(
    '(anonymous namespace)::kFamilyLabels'
    '(anonymous namespace)::kKindNames'
    '(anonymous namespace)::kKindFamilies'
    '(anonymous namespace)::kFamilyIndex'
  )
  local symbol

  for symbol in "${public_symbols[@]}"; do
    assert_single_owned_definition \
      "$archive_nm" "$provider_owner" "$symbol" || return $?
  done
  for symbol in "${data_symbols[@]}"; do
    assert_single_owned_data \
      "$archive_nm" "$provider_owner" "$symbol" || return $?
    assert_single_owned_data "$object_nm" "" "$symbol" || return $?
  done

  # The command projection is live firmware evidence that the provider archive
  # member was pulled into the application, not merely compiled.  Linker
  # section GC may legitimately remove public entry points with no callers.
  # The consumers-off profile has no family-name/kind-family compatibility
  # callers, so demanding those wrappers would turn a dead-code optimization
  # into a false failure.
  assert_elf_definition "$elf_nm" 'systemEventCatalogFamilyCount(' || return $?
  assert_elf_definition "$elf_nm" 'systemEventCatalogFamilyAt(' || return $?
  assert_elf_definition "$elf_nm" 'systemEventCatalogFamilyKindAt(' || return $?
  assert_elf_definition "$elf_nm" 'systemEventKindName(' || return $?
  assert_elf_definition "$elf_nm" 'systemEventKindFromName(' || return $?
  if [[ "$expectation" != "xiao_consumers_off" ]]; then
    assert_elf_definition "$elf_nm" 'systemEventFamilyName(' || return $?
    assert_elf_definition "$elf_nm" 'systemEventKindFamily(' || return $?
  fi

  # These were the pre-extraction copies in System_Events.cpp.  Finding any of
  # them means a consumer still owns a private catalog table.
  assert_nm_absent "$archive_nm" 'kEventKindNames' || return $?
  assert_nm_absent "$archive_nm" 'kEventKindFamily' || return $?
  assert_nm_absent "$archive_nm" 'kEventFamilyNames' || return $?
}

verify_json_mode() {
  local compile_commands="$1"
  local object_dir="$2"
  local link_map="$3"
  local marker="$4"
  local archive_nm="$5"
  local elf_nm="$6"
  local json_object="$object_dir/System_EventCatalogJson.cpp.obj"
  local count
  count="$(compile_source_count "$compile_commands" "$CATCOV_JSON_SOURCE")" ||
    return $?

  if [[ "$CATCOV_MODE" == "provider-only" ]]; then
    [[ "$count" == "0" ]] || {
      fail "provider-only build unexpectedly compiled System_EventCatalogJson.cpp"
      return 1
    }
    [[ ! -e "$json_object" ]] || {
      fail "provider-only build left an unexpected JSON adapter object"
      return 1
    }
    if grep -Fq '(System_EventCatalogJson.cpp.obj)' "$link_map"; then
      fail "provider-only link map contains the JSON adapter"
      return 1
    fi
    assert_nm_absent "$archive_nm" 'systemEventCatalogJsonSize(' || return $?
    assert_nm_absent "$archive_nm" 'systemEventCatalogWriteJson(' || return $?
    assert_nm_absent "$archive_nm" 'systemEventCatalogJsonToBuffer(' || return $?
    assert_nm_absent "$elf_nm" 'systemEventCatalogJsonSize(' || return $?
    assert_nm_absent "$elf_nm" 'systemEventCatalogWriteJson(' || return $?
    assert_nm_absent "$elf_nm" 'systemEventCatalogJsonToBuffer(' || return $?
    return 0
  fi

  [[ "$count" == "1" ]] || {
    fail "System_EventCatalogJson.cpp has ${count} compile entries, expected one"
    return 1
  }
  assert_newer_than "$json_object" "$marker" || return $?
  grep -Fq '(System_EventCatalogJson.cpp.obj)' "$link_map" || {
    fail "JSON adapter object is missing from the application link map"
    return 1
  }
  assert_single_owned_definition \
    "$archive_nm" 'System_EventCatalogJson.cpp.obj:' \
    'systemEventCatalogJsonSize(' || return $?
  assert_single_owned_definition \
    "$archive_nm" 'System_EventCatalogJson.cpp.obj:' \
    'systemEventCatalogWriteJson(' || return $?
  assert_single_owned_definition \
    "$archive_nm" 'System_EventCatalogJson.cpp.obj:' \
    'systemEventCatalogJsonToBuffer(' || return $?
  assert_elf_definition "$elf_nm" 'systemEventCatalogJsonToBuffer(' || return $?
}

verify_oled_indexed_consumer() {
  local archive_nm="$1"
  local owner="$2"
  local symbol
  local indexed_symbols=(
    'systemEventCatalogFamilyCount('
    'systemEventCatalogFamilyAt('
    'systemEventCatalogFamilyKindAt('
  )

  for symbol in "${indexed_symbols[@]}"; do
    assert_owned_undefined_reference \
      "$archive_nm" "$owner" "$symbol" || return $?
  done
  assert_owner_symbol_absent \
    "$archive_nm" "$owner" 'systemEventCatalogJson' || return $?
}

verify_phase3_consumer_references() {
  local expectation="$1"
  local archive_nm="$2"
  local oled_automations_owner='OLED_Mode_Automations.cpp.obj:'
  local oled_utils_owner='OLED_Utils.cpp.obj:'
  local g2_history_owner='G2_Glasses.cpp.obj:'
  local g2_automations_owner='G2_Page_Automations.cpp.obj:'
  local g2_protocol_owner='System_G2_Protocol.cpp.obj:'

  case "$expectation" in
    feather_current|classic_current)
      verify_oled_indexed_consumer \
        "$archive_nm" "$oled_automations_owner" || return $?
      verify_oled_indexed_consumer \
        "$archive_nm" "$oled_utils_owner" || return $?
      assert_owned_undefined_reference \
        "$archive_nm" "$g2_history_owner" \
        'systemEventKindName(' || return $?
      assert_owner_symbol_absent \
        "$archive_nm" "$g2_history_owner" \
        'systemEventCatalogJson' || return $?
      assert_owner_symbol_absent \
        "$archive_nm" "$g2_automations_owner" \
        'systemEventCatalogJson' || return $?
      assert_owner_symbol_absent \
        "$archive_nm" "$g2_protocol_owner" \
        'systemEventCatalogJson' || return $?
      ;;
    feather_g2_no_automation)
      verify_oled_indexed_consumer \
        "$archive_nm" "$oled_utils_owner" || return $?
      assert_owned_undefined_reference \
        "$archive_nm" "$g2_history_owner" \
        'systemEventKindName(' || return $?
      assert_owner_symbol_absent \
        "$archive_nm" "$g2_history_owner" \
        'systemEventCatalogJson' || return $?
      assert_owner_symbol_absent \
        "$archive_nm" "$g2_protocol_owner" \
        'systemEventCatalogJson' || return $?
      ;;
    xiao_current)
      verify_oled_indexed_consumer \
        "$archive_nm" "$oled_automations_owner" || return $?
      verify_oled_indexed_consumer \
        "$archive_nm" "$oled_utils_owner" || return $?
      ;;
    xiao_consumers_off)
      # OLED_Utils.cpp is unconditional because it owns display-off stubs. Its
      # archive member must not retain the display-only catalog picker when the
      # resolved display gate is off.
      assert_owner_symbol_absent \
        "$archive_nm" "$oled_utils_owner" \
        'systemEventCatalogFamilyCount(' || return $?
      assert_owner_symbol_absent \
        "$archive_nm" "$oled_utils_owner" \
        'systemEventCatalogFamilyAt(' || return $?
      assert_owner_symbol_absent \
        "$archive_nm" "$oled_utils_owner" \
        'systemEventCatalogFamilyKindAt(' || return $?
      assert_owner_symbol_absent \
        "$archive_nm" "$oled_utils_owner" \
        'systemEventCatalogJson' || return $?
      ;;
    *)
      fail "unknown Phase 3 consumer expectation '$expectation'"
      return 1
      ;;
  esac
}

verify_profile_gates() {
  local expectation="$1"
  local manifest="$2"
  local compile_commands="$3"
  local object_dir="$4"
  local link_map="$5"
  local marker="$6"

  case "$expectation" in
    feather_current|classic_current)
      assert_manifest_flags "$manifest" \
        ENABLE_HTTP_SERVER 1 ENABLE_OLED_DISPLAY 1 ENABLE_AUTOMATION 1 \
        ENABLE_BLUETOOTH 1 ENABLE_G2_GLASSES 1 || return $?
      require_unit "$compile_commands" "$object_dir" "$link_map" "$marker" \
        WebServer_Server || return $?
      require_unit "$compile_commands" "$object_dir" "$link_map" "$marker" \
        OLED_Mode_Automations || return $?
      require_unit "$compile_commands" "$object_dir" "$link_map" "$marker" \
        System_Automation || return $?
      require_unit "$compile_commands" "$object_dir" "$link_map" "$marker" \
        System_G2_Protocol || return $?
      require_unit "$compile_commands" "$object_dir" "$link_map" "$marker" \
        G2_Glasses || return $?
      require_unit "$compile_commands" "$object_dir" "$link_map" "$marker" \
        OLED_Utils || return $?
      require_unit "$compile_commands" "$object_dir" "$link_map" "$marker" \
        G2_Page_Automations || return $?
      ;;
    feather_g2_no_automation)
      assert_manifest_flags "$manifest" \
        ENABLE_HTTP_SERVER 1 ENABLE_OLED_DISPLAY 1 ENABLE_AUTOMATION 0 \
        ENABLE_BLUETOOTH 1 ENABLE_G2_GLASSES 1 || return $?
      require_unit "$compile_commands" "$object_dir" "$link_map" "$marker" \
        System_G2_Protocol || return $?
      require_unit "$compile_commands" "$object_dir" "$link_map" "$marker" \
        G2_Glasses || return $?
      require_unit "$compile_commands" "$object_dir" "$link_map" "$marker" \
        OLED_Utils || return $?
      forbid_unit "$compile_commands" "$object_dir" "$link_map" \
        OLED_Mode_Automations || return $?
      forbid_unit "$compile_commands" "$object_dir" "$link_map" \
        System_Automation || return $?
      forbid_unit "$compile_commands" "$object_dir" "$link_map" \
        G2_Page_Automations || return $?
      ;;
    xiao_current)
      assert_manifest_flags "$manifest" \
        ENABLE_HTTP_SERVER 1 ENABLE_OLED_DISPLAY 1 ENABLE_AUTOMATION 1 \
        ENABLE_BLUETOOTH 0 ENABLE_G2_GLASSES 0 || return $?
      require_unit "$compile_commands" "$object_dir" "$link_map" "$marker" \
        WebServer_Server || return $?
      require_unit "$compile_commands" "$object_dir" "$link_map" "$marker" \
        OLED_Mode_Automations || return $?
      require_unit "$compile_commands" "$object_dir" "$link_map" "$marker" \
        OLED_Utils || return $?
      require_unit "$compile_commands" "$object_dir" "$link_map" "$marker" \
        System_Automation || return $?
      forbid_unit "$compile_commands" "$object_dir" "$link_map" \
        System_G2_Protocol || return $?
      forbid_unit "$compile_commands" "$object_dir" "$link_map" \
        G2_Page_Automations || return $?
      ;;
    xiao_consumers_off)
      assert_manifest_flags "$manifest" \
        ENABLE_WIFI 0 ENABLE_HTTP_SERVER 0 ENABLE_OLED_DISPLAY 0 \
        ENABLE_AUTOMATION 0 ENABLE_BLUETOOTH 0 ENABLE_G2_GLASSES 0 \
        ENABLE_R1_HEALTH 0 ENABLE_HTTPS 0 ENABLE_BONDED_MODE 0 || return $?
      assert_manifest_levels "$manifest" \
        NETWORK_FEATURE_LEVEL 0 WEB_FEATURE_LEVEL 0 I2C_FEATURE_LEVEL 0 \
        DISPLAY_TYPE 0 INPUT_DEVICE_TYPE 0 || return $?
      forbid_unit "$compile_commands" "$object_dir" "$link_map" \
        WebServer_Server || return $?
      forbid_unit "$compile_commands" "$object_dir" "$link_map" \
        OLED_Mode_Automations || return $?
      forbid_unit "$compile_commands" "$object_dir" "$link_map" \
        System_Automation || return $?
      forbid_unit "$compile_commands" "$object_dir" "$link_map" \
        System_G2_Protocol || return $?
      forbid_unit "$compile_commands" "$object_dir" "$link_map" \
        G2_Page_Automations || return $?
      ;;
    *)
      fail "unknown profile gate expectation '$expectation'"
      return 1
      ;;
  esac
}

verify_profile() {
  local label="$1"
  local board="$2"
  local expectation="$3"
  local marker="$4"
  local build_dir="$CATCOV_ROOT/build-$board"
  local object_dir="$build_dir/esp-idf/hardwareone/CMakeFiles/__idf_hardwareone.dir"
  local compile_commands="$build_dir/compile_commands.json"
  local archive="$build_dir/esp-idf/hardwareone/libhardwareone.a"
  local manifest="$build_dir/BUILD_INFO.md"
  local project_name
  local app_elf
  local app_bin
  local link_map
  local provider_object="$object_dir/System_EventCatalog.cpp.obj"
  local compiler
  local nm_tool
  local archive_nm="$CATCOV_TEMP_DIR/${label}.archive.nm"
  local object_nm="$CATCOV_TEMP_DIR/${label}.provider.nm"
  local elf_nm="$CATCOV_TEMP_DIR/${label}.elf.nm"

  assert_newer_than "$compile_commands" "$marker" || return $?
  assert_newer_than "$build_dir/project_description.json" "$marker" || return $?
  project_name="$(project_name_for_build "$build_dir")" || return $?
  app_elf="$build_dir/${project_name}.elf"
  app_bin="$build_dir/${project_name}.bin"
  link_map="$build_dir/${project_name}.map"

  assert_newer_than "$provider_object" "$marker" || return $?
  assert_newer_than "$archive" "$marker" || return $?
  assert_newer_than "$app_elf" "$marker" || return $?
  assert_newer_than "$app_bin" "$marker" || return $?
  assert_newer_than "$link_map" "$marker" || return $?
  assert_newer_than "$manifest" "$marker" || return $?
  if grep -Fq 'STALE —' "$manifest"; then
    fail "$manifest reports stale feature resolution"
    return 1
  fi

  require_unit "$compile_commands" "$object_dir" "$link_map" "$marker" \
    System_EventCatalog || return $?

  compiler="$(compiler_for_source \
    "$compile_commands" "$CATCOV_PROVIDER_SOURCE")" || {
      fail "could not resolve the provider compiler from compile_commands.json"
      return 1
    }
  case "$compiler" in
    *-g++) nm_tool="${compiler%-g++}-nm" ;;
    *-c++) nm_tool="${compiler%-c++}-nm" ;;
    *)
      fail "cannot derive nm from provider compiler '$compiler'"
      return 1
      ;;
  esac
  [[ -x "$nm_tool" ]] || {
    fail "derived nm tool is not executable: $nm_tool"
    return 1
  }

  "$nm_tool" -A -C "$archive" > "$archive_nm" || return $?
  "$nm_tool" -C "$provider_object" > "$object_nm" || return $?
  "$nm_tool" -C "$app_elf" > "$elf_nm" || return $?
  verify_provider_ownership \
    "$archive_nm" "$object_nm" "$elf_nm" "$expectation" || return $?
  verify_json_mode \
    "$compile_commands" "$object_dir" "$link_map" "$marker" \
    "$archive_nm" "$elf_nm" || return $?
  verify_profile_gates \
    "$expectation" "$manifest" "$compile_commands" "$object_dir" \
    "$link_map" "$marker" || return $?
  verify_phase3_consumer_references \
    "$expectation" "$archive_nm" || return $?

  echo "event catalog coverage: PASS $label ($board)"
}

build_and_verify_row() {
  local label="$1"
  local board="$2"
  local expectation="$3"
  local marker="$CATCOV_TEMP_DIR/${label}.start"
  local prepared_config="$CATCOV_TEMP_DIR/${label}.System_BuildConfig.h"

  CATCOV_CURRENT_PROFILE="$label"
  restore_baseline_fresh || return $?
  prepare_profile "$expectation" || return $?
  cp "$CATCOV_CONFIG" "$prepared_config" || return $?
  touch "$marker" || return $?
  printf '\n=== event catalog row: %s | board=%s | start=%s ===\n' \
    "$label" "$board" "$(date '+%Y-%m-%d %H:%M:%S %Z')"

  run_build_step_with_config_guard \
    "$label" fullclean "$prepared_config" "$board" fullclean || return $?
  run_build_step_with_config_guard \
    "$label" build "$prepared_config" "$board" build || return $?
  verify_profile "$label" "$board" "$expectation" "$marker" || return $?
  cmp -s "$CATCOV_CONFIG" "$prepared_config" ||
    fail "System_BuildConfig.h changed while verifying $label"
}

cleanup_board() {
  local label="$1"
  local board="$2"
  local expectation="$3"
  echo "event catalog coverage: rebuilding ordinary $board artifacts"
  build_and_verify_row "$label" "$board" "$expectation"
}

cleanup_signal() {
  local status="$1"
  [[ "$CATCOV_CLEANUP_SIGNAL_STATUS" -ne 0 ]] ||
    CATCOV_CLEANUP_SIGNAL_STATUS="$status"
  echo "event catalog coverage: cleanup interrupted; continuing source/artifact recovery" >&2
}

# Before the byte-exact source backup is complete, cleanup only owns the
# freshly-created temporary directory and build lock. The source has not been
# touched yet, so attempting a restore from a missing/partial backup would be
# less safe than leaving it alone.
preflight_cleanup() {
  local status="$1"
  trap - EXIT
  if [[ -n "$CATCOV_TEMP_DIR" ]]; then
    rm -rf -- "$CATCOV_TEMP_DIR"
  fi
  if [[ -n "$CATCOV_LOCK_DIR" ]]; then
    rmdir "$CATCOV_LOCK_DIR" 2>/dev/null || true
  fi
  exit "$status"
}

cleanup() {
  local matrix_status="$1"
  local matrix_profile="$CATCOV_CURRENT_PROFILE"
  local cleanup_status=0
  local status=0
  local source_restored=0
  trap - EXIT
  trap 'cleanup_signal 130' INT
  trap 'cleanup_signal 143' TERM
  set +e

  if [[ "$CATCOV_MATRIX_STARTED" -eq 1 ]]; then
    cleanup_board cleanup_feathers3 feathers3 feather_current
    status=$?
    if [[ "$status" -ne 0 ]]; then
      echo "event catalog coverage: ordinary FeatherS3 recovery failed ($status)" >&2
      [[ "$cleanup_status" -ne 0 ]] || cleanup_status=$status
    fi

    cleanup_board cleanup_xiao_s3 xiao_s3 xiao_current
    status=$?
    if [[ "$status" -ne 0 ]]; then
      echo "event catalog coverage: ordinary XIAO recovery failed ($status)" >&2
      [[ "$cleanup_status" -ne 0 ]] || cleanup_status=$status
    fi
  fi

  restore_original_exact
  status=$?
  if [[ "$status" -eq 0 ]]; then
    source_restored=1
    echo "event catalog coverage: System_BuildConfig.h bytes/mode/mtime restored"
  else
    echo "event catalog coverage: source restoration failed; backup retained at $CATCOV_BACKUP" >&2
    [[ "$cleanup_status" -ne 0 ]] || cleanup_status=$status
  fi

  if [[ "$source_restored" -eq 1 && -n "$CATCOV_TEMP_DIR" ]]; then
    rm -rf "$CATCOV_TEMP_DIR"
  fi
  if [[ -n "$CATCOV_LOCK_DIR" ]]; then
    rmdir "$CATCOV_LOCK_DIR" 2>/dev/null || true
  fi

  if [[ "$CATCOV_CLEANUP_SIGNAL_STATUS" -ne 0 && "$cleanup_status" -eq 0 ]]; then
    cleanup_status="$CATCOV_CLEANUP_SIGNAL_STATUS"
  fi

  if [[ "$matrix_status" -ne 0 ]]; then
    echo "event catalog coverage: matrix failed in $matrix_profile ($matrix_status)" >&2
    exit "$matrix_status"
  fi
  if [[ "$cleanup_status" -ne 0 ]]; then
    exit "$cleanup_status"
  fi
  if [[ -n "$CATCOV_ONLY_PROFILE" ]]; then
    echo "event catalog coverage: selected profile and recovery builds passed ($CATCOV_ONLY_PROFILE)"
  else
    echo "event catalog coverage: all matrix and recovery profiles passed"
  fi
  exit 0
}

signal_exit() {
  local status="$1"
  echo "event catalog coverage: interrupted during $CATCOV_CURRENT_PROFILE" >&2
  exit "$status"
}

while (( $# > 0 )); do
  case "$1" in
    --provider-only)
      CATCOV_MODE="provider-only"
      shift
      ;;
    --profile)
      [[ $# -ge 2 ]] || {
        echo 'event catalog coverage: --profile requires a name' >&2
        usage >&2
        exit 2
      }
      [[ -z "$CATCOV_ONLY_PROFILE" ]] || {
        echo 'event catalog coverage: --profile may be specified only once' >&2
        usage >&2
        exit 2
      }
      CATCOV_ONLY_PROFILE="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      usage >&2
      exit 2
      ;;
  esac
done

case "$CATCOV_ONLY_PROFILE" in
  ""|feathers3_current|feathers3_g2_no_automation|xiao_current|classic_current|xiao_consumers_off)
    ;;
  *)
    echo "event catalog coverage: unknown profile '$CATCOV_ONLY_PROFILE'" >&2
    usage >&2
    exit 2
    ;;
esac

[[ -n "${IDF_PATH:-}" && -d "$IDF_PATH" ]] || {
  echo 'event catalog coverage: ESP-IDF is not exported; source $IDF_PATH/export.sh first' >&2
  exit 2
}
command -v idf.py >/dev/null 2>&1 || {
  echo 'event catalog coverage: idf.py is not on PATH; source $IDF_PATH/export.sh first' >&2
  exit 2
}
command -v python3 >/dev/null 2>&1 || {
  echo 'event catalog coverage: python3 is required' >&2
  exit 2
}
command -v cksum >/dev/null 2>&1 || {
  echo 'event catalog coverage: cksum is required for the build lock' >&2
  exit 2
}
[[ -f "$CATCOV_CONFIG" && -w "$CATCOV_CONFIG" ]] || {
  echo "event catalog coverage: missing or unwritable $CATCOV_CONFIG" >&2
  exit 2
}
[[ -f "$CATCOV_PROVIDER_SOURCE" ]] || {
  echo "event catalog coverage: missing $CATCOV_PROVIDER_SOURCE" >&2
  exit 2
}
[[ -x "$CATCOV_BUILD_BOARD" ]] || {
  echo "event catalog coverage: missing executable $CATCOV_BUILD_BOARD" >&2
  exit 2
}
for board in feathers3 xiao_s3 feather_esp32_v2; do
  [[ -f "$CATCOV_ROOT/boards/$board.defaults" ]] || {
    echo "event catalog coverage: missing boards/$board.defaults" >&2
    exit 2
  }
done
if [[ "$CATCOV_MODE" == "full" && ! -f "$CATCOV_JSON_SOURCE" ]]; then
  echo 'event catalog coverage: default mode requires the Phase 2 JSON adapter; use --provider-only during Phase 1' >&2
  exit 2
fi

CATCOV_TEMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/hw1-event-catalog.XXXXXX")"
CATCOV_BACKUP="$CATCOV_TEMP_DIR/System_BuildConfig.h.baseline"
CATCOV_LOCK_KEY="$(printf '%s\n' "$CATCOV_ROOT" | cksum | awk '{print $1}')"
CATCOV_LOCK_DIR="${TMPDIR:-/tmp}/hw1-event-catalog-${CATCOV_LOCK_KEY}.lock"
if ! mkdir "$CATCOV_LOCK_DIR"; then
  rmdir "$CATCOV_TEMP_DIR" 2>/dev/null || true
  echo "event catalog coverage: another matrix may own $CATCOV_LOCK_DIR" >&2
  exit 3
fi

trap 'preflight_cleanup $?' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
cp -p "$CATCOV_CONFIG" "$CATCOV_BACKUP"
CATCOV_ORIGINAL_MODE="$(file_mode "$CATCOV_CONFIG")"
trap 'cleanup $?' EXIT
trap 'signal_exit 130' INT
trap 'signal_exit 143' TERM

cmp -s "$CATCOV_CONFIG" "$CATCOV_BACKUP" ||
  fail "initial metadata-preserving backup is not byte-identical"
same_mode_and_mtime "$CATCOV_CONFIG" "$CATCOV_BACKUP" ||
  fail "initial backup did not preserve mode/mtime"

CATCOV_MATRIX_STARTED=1
case "$CATCOV_ONLY_PROFILE" in
  feathers3_current)
    build_and_verify_row feathers3_current feathers3 feather_current
    ;;
  feathers3_g2_no_automation)
    build_and_verify_row feathers3_g2_no_automation \
      feathers3 feather_g2_no_automation
    ;;
  xiao_current)
    build_and_verify_row xiao_current xiao_s3 xiao_current
    ;;
  classic_current)
    build_and_verify_row classic_current feather_esp32_v2 classic_current
    ;;
  xiao_consumers_off)
    build_and_verify_row xiao_consumers_off xiao_s3 xiao_consumers_off
    ;;
  "")
    build_and_verify_row feathers3_current feathers3 feather_current
    build_and_verify_row feathers3_g2_no_automation \
      feathers3 feather_g2_no_automation
    build_and_verify_row xiao_current xiao_s3 xiao_current
    build_and_verify_row classic_current feather_esp32_v2 classic_current
    build_and_verify_row xiao_consumers_off xiao_s3 xiao_consumers_off
    echo "event catalog coverage: five matrix rows passed; running recovery builds"
    ;;
esac

if [[ -n "$CATCOV_ONLY_PROFILE" ]]; then
  echo "event catalog coverage: selected profile passed; running recovery builds"
fi
exit 0
