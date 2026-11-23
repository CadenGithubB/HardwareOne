"""The live OLED layout as a landscape-only double for harnesses that splice
shipping screen code (the firmware reads it at run time: HAL_Display.h's
SCREEN_WIDTH, oledPortrait(), oledContentBottom() ...). The harnesses draw the
landscape canvas, so the portrait branches compile but are never taken."""
from test_web_batch_handlers import extract_block

LANDSCAPE_STUBS = r'''
#ifndef SCREEN_WIDTH
#ifdef DISPLAY_WIDTH
#define SCREEN_WIDTH DISPLAY_WIDTH
#else
#define SCREEN_WIDTH 128
#endif
#endif
#define oledPortrait() (false)
#define oledCols() (SCREEN_WIDTH / 6)
#define oledContentBottom() (OLED_CONTENT_START_Y + OLED_CONTENT_HEIGHT - 2)
#define oledKvRow(...) (0)
#define oledPrintWrapped(...) (0)
#define oledHeroCard(...) ((void)0)
#define oledMeter(...) ((void)0)
'''

# The shared list renderers' portrait helpers (OLED_Utils.cpp), in definition order.
PORTRAIT_SCROLL_HELPERS = ('static void oledScrollFitRows(', 'static void oledScrollDrawPortraitBar(',
                           'static void oledScrollRenderPortrait(')


def portrait_scroll_helpers(utils: str, render: bool = True) -> str:
    """The helpers the list renderers call; `render` adds the portrait renderer
    (needed when oledScrollRender itself is spliced)."""
    names = PORTRAIT_SCROLL_HELPERS if render else PORTRAIT_SCROLL_HELPERS[:2]
    return '\n'.join(extract_block(utils, name) for name in names)
