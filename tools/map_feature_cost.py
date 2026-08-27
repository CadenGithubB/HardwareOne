#!/usr/bin/env python3
"""Per-FEATURE flash/RAM attribution from a link map.

    python3 tools/map_feature_cost.py build-<board>/hardwareone-idf.map <bin_size_bytes> [out.json]

Groups every placed input section (addr != 0 -- gc-discarded sections are listed at
address 0 and must be skipped) by object file, then maps objects to feature buckets.

Strings: GNU ld lists each post-merge .rodata.str pool under its FIRST contributor
(esp_app_desc.c.obj holds the 1.77 MB flash pool on this link; libbtdm arch_main.o a
minor one) and every other object at its PRE-merge size. The pools fold only ~7% on
this firmware, so per-object pre-merge sizes scaled by (pool / sum_pre_merge) are a
good per-feature estimate -- shown as `str~`. `placed + pool` lands ~18-19 KB short of
the .bin: that residual is section alignment fill (ld's *fill* rows, ~17.6 KB in
.flash.text alone), not an attribution error. If the gap is ever ~1.7 MB, the pool
holder object has changed -- look for a single .str row of that size.

Corrections from the 2026-08-23 adversarial review (do not regress):
  - .xt.prop/.xt.lit/.eh_frame are NOT in the .bin (no ALLOC flag) -> `nonalloc`, never rodata.
  - .wifi*iram/.coexiram input sections are CODE placed in .flash.text by IDF linker fragments.
  - .bss/.noinit are classified by ADDRESS, not name: CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
    moves wifi/lwip/bt .bss.* into .ext_ram.bss, so the name lies (WiFi's "internal" bss is 413 B).
  - libsodium's archive is libespressif__libsodium.a (managed component prefix).
  - G2_Ring.cpp / G2_Health.cpp are the R1 ring's BLE central + vitals app, not G2 lens code.

See docs/FEATURE_COST_LEDGER_2026-08-23.md for the analysis this was written for.
"""
import re,sys,collections,json
path=sys.argv[1]
lines=open(path,errors='replace').read().split('\n')
in_map=False
rows=[]
POOL=[0]
for i,l in enumerate(lines):
    if l.startswith('Linker script and memory map'): in_map=True; continue
    if not in_map: continue
    m=re.match(r'^ (\.[\w.$*+-]+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(.+)$', l)
    if m: sec,addr,size,obj=m.group(1),int(m.group(2),16),int(m.group(3),16),m.group(4).strip()
    else:
        m2=re.match(r'^ (\.[\w.$*+-]+)\s*$', l)
        if not (m2 and i+1<len(lines)): continue
        n=re.match(r'^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(.+)$', lines[i+1])
        if not n: continue
        sec,addr,size,obj=m2.group(1),int(n.group(1),16),int(n.group(2),16),n.group(3).strip()
    if size==0 or addr==0: continue
    if '.obj' not in obj and '.o' not in obj and '.a' not in obj: continue
    mm=re.search(r'\(([^)]+)\)\s*$', obj)
    name=mm.group(1) if mm else obj.split('/')[-1]
    lib=obj.split('(')[0].split('/')[-1] if '(' in obj else obj.split('/')[-1]
    if '.str' in sec and name=='esp_app_desc.c.obj': POOL[0]+=size; continue   # ld lists each merged pool under its first contributor;
    if '.str' in sec and name=='arch_main.o' and size>4000: POOL[0]+=size; continue  # the 3 minor pools' holders (btdm .str1.1 etc.)
    rows.append((lib,name,sec,addr,size))

