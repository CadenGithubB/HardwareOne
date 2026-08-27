#!/usr/bin/env bash
# Per-feature flash-cost sweep: flip one feature family off, rebuild, record .bin size, restore.
# Run from an ISOLATED CLONE of the tree (it rewrites System_BuildConfig.h and the saved
# sdkconfig in place, restoring after each variant). Results: sweep_results.tsv.
# Produced docs/FEATURE_COST_LEDGER_2026-08-23.md §3; baseline reproduced the live image byte-exact.
cd "$(dirname "$0")"
source $HOME/esp/esp-idf/export.sh >/dev/null 2>&1
F=components/hardwareone/System_BuildConfig.h
SDK=build-xiao_s3/sdkconfig
OUT=sweep_results.tsv
cp $F $F.base; cp $SDK $SDK.base
set_flag(){ sed -i '' -E "s/^(#define $1[[:space:]]+)[0-9]+/\1$2/" $F; }
size(){ stat -f %z build-xiao_s3/hardwareone-idf.bin 2>/dev/null || echo FAIL; }
build(){ tools/build_board.sh xiao_s3 build > "sweep_$1.log" 2>&1 && size || echo FAIL; }
echo -e "variant\tbytes" > $OUT
echo -e "BASELINE_full\t$(build baseline)" >> $OUT
run(){ name=$1; shift; cp $F.base $F; cp $SDK.base $SDK; eval "$*"; echo -e "$name\t$(build $name)" >> $OUT; cp $F.base $F; cp $SDK.base $SDK; }
run BT_family_off       'set_flag ENABLE_BLUETOOTH 0; set_flag ENABLE_G2_GLASSES 0; set_flag ENABLE_R1_HEALTH 0; set_flag ENABLE_G2_TESTSUITE 0; sed -i "" "s/^CONFIG_BT_ENABLED=y/CONFIG_BT_ENABLED=n/" $SDK'
run G2_glasses_off      'set_flag ENABLE_G2_GLASSES 0; set_flag ENABLE_G2_TESTSUITE 0'
run G2_testsuite_off    'set_flag ENABLE_G2_TESTSUITE 0'
run R1_health_off       'set_flag ENABLE_R1_HEALTH 0'
run NETWORK_family_off  'set_flag NETWORK_FEATURE_LEVEL 0; set_flag WEB_FEATURE_LEVEL 0; set_flag ENABLE_HTTPS 0; set_flag ENABLE_BONDED_MODE 0'
run WEB_off             'set_flag WEB_FEATURE_LEVEL 0; set_flag ENABLE_HTTPS 0'
run HTTPS_off           'set_flag ENABLE_HTTPS 0'
run BONDED_off          'set_flag ENABLE_BONDED_MODE 0'
run I2C_OLED_INPUT_off  'set_flag I2C_FEATURE_LEVEL 0; set_flag DISPLAY_TYPE 0; set_flag INPUT_DEVICE_TYPE 0'
run OLED_INPUT_off      'set_flag DISPLAY_TYPE 0; set_flag INPUT_DEVICE_TYPE 0'
run SENSE_cam_mic_off   'set_flag XIAO_ESP32S3_SENSE_ENABLED 0'
run AUTOMATION_off      'set_flag ENABLE_AUTOMATION 0'
run LLM_backend_off     'set_flag ENABLE_LLM_BACKEND 0; set_flag ENABLE_LLM_SOURCE_CM5 0'
run UART_hostlink_off   'set_flag ENABLE_UART_HOST_LINK 0; set_flag ENABLE_RASPBERRY_PI_HOST_POWER 0; set_flag ENABLE_RASPBERRY_PI_HOST_FAN 0; set_flag ENABLE_LLM_BACKEND 0; set_flag ENABLE_LLM_SOURCE_CM5 0'
run Os_instead_of_O2    'sed -i "" -e "s/^CONFIG_COMPILER_OPTIMIZATION_PERF=y/# CONFIG_COMPILER_OPTIMIZATION_PERF is not set/" -e "s/^# CONFIG_COMPILER_OPTIMIZATION_SIZE is not set/CONFIG_COMPILER_OPTIMIZATION_SIZE=y/" $SDK'
echo SWEEP_DONE >> $OUT
