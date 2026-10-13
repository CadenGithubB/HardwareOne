#!/usr/bin/env python3
"""Check the real G2 render interface with enabled and disabled feature gates."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[2]
REPO = SOURCE.parents[1]
COMPILER = shutil.which("c++")
SANITIZE = False

# Compile the same callers for every gate combination. Enabled tests stop at an
# object file: these check the public API without replacing its implementation.
RENDER_CONTRACT = r'''
#include "System_TextPager.h"
#include <cassert>
#include <cstring>
#include <type_traits>
using ListPage = bool (*)(const char* const*, size_t, const G2ContainerGeom&);
using TextPage = bool (*)(const char*, const G2ContainerGeom&, void (*)(), G2TapFn);
using PagerRender = bool (*)(TextPager&, char*, size_t, const G2TextPageChrome&,
                            const G2ContainerGeom&, void (*)(), G2TapFn);
using RebuildProbe = bool (*)(const char*, const char*, const G2ContainerGeom&, void (*)());
using MultiTextPage = bool (*)(const G2TextChildSpec*, size_t, void (*)(), G2TapFn);
using TextPageReady = bool (*)(void (*)(), G2TapFn, bool*);
using MixedPage = bool (*)(const char* const*, size_t, const G2ContainerGeom&,
                          const G2TextChildSpec&);
using MixedPage2 = bool (*)(const char* const*, size_t, const G2ContainerGeom&,
                           const G2TextChildSpec&, const G2TextChildSpec&);
static_assert(std::is_same_v<decltype(&g2ShowListPage), ListPage>);
static_assert(std::is_same_v<decltype(&g2ShowTextPage), TextPage>);
static_assert(std::is_same_v<decltype(&g2TextPagerRender), PagerRender>);
static_assert(std::is_same_v<decltype(&g2ShowTextPageRebuildProbe), RebuildProbe>);
static_assert(std::is_same_v<decltype(&g2ShowMultiTextPage), MultiTextPage>);
static_assert(std::is_same_v<decltype(&g2TextPageReady), TextPageReady>);
static_assert(std::is_same_v<decltype(&g2ShowMixedListText), MixedPage>);
static_assert(std::is_same_v<decltype(&g2ShowMixedListText2), MixedPage2>);
static_assert(std::is_same_v<decltype(&g2RelistMixedListText), MixedPage>);
static_assert(std::is_same_v<G2TapFn, void (*)(G2TapKind)>);
static_assert(G2_TAP_PAGE_NEXT == 0 && G2_TAP_PAGE_PREV == 1);
static_assert(std::is_same_v<decltype(G2TextPageChrome::title), const char*>);
static_assert(std::is_same_v<decltype(G2TextPageChrome::navHint), const char*>);
static_assert(std::is_same_v<decltype(G2TextPageChrome::singleHint), const char*>);
static_assert(std::is_same_v<decltype(G2TextPageChrome::separator), const char*>);
static_assert(std::is_same_v<decltype(G2TextPageChrome::emptyMsg), const char*>);

static int callbackCalls = 0;
static void onExit() { ++callbackCalls; }
static void onTap(G2TapKind) { ++callbackCalls; }
void checkRenderCalls() {
    const char* const items[] = {"one", "two"};
    const G2TextChildSpec children[] = {
        {"title", "Header", 1, G2_GEOM_STATUS_BAR, false},
        {"body", "Content", 2, G2_GEOM_BOTTOM_HALF, true},
    };
    const G2TextPageChrome chrome = {"Title", "Next", "Back", "---", "Empty"};
    uint16_t offsets[] = {0, 4};
    const char body[] = "body";
    TextPager pager = {body, offsets, 1, 100, 1, 0, true};
    char pageBuf[] = "unchanged";
    bool pending = true;
    const bool results[] = {
        g2ShowListPage(items, 2),
        g2ShowListPage(items, 2, G2_GEOM_SMALL),
        g2ShowTextPage("text"),
        g2ShowTextPage("text", G2_GEOM_SMALL),
        g2ShowTextPage("text", G2_GEOM_SMALL, onExit),
        g2ShowTextPage("text", G2_GEOM_SMALL, onExit, onTap),
        g2TextPagerRender(pager, pageBuf, sizeof(pageBuf), chrome,
                          G2_GEOM_LARGE, onExit, onTap),
        g2ShowTextPageRebuildProbe("wait", "text"),
        g2ShowTextPageRebuildProbe("wait", "text", G2_GEOM_SMALL),
        g2ShowTextPageRebuildProbe("wait", "text", G2_GEOM_SMALL, onExit),
        g2TextPageReady(onExit, onTap),
        g2TextPageReady(onExit, onTap, &pending),
        g2ShowMultiTextPage(children, 2),
        g2ShowMultiTextPage(children, 2, onExit),
        g2ShowMultiTextPage(children, 2, onExit, onTap),
        g2ShowMixedListText(items, 2, G2_GEOM_LARGE, children[0]),
        g2ShowMixedListText2(items, 2, G2_GEOM_LARGE, children[0], children[1]),
        g2RelistMixedListText(items, 2, G2_GEOM_LARGE, children[0]),
    };
#if !(ENABLE_BLUETOOTH && ENABLE_G2_GLASSES)
    for (bool result : results) assert(!result);
    assert(callbackCalls == 0);
    assert(!pending);
    assert(std::strcmp(pageBuf, "unchanged") == 0);
    assert(pager.body == body && pager.pageOff == offsets);
    assert(pager.maxPages == 1 && pager.pageBudget == 100);
    assert(pager.pageCount == 1 && pager.curPage == 0 && pager.truncated);
    assert(offsets[0] == 0 && offsets[1] == 4);
#endif
}
'''

TYPE_CONTRACT = r'''
#include <type_traits>
static_assert(std::is_aggregate_v<G2ContainerGeom>);
static_assert(std::is_aggregate_v<G2TextChildSpec>);
static_assert(sizeof(G2ContainerGeom) == 4 * sizeof(uint32_t));
static_assert(std::is_same_v<decltype(G2ContainerGeom::x), uint32_t>);
static_assert(std::is_same_v<decltype(G2ContainerGeom::y), uint32_t>);
static_assert(std::is_same_v<decltype(G2ContainerGeom::w), uint32_t>);
static_assert(std::is_same_v<decltype(G2ContainerGeom::h), uint32_t>);
static_assert(std::is_same_v<decltype(G2TextChildSpec::containerName), const char*>);
static_assert(std::is_same_v<decltype(G2TextChildSpec::content), const char*>);
static_assert(std::is_same_v<decltype(G2TextChildSpec::containerId), uint32_t>);
static_assert(std::is_same_v<decltype(G2TextChildSpec::geom), G2ContainerGeom>);
static_assert(std::is_same_v<decltype(G2TextChildSpec::eventCapture), bool>);
constexpr G2TextChildSpec child = {"name", "content", 7, G2_GEOM_LARGE, false};
static_assert(child.containerId == 7 && !child.eventCapture);
static_assert(child.geom.x == 8 && child.geom.y == 8 &&
              child.geom.w == 560 && child.geom.h == 272);
// All existing presets remain available to consumers of either header.
constexpr G2ContainerGeom presets[] = {
    G2_GEOM_FULL, G2_GEOM_LARGE, G2_GEOM_MEDIUM, G2_GEOM_SMALL,
    G2_GEOM_TOP_HALF, G2_GEOM_BOTTOM_HALF, G2_GEOM_LEFT_HALF, G2_GEOM_RIGHT_HALF,
    G2_GEOM_SPLIT_LIST, G2_GEOM_SPLIT_RIGHT, G2_GEOM_SPLIT_RIGHT_TOP,
    G2_GEOM_QUAD_TL, G2_GEOM_QUAD_TR, G2_GEOM_QUAD_BL, G2_GEOM_QUAD_BR,
    G2_GEOM_STATUS_BAR, G2_GEOM_FOOTER, G2_GEOM_BOTTOM_BAR, G2_GEOM_BOTTOM_BAR_L,
    G2_GEOM_BOTTOM_BAR_R, G2_GEOM_RIGHT_COL, G2_GEOM_TALL_NARROW, G2_GEOM_CENTER_DOT,
};
'''


class DisabledG2Header(unittest.TestCase):
    def compile_unit(self, source, *, bluetooth=1, glasses=1, sdk_bluetooth=1,
                     run=False, standalone=False):
        self.assertIsNotNone(COMPILER, "A host C++ compiler is required")
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            (directory / "Arduino.h").write_text("#include <stdint.h>\n#include <stddef.h>\n")
            (directory / "sdkconfig.h").write_text(
                '#define CONFIG_IDF_TARGET "esp32s3"\n'
                '#define CONFIG_IDF_TARGET_ESP32S3 1\n' +
                ('#define CONFIG_BT_ENABLED 1\n#define CONFIG_BT_BLUEDROID_ENABLED 1\n'
                 if sdk_bluetooth else ''))
            profile = directory / "features.h"
            profile.write_text(
                f"#undef ENABLE_BLUETOOTH\n#define ENABLE_BLUETOOTH {bluetooth}\n"
                f"#undef ENABLE_G2_GLASSES\n#define ENABLE_G2_GLASSES {glasses}\n")
            unit = directory / "test.cpp"
            unit.write_text(source)
            output = directory / ("test" if run else "test.o")
            command = [COMPILER, "-std=c++17", "-I", str(SOURCE)]
            if not standalone:
                command += ["-I", str(directory), "-DARDUINO_XIAO_ESP32S3_DEV=1",
                            f'-DHW1_BOARD_CONFIG_HEADER="{REPO / "boards/xiao_s3.features.h"}"',
                            f'-DHW1_FEATURE_CONFIG_HEADER="{profile}"']
            if not run:
                command += ["-c"]
            command += [str(unit), "-o", str(output)]
            if SANITIZE:
                command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
            compiled = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            if run:
                executed = subprocess.run([str(output)], capture_output=True, text=True)
                self.assertEqual(executed.returncode, 0, executed.stderr)

    def check_gates(self, bluetooth, glasses, sdk_bluetooth):
        self.compile_unit('''#include "G2_Glasses.h"
#ifdef SYSTEM_G2_PROTOCOL_H
#error The public rendering API must not import the wire-protocol header
#endif
''' + RENDER_CONTRACT + TYPE_CONTRACT + '''
#include <cassert>
#include <cstring>
#include <type_traits>
static_assert(!(ENABLE_BLUETOOTH && ENABLE_G2_GLASSES), "disabled G2 branch");
int main() {
    checkRenderCalls();
    static_assert(std::is_same_v<decltype(&g2ConnectionStatusSnapshot),
                  bool (*)(G2ConnectionStatus*)>, "disabled snapshot preserves signature");
    G2ConnectionStatus status;
    assert(!status.running && std::strcmp(status.state, "off") == 0);
    assert(!status.left.connected && !status.right.connected);
    assert(status.left.mtu == 0 && status.right.mtu == 0);
    assert(status.left.battery == -1 && status.right.battery == -1);
    assert(status.left.tx == 0 && status.right.tx == 0 && status.left.rx == 0 && status.right.rx == 0);
    status.state = "sentinel"; status.running = true;
    status.left = {true, 123, 45, 67, 89};
    status.right = {true, 987, 65, 43, 21};
    assert(!g2ConnectionStatusSnapshot(nullptr));
    assert(!g2ConnectionStatusSnapshot(&status));
    assert(status.running && std::strcmp(status.state, "sentinel") == 0);
    assert(status.left.connected && status.left.mtu == 123 && status.left.battery == 45 &&
           status.left.tx == 67 && status.left.rx == 89);
    assert(status.right.connected && status.right.mtu == 987 && status.right.battery == 65 &&
           status.right.tx == 43 && status.right.rx == 21);
    static_assert(std::is_same_v<decltype(&formatG2Status),
                  void (*)(const G2ConnectionStatus&, char*, size_t)>,
                  "disabled formatter preserves signature");
    char text[2] = {'!', '!'};
    formatG2Status(status, nullptr, 0);
    formatG2Status(status, nullptr, 2);
    formatG2Status(status, text, 0);
    assert(text[0] == '!' && text[1] == '!');
    formatG2Status(status, text, 1);
    assert(text[0] == '\\0' && text[1] == '!');
    text[0] = '!';
    getG2Status(nullptr, 0);
    getG2Status(nullptr, 2);
    getG2Status(text, 0);
    assert(text[0] == '!' && text[1] == '!');
    getG2Status(text, 1);
    assert(text[0] == '\\0' && text[1] == '!');
    return initG2Client() || isG2Connected() || getG2State() != G2_STATE_IDLE;
}
''', bluetooth=bluetooth, glasses=glasses, sdk_bluetooth=sdk_bluetooth, run=True)

    def test_enabled_rendering_interface(self):
        self.compile_unit('''#include "G2_Glasses.h"
#ifdef SYSTEM_G2_PROTOCOL_H
#error The public rendering API must not import the wire-protocol header
#endif
static_assert(ENABLE_BLUETOOTH && ENABLE_G2_GLASSES, "enabled G2 branch");
''' + RENDER_CONTRACT + TYPE_CONTRACT)

    def test_render_types_without_feature_config_or_arduino(self):
        self.compile_unit('#include "G2_RenderTypes.h"\n' + TYPE_CONTRACT,
                          standalone=True)

    def test_protocol_and_public_header_include_order(self):
        for headers in (("System_G2_Protocol.h", "G2_Glasses.h"),
                        ("G2_Glasses.h", "System_G2_Protocol.h")):
            with self.subTest(headers=headers):
                self.compile_unit("".join(f'#include "{header}"\n' for header in headers) +
                                  RENDER_CONTRACT + TYPE_CONTRACT + '''
// The wire builders and the public renderer still accept the same shared types.
using MixedBuilder = size_t (*)(uint32_t, const char*, const char* const*, size_t,
                               const G2ContainerGeom&, const G2TextChildSpec&,
                               uint32_t, uint8_t*, size_t);
static_assert(std::is_same_v<decltype(&g2BuildCreateMixedListTextPb), MixedBuilder>);
''')

    def test_application_bluetooth_disabled(self):
        self.check_gates(bluetooth=0, glasses=1, sdk_bluetooth=1)

    def test_glasses_disabled_with_bluetooth_present(self):
        self.check_gates(bluetooth=1, glasses=0, sdk_bluetooth=1)

    def test_sdk_bluetooth_disabled(self):
        self.check_gates(bluetooth=1, glasses=1, sdk_bluetooth=0)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--cxx", default=COMPILER)
    parser.add_argument("--sanitize", action="store_true")
    args, remaining = parser.parse_known_args()
    COMPILER = args.cxx
    SANITIZE = args.sanitize
    unittest.main(argv=[__file__, *remaining])