def bucket(lib,name):
    n=name.lower(); L=lib.lower()
    if L=='libhardwareone.a':
        rules=[
         (('g2_ring','g2_health','r1_','system_r1_protocol'),'R1 ring / health'),
         (('g2_','system_g2_protocol','g2_glasses'),'G2 glasses (lens UI + protocol)'),
         (('bluetooth.cpp','ble_'),'Bluetooth (app layer)'),
         (('oled_',),'OLED display UI'),
         (('webpage_','webserver_','system_web','system_session','system_http'),'Web UI (server + embedded HTML/JS/CSS pages)'),
         (('system_espnow','system_bond','system_mesh','system_esp_now','system_broadcast'),'ESP-NOW / mesh / bond'),
         (('system_llm','system_dictation','system_uartlink','system_cm5','system_dictationpolicy'),'LLM backend / CM5 link / dictation'),
         (('system_camera','system_imagemanager','system_microphone','system_liveaudio','system_audio','system_capturecrypto','system_capture'),'Camera / mic / live audio / capture'),
         (('system_espsr',),'ESP-SR'),
         (('system_edgeimpulse',),'Edge Impulse'),
         (('i2csensor_','system_i2c','system_sensorlogging','system_sensorstubs','system_sensor'),'I2C sensors + gamepad + sensor logging'),
         (('system_maps','system_mapviewport'),'Maps'),
         (('system_automation',),'Automations'),
         (('system_icons',),'Icons (rodata tables)'),
         (('system_ota','hw1_ota'),'OTA / recovery updater'),
         (('system_mqtt','webpage_mqtt'),'MQTT'),
         (('system_wifi','system_network','system_ntp','system_timeanchors','system_time'),'WiFi / network / time (app layer)'),
         (('system_debug','system_memtracker','system_memorymonitor','system_memutil','system_taskutils','system_crashrecord','system_events','system_notifications'),'Debug / diagnostics / notifications'),
         (('system_command','system_cli','system_utils','system_settings','system_user','system_auth','system_firsttimesetup','system_setupwizard','system_featureregistry','system_configload','hardwareone.cpp','system_filesystem','system_vfs','system_mutex','system_bootstate','system_ramflush','system_crypto','system_secret','system_selfdevice','system_rtc','system_clock','system_dst'),'Core (commands, CLI, settings, users, FS, boot)'),
        ]
        for keys,b in rules:
            if any(n.startswith(k) for k in keys): return b
        return 'HardwareOne other: '+name
    if L=='libarduino.a':
        # ~78% of libarduino.a is FEATURE wrappers, not core (2026-08-23 review):
        if n.startswith('ble') or 'hal-bt' in n: return 'Bluetooth (app layer)'
        if n.startswith(('network','sta.','ap.','wifi','ipaddress')): return 'WiFi / network / time (app layer)'
        if n.startswith(('sd.','sd_diskio','spi.')) or 'hal-spi' in n: return 'SD card (FATFS/SDMMC/SDSPI + Arduino SD)'
        if n.startswith('wire') or 'hal-i2c' in n: return 'I2C sensors + gamepad + sensor logging'
        if n.startswith(('fs.','vfs_api','littlefs')): return 'LittleFS'
        return 'Arduino core (genuine: String/Print/HWCDC/UART/GPIO)'
    libs=[
     (('libbt.a','libbtdm_app.a','libbtbb','libble_'),'Bluetooth stack + controller (IDF/Bluedroid)'),
     (('libnet80211','libpp.a','libphy','libcoexist','libesp_wifi','libwpa_supplicant','libesp_netif','liblwip','libesp_phy','libmesh','libsmartconfig','libwapi','libcore.a','libespnow'),'WiFi / LWIP / PHY (IDF)'),
     (('libmbedtls','libmbedcrypto','libmbedx509','libesp-tls','libesp_tls'),'TLS / mbedTLS (HTTPS)'),
     (('libesp_http_server','libesp_http_client','libhttp_parser','libesp_https','libesp_websocket'),'HTTP server/client (IDF)'),
     (('libsodium','libespressif__libsodium'),'libsodium (ESP-NOW / bond / capture crypto)'),

     (('libespressif__esp32-camera','libesp32-camera'),'Camera driver (IDF component)'),
     (('libesp-tflite-micro','libtflite'),'TFLite-micro (Edge Impulse)'),
     (('libesp_littlefs','liblittlefs','libjoltwallet'),'LittleFS'),
     (('libsdmmc','libesp_driver_sdmmc','libesp_driver_sdspi','libfatfs','libesp_vfs_fat','libesp_driver_sd','libesp_driver_spi'),'SD card (FATFS/SDMMC/SDSPI + Arduino SD)'),   # spi_master pulled only by sdspi on this build
     (('libesp_driver_i2s',),'Camera / mic / live audio / capture'),   # PDM mic, pulled by HAL_Audio only
     (('libespressif__esp_jpeg',),'Camera driver (IDF component)'),
     (('libnvs_flash',),'NVS'),
     (('libc.a','libm.a','libnewlib','libgcc','libstdc++','libsupc++','libc_nano'),'libc / libstdc++ / libgcc'),
     (('libfreertos',),'FreeRTOS'),
    ]
    for keys,b in libs:
        if any(L.startswith(k) for k in keys): return b
    if L=='libhardwareone_libs.a':
        if 'lc3' in n or n in ('tables.c.obj','mdct.c.obj','ltpf.c.obj','spec.c.obj','tns.c.obj','sns.c.obj','plc.c.obj','bits.c.obj','energy.c.obj','bwdet.c.obj','attdet.c.obj','lc3.c.obj'): return 'liblc3 (G2 mic audio)'
        if 'adafruit' in n or 'ssd1306' in n or 'gfx' in n: return 'Adafruit display/sensor libs'
        return 'Vendored libs other: '+name
    return 'ESP-IDF platform (drivers, heap, system, rom, etc.)'

agg=collections.defaultdict(collections.Counter)
for lib,name,sec,addr,size in rows:
    b=bucket(lib,name); c=agg[b]
    if '.str' in sec: c['str_pre']+=size
    elif sec.startswith(('.text','.literal','.iram')): c['text']+=size
    elif sec.startswith('.rodata'): c['rodata']+=size
    elif sec.startswith(('.wifi','.coexiram','.phyiram')): c['text']+=size   # placed in .flash.text/.iram by IDF linker fragments
    elif sec.startswith(('.xt','.eh_frame','.gcc_except')): c['nonalloc']+=size  # NOT in the .bin (no ALLOC flag)
    elif sec.startswith(('.data','.dram')): c['data']+=size
    elif sec.startswith(('.bss','.noinit','.ext_ram')):
        # classify by PLACEMENT: CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY moves
        # wifi/lwip/bt .bss.* into .ext_ram.bss, so the NAME lies. PSRAM data is
        # 0x3c000000-0x3dffffff on S3; internal DRAM 0x3f000000+.
        if 0x3c000000 <= addr < 0x3e000000: c['bss_ext']+=size
        else: c['bss_int']+=size
    else: c['other']+=size
FLASH_TOTAL=int(sys.argv[2]) if len(sys.argv)>2 else 0
placed=sum(c['text']+c['rodata']+c['data'] for c in agg.values())
str_pre=sum(c['str_pre'] for c in agg.values())
pool=POOL[0]
print(f"placed(text+rodata+data)={placed:,}  str_pre_merge={str_pre:,}  true merged pool={pool:,} (fold {100*(1-pool/str_pre):.1f}%)  placed+pool={placed+pool:,} vs image {FLASH_TOTAL:,}")
scale=pool/str_pre if str_pre else 0
out=[]
for b,c in agg.items():
    flash=c['text']+c['rodata']+c['data']; s=int(c['str_pre']*scale)
    out.append((flash+s,b,c,s))
out.sort(reverse=True)
print(f"{'TOTAL~':>10} {'code':>9} {'rodata':>9} {'str~':>8} {'data':>6} {'bss_int':>8} {'bss_ext':>8}  feature")
for tot,b,c,s in out:
    print(f"{tot:10,d} {c['text']:9,d} {c['rodata']:9,d} {s:8,d} {c['data']:6,d} {c['bss_int']:8,d} {c['bss_ext']:8,d}  {b}")
json.dump({b:dict(c,str_est=s) for tot,b,c,s in out}, open(sys.argv[3],'w'), indent=1) if len(sys.argv)>3 else None
