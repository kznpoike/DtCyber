/*--------------------------------------------------------------------------
**
**  Copyright (c) 2003-2011, Tom Hunter
**                2025       John Huntley
**
**  Name: window_wayland.c
**
**  Description:
**      Simulate CDC 6612 or CC545 console display on a Wayland compositor.
**
**  This program is free software: you can redistribute it and/or modify
**  it under the terms of the GNU General Public License version 3 as
**  published by the Free Software Foundation.
**
**  This program is distributed in the hope that it will be useful,
**  but WITHOUT ANY WARRANTY; without even the implied warranty of
**  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**  GNU General Public License version 3 for more details.
**
**  You should have received a copy of the GNU General Public License
**  version 3 along with this program in file "license-gpl-3.0.txt".
**  If not, see <http://www.gnu.org/licenses/gpl-3.0.txt>.
**
**--------------------------------------------------------------------------
*/

/*
**  -------------
**  Include Files
**  -------------
*/
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <time.h>

#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H

#include "const.h"
#include "types.h"
#include "proto.h"
#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-client-protocol.h"

/*
**  -----------------
**  Private Constants
**  -----------------
*/
#define ListSize         10000
#define FrameTime        100000
#define FramesPerSecond  (1000000 / FrameTime)
#define MaxX             0777
#define MaxY             0777
#define MAXBUFFERS       10
#define MAXFONTS         3
#define MAXGLYPHS        256
#define GAMMA            2.2   /* The normal standard sRGB gamma value */
#define FontNdxSmall     0
#define FontNdxMedium    1
#define FontNdxLarge     2
#define DPI              75.0
#define MaxPline         255

/*
**  -----------------------
**  Private Macro Functions
**  -----------------------
*/

#ifndef WAYDEBUG
#ifdef _DEBUG
#define WAYDEBUG 1
#else
#define WAYDEBUG 0
#endif
#endif

/*
**  -----------------------------------------
**  Private Typedef and Structure Definitions
**  -----------------------------------------
*/
typedef struct dispList
    {
    u16 xPos;                       /* horizontal position */
    u16 yPos;                       /* vertical position */
    u8  fontSize;                   /* size of font */
    u8  ch;                         /* character to be displayed */
    } DispList;

typedef struct pixelARGB            /* the standard ARGB 32 bit pixel structure */
    {                               /* in little endian order */
    unsigned char blue;
    unsigned char green;
    unsigned char red;
    unsigned char alpha;
    } PixelARGB;

typedef struct dtCyberFont          /* our cache structure for an initialized */
    {                               /* font face at a specific point size */
    char *fontFamily;
    double pointSize;
    FT_Pos bsAdvance;
    char *filePath;
    FT_Face face;
    FT_Glyph glyphCache[MAXGLYPHS]; /* a local cache of the first utf-8 code points */
    } DtCyberFont;

typedef struct wlContentBuffer      /* our pixel buffer structure for buffer reuse */
    {
    struct wl_buffer *frameBuffer;
    bool frameBufferAvailable;
    int pixelBufferSize;
    PixelARGB *framePixels; 
    } WlContentBuffer;

struct pointerEvent
    {
    uint32_t eventMask;
    wl_fixed_t surfaceX, surfaceY;
    uint32_t button, ptrState;
    uint32_t time;
    uint32_t serial;
    struct
        {
        bool valid;
        wl_fixed_t value;
        int32_t discrete;
        } axes[2];
    uint32_t axisSource;
    };

enum touch_event_mask
    {
    TOUCH_EVENT_DOWN = 1 << 0,
    TOUCH_EVENT_UP = 1 << 1,
    TOUCH_EVENT_MOTION = 1 << 2,
    TOUCH_EVENT_CANCEL = 1 << 3,
    TOUCH_EVENT_SHAPE = 1 << 4,
    TOUCH_EVENT_ORIENTATION = 1 << 5,
    };

struct touch_point
    {
    bool valid;
    int32_t id;
    uint32_t eventMask;
    wl_fixed_t surfaceX, surfaceY;
    wl_fixed_t major, minor;
    wl_fixed_t orientation;
    };

struct touch_event
    {
    uint32_t eventMask;
    uint32_t time;
    uint32_t serial;
    struct touch_point points[10];
    };

typedef struct wlClientState
    {
    /* Globals */
    struct wl_display *wlDisplay;
    struct wl_registry *wlRegistry;
    struct wl_shm *wlShm;
    struct wl_compositor *wlCompositor;
    struct wl_data_device_manager *wlDataDeviceManager;
    struct wl_seat *wlSeat;
    struct wl_data_device *wlDataDevice;
    struct xdg_wm_base *xdgWmBase;
    struct zxdg_decoration_manager_v1 *zxdgDecorationManagerV1;
    unsigned char gammaTable[256];
    /* Objects */
    struct wl_surface *wlSurface;
    struct xdg_surface *xdgSurface;
    struct xdg_toplevel *xdgToplevel;
    struct zxdg_toplevel_decoration_v1 *zxdgToplevelDecorationV1;
    struct wl_keyboard *wlKeyboard;
    struct wl_pointer *wlPointer;
    struct wl_touch *wlTouch;
    /* State */
    uint32_t lastFrame;
    int width;
    int height;
    int pendingWidth;
    int pendingHeight;
    bool processConfigure;
    uint32_t pageSize;
    int pixelBufferSize;
    PixelARGB *image;
    int imageSize;
    FT_Vector pen;
    bool closed;
    bool syncDone;
    struct pointerEvent pointerEvent;
    struct xkb_state *xkbState;
    struct xkb_context *xkbContext;
    struct xkb_keymap *xkbKeymap;
    struct touch_event touchEvent;
    enum zxdg_toplevel_decoration_v1_mode decorationMode;
    /* Cursor support */
    struct wl_surface *cursorSurface;
    struct wl_cursor_theme *cursorTheme;
    struct wl_cursor *cursor;
    struct wl_cursor_image *cursorImage;
    struct wl_buffer *cursorBuffer;
    /* Keyboard input processing */
    int keyBufMax;
    int keyBufIn;
    int keyBufOut;
    xkb_keysym_t *keyBuf;
    bool pasteActive;
    bool ddOfferedTextPlain;
    /* Font processing */
    DtCyberFont fonts[MAXFONTS];
    DtCyberFont *currFont;
    int currFontNdx;
    FT_Library library;
    /* Frame buffer proxessing */
    WlContentBuffer buffers[MAXBUFFERS];
    int maxBuffers;
    u16 offsetMapY[MaxY+1];
    } WlClientState;

/*
**  ---------------------------
**  Private Function Prototypes
**  ---------------------------
*/

/*--------------------------------------------------------------------------
**  Purpose:        Main windows handline loopthread.
**------------------------------------------------------------------------*/
void *windowThread(void *param);

/*--------------------------------------------------------------------------
**  Purpose:        Surface frame handling functions.
**                  Foreward definition of the frame callback processor.
**------------------------------------------------------------------------*/
const struct wl_callback_listener wlSurfaceFrameListener;

/*
**  ----------------
**  Public Variables
**  ----------------
*/

/*
**  -----------------
**  Private Variables
**  -----------------
*/
static volatile bool   displayActive = FALSE;
static u8              currentFont;
static u8              oldFont = 0;
static i16             currentX;
static i16             currentY;
static DispList        display[ListSize];
static DispList        *curr;
static DispList        *end;
static pthread_t       displayThread;
static u32             listEnd;
static int             width;
static int             height;
static pthread_mutex_t mutexDisplay;
static u8              *lpClipToKeyboard    = NULL;
static u8              *lpClipToKeyboardPtr = NULL;
static u8              clipToKeyboardDelay  = 0;
static int             usageDisplayCount = 0;
static bool            isMeta;
static WlClientState   state;
static int             debugWayland = WAYDEBUG;

/*--------------------------------------------------------------------------
**  Pointer support constants
**------------------------------------------------------------------------*/
/*--------------------------------------------------------------------------
**  An event bit mask definition tracking which pointer events have occurred
**  in this pointer frame.
**------------------------------------------------------------------------*/
enum pointerEventMask
    {
    POINTER_EVENT_ENTER = 1 << 0,
    POINTER_EVENT_LEAVE = 1 << 1,
    POINTER_EVENT_MOTION = 1 << 2,
    POINTER_EVENT_BUTTON = 1 << 3,
    POINTER_EVENT_AXIS = 1 << 4,
    POINTER_EVENT_AXIS_SOURCE = 1 << 5,
    POINTER_EVENT_AXIS_STOP = 1 << 6,
    POINTER_EVENT_AXIS_DISCRETE = 1 << 7,
    };
uint32_t axis_events = POINTER_EVENT_AXIS
    | POINTER_EVENT_AXIS_SOURCE
    | POINTER_EVENT_AXIS_STOP
    | POINTER_EVENT_AXIS_DISCRETE;

/*--------------------------------------------------------------------------
**  Code translation strings for debug printing.
**------------------------------------------------------------------------*/
char *axis_name[2] =
    {
    [WL_POINTER_AXIS_VERTICAL_SCROLL] = "vertical",
    [WL_POINTER_AXIS_HORIZONTAL_SCROLL] = "horizontal",
    };
char *axis_source[4] =
    {
    [WL_POINTER_AXIS_SOURCE_WHEEL] = "wheel",
    [WL_POINTER_AXIS_SOURCE_FINGER] = "finger",
    [WL_POINTER_AXIS_SOURCE_CONTINUOUS] = "continuous",
    [WL_POINTER_AXIS_SOURCE_WHEEL_TILT] = "wheel tilt",
    };

/*--------------------------------------------------------------------------
**
**  Public Functions
**
**------------------------------------------------------------------------*/

/*--------------------------------------------------------------------------
**  Purpose:        Create POSIX thread which will deal with all Wayland
**                  functions.
**
**  Parameters:     Name        Description.
**
**  Returns:        Nothing.
**
**------------------------------------------------------------------------*/
void windowInit(void)
    {
    int            rc;
    pthread_attr_t attr;

    /*
    **  Create display list pool.
    */
    listEnd = 0;

    /*
    **  Create a mutex to synchronise access to display list.
    */
    pthread_mutex_init(&mutexDisplay, NULL);

    /*
    **  Create POSIX thread with default attributes.
    */
    pthread_attr_init(&attr);
    rc            = pthread_create(&displayThread, &attr, windowThread, NULL);
    displayActive = TRUE;
    }

/*--------------------------------------------------------------------------
**  Purpose:        Set font size.
**                  functions.
**
**  Parameters:     Name        Description.
**                  size        font size in points.
**
**  Returns:        Nothing.
**
**------------------------------------------------------------------------*/
void windowSetFont(u8 font)
    {
    currentFont = font;
    }

/*--------------------------------------------------------------------------
**  Purpose:        Set X coordinate.
**
**  Parameters:     Name        Description.
**                  x           horinzontal coordinate (0 - 0777)
**
**  Returns:        Nothing.
**
**------------------------------------------------------------------------*/
void windowSetX(u16 x)
    {
    currentX = x;
    }

/*--------------------------------------------------------------------------
**  Purpose:        Set Y coordinate.
**
**  Parameters:     Name        Description.
**                  y           horinzontal coordinate (0 - 0777)
**
**  Returns:        Nothing.
**
**------------------------------------------------------------------------*/
void windowSetY(u16 y)
    {
    currentY = 0777 - y;
    }

/*--------------------------------------------------------------------------
**  Purpose:        Queue characters.
**
**  Parameters:     Name        Description.
**                  ch          character to be queued.
**
**  Returns:        Nothing.
**
**------------------------------------------------------------------------*/
void windowQueue(u8 ch)
    {
    DispList *elem;

    /*
    **  Protect display list.
    */
    pthread_mutex_lock(&mutexDisplay);

    if (!((listEnd >= ListSize)
        || (currentX == -1)
        || (currentY == -1)))
        {

        if (ch != 0)
            {
            elem           = display + listEnd++;
            elem->ch       = ch;
            elem->fontSize = currentFont;
            elem->xPos     = currentX;
            elem->yPos     = currentY;
            }

        currentX += currentFont;

        }
    /*
    **  Release display list.
    */
    pthread_mutex_unlock(&mutexDisplay);
    }

/*--------------------------------------------------------------------------
**  Purpose:        Terminate console window.
**
**  Parameters:     Name        Description.
**
**  Returns:        Nothing.
**
**------------------------------------------------------------------------*/
void windowTerminate(void)
    {
    if (displayActive)
        {
        displayActive = FALSE;
        pthread_join(displayThread, NULL);
        }
    }

/*
 **--------------------------------------------------------------------------
 **
 **  Private Functions
 **
 **--------------------------------------------------------------------------
 */

/*--------------------------------------------------------------------------
**  Shared memory support code.
**------------------------------------------------------------------------*/
void wayDebug(int level, char *file, int line, char *fmt, ...)
    {
    if ((debugWayland > 0) && (level <= debugWayland))
        {
        char pline[MaxPline] = "\0";
        va_list param;
        va_start(param, fmt);
        vsnprintf(pline, MaxPline, fmt, param);
        va_end(param);
        logDtError(file, line, pline);
        }
    }

/*--------------------------------------------------------------------------
**  Shared memory support code.
**------------------------------------------------------------------------*/

/*--------------------------------------------------------------------------
**  Purpose:        Shared memory support code.
**                  Randomize the supplied candidate file name with a TOD
**                  suffix. Needed to support multiple mappings.
**                  Note: the caller of this function needs to ensure that
**                  the passed template name is at least 7 characters long.
**
**  Parameters:     Name        Description.
**                  buf         Template name to be randomized.
**
**  Returns:        Nothing. The template name string is updated to a
**                  random, time based character string.
**
**------------------------------------------------------------------------*/
static void
randName(char *buf)
    {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long r = ts.tv_nsec;
    for (int i = 0; i < 6; ++i)
        {
        buf[i] = 'A'+(r&15)+(r&16)*2;
        r >>= 5;
        }
    }

/*--------------------------------------------------------------------------
**  Purpose:        Shared memory support code.
**                  Create a shared memory file suitable for memory mapping.
**
**  Parameters:     Name        Description.
**
**  Returns:        The file identifier number on success or -1 on error.
**
**------------------------------------------------------------------------*/
static int
createShmFile(void)
    {
    int retries = 100;
    do
        {
        char name[] = "/wl_shm-XXXXXX";
        randName(name + sizeof(name) - 7);
        --retries;
        int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0)
            {
            shm_unlink(name);
            return fd;
            }
        } while (retries > 0 && errno == EEXIST);
    return -1;
    }

/*--------------------------------------------------------------------------
**  Purpose:        Shared memory support code.
**                  Create a shared memory file suitable for memory
**                  mapping. Set the file size to the requested size so
**                  that a following mmap action has the needed memory
**                  area.
**
**  Parameters:     Name        Description.
**                  size        The desired available shared size in bytes.
**
**  Returns:        The file identifier number on success or -1 on error.
**
**------------------------------------------------------------------------*/
static int
allocateShmFile(size_t size)
    {
    int fd = createShmFile();
    if (fd < 0)
        {
        return -1;
        }
    int ret;
    do
        {
        ret = ftruncate(fd, size);
        } while (ret < 0 && errno == EINTR);
    if (ret < 0)
        {
        close(fd);
        return -1;
        }
    return fd;
    }

/*--------------------------------------------------------------------------
**  Font handling and character display support code.
**------------------------------------------------------------------------*/

/*--------------------------------------------------------------------------
**  Purpose:        Font handling.
**                  Locate a font definiton file path for the requested
**                  font family. The FontConfig auxillary package is used
**                  to search the available, installed fonts on the system.
**
**  Parameters:     Name        Description.
**                  fontFamily  The requested font family name.
**                  pointSize   The requested font point size.
**
**  Returns:        A character string containing the font family
**                  definition file path or NULL on error.
**                  Note: The returned file path string is locally
**                  allocated and it is the responsibility of the caller
**                  to free the memory once finished with the string.
**
**------------------------------------------------------------------------*/
static char *
findFontFile(const char *fontFamily, const double pointSize)
    {
    char *filePath;
    FcConfig *config;

    config = FcInitLoadConfigAndFonts();
    FcPattern* pat = FcNameParse((FcChar8 *)fontFamily);
    if (!pat)
        {
        logDtError(LogErrorLocation, "Unable to parse the font family name.\n");
        return NULL;
        }
    FcPatternAddDouble(pat, FC_PIXEL_SIZE, pointSize);
    FcPatternAddDouble(pat, FC_DPI, DPI);
    FcPatternAddInteger(pat, FC_SPACING, FC_MONO);
    FcResult res;
    FcFontSet* fs = FcFontSetCreate();
    FcDefaultSubstitute(pat);
    FcConfigSubstitute(config, pat, FcMatchPattern);
    FcPattern* pat2 = FcFontMatch(config, pat, &res);
    if (pat2)
        {
        FcFontSetAdd(fs, pat2);
        }
    FcPatternDestroy(pat);
    if (res == FcResultMatch)
        {
        FcObjectSet* os = FcObjectSetBuild (FC_FAMILY, FC_STYLE, FC_LANG, FC_FILE,
            FC_FONTFORMAT, FC_SIZE, FC_PIXEL_SIZE, FC_SPACING, (char *) 0);
        for (int i=0; fs && i < fs->nfont; ++i)
            {
            wayDebug(1, LogErrorLocation, "Processing font number %d of %d\n", i, fs->nfont);
            FcPattern* font = FcPatternFilter(fs->fonts[i], os);
            FcChar8 *file;
            FcResult res1;
            double size;
            int spacing;

            if (FcPatternGetString(font, FC_FAMILY, 0, &file) == FcResultMatch)
                {
                wayDebug(1, LogErrorLocation, "  Font family name: %s\n", (char *)file);
                }
            if (FcPatternGetString(font, FC_STYLE, 0, &file) == FcResultMatch)
                {
                wayDebug(1, LogErrorLocation, "  Font style: %s\n", (char *)file);
                }
            if (FcPatternGetString(font, FC_LANG, 0, &file) == FcResultMatch)
                {
                wayDebug(1, LogErrorLocation, "  Font language code: %s\n", (char *)file);
                }
            if (FcPatternGetString(font, FC_FONTFORMAT, 0, &file) == FcResultMatch)
                {
                wayDebug(1, LogErrorLocation, "  Font format: %s\n", (char *)file);
                }
            if ((res1 = FcPatternGetDouble(font, FC_SIZE, 0, &size)) == FcResultMatch)
                {
                wayDebug(1, LogErrorLocation, "  Font point size: %f\n", size);
                }
            else
                {
                wayDebug(1, LogErrorLocation, "  Attempt to get point size object returned %d\n", res1);
                }
            if ((res1 = FcPatternGetDouble(font, FC_PIXEL_SIZE, 0, &size)) == FcResultMatch)
                {
                wayDebug(1, LogErrorLocation, "  Font pixel size: %f\n", size);
                }
            else
                {
                wayDebug(1, LogErrorLocation, "  Attempt to get pixel size object returned %d\n", res1);
                }
            if ((res1 = FcPatternGetInteger(font, FC_SPACING, 0, &spacing)) == FcResultMatch)
                {
                wayDebug(1, LogErrorLocation, "  Font spacing: %d\n", spacing);
                }
            else
                {
                wayDebug(1, LogErrorLocation, "  Attempt to get spacing object returned %d\n", res1);
                }
            if (FcPatternGetString(font, FC_FILE, 0, &file) == FcResultMatch)
                {
                /*------------------------------------------------------------------
                ** When we destroy the font set below we loose the file path memory
                ** so we need to allocate a new structure and copy here
                **------------------------------------------------------------------*/
                char *tmp = calloc(sizeof(char), strlen((char *)file)+1);
                filePath = strcpy(tmp, (char *)file);
                wayDebug(1, LogErrorLocation, "  Font file location: %s\n", filePath);
                }
            FcPatternDestroy(font);
            }
        FcObjectSetDestroy(os);
        }
    else
        {
        logDtError(LogErrorLocation, "  Attempt to locate font file returned %d\n", res);
        filePath = NULL;
        }
    FcPatternDestroy(pat2);
    FcFontSetDestroy(fs);
    FcConfigDestroy(config);
    FcFini();
    return filePath;
    }

/*
 **--------------------------------------------------------------------------
 **  Circular buffer processing for keyboard input handling.
 **  Note that the buffer and associated state are maintained in the 
 **  client state object.
 **
 **  For a circular buffer the conditions are:
 **    Buffer empty when In == Out
 **    Buffer full when IN + 1 == Out
 **    In and Out pointers are only incremented modulo Max
 **
 **  In this implementation we do not use the N'th buffer slot.
 **  That allows the conditions above to be met for a simple 
 **  management scheme at a small overhead cost.
 **--------------------------------------------------------------------------
 */

void
allocateKeyBuff(WlClientState *state, int size)
    {
    if (size < 1 || size > 256)
        {
        /*
        ** Sanity check for a reasonable buffer size
        **/
        return;
        }
    state->keyBuf = calloc(sizeof(xkb_keysym_t), size);
    state->keyBufMax = size;
    state->keyBufIn = 0;
    state->keyBufOut = 0;
    }

void
releaseKeyBuff(WlClientState *state)
    {
    if (state->keyBuf == NULL) return;
    free((void *)state->keyBuf);
    state->keyBuf = NULL;
    state->keyBufMax = 0;
    state->keyBufIn = 0;
    state->keyBufOut = 0;
    }

/*
 **--------------------------------------------------------------------------
 ** Queue and incoming key press into the buffer. Silently 
 ** drop the keypress if the buffer is full.
 **--------------------------------------------------------------------------
 */

void
queueKey(WlClientState *state, xkb_keysym_t keyPress)
    {
    if (state->keyBuf == NULL)
        {
        return;
        }
    if (((state->keyBufIn + 1) % state->keyBufMax) == state->keyBufOut)
        {
        return;
        }
    else
        {
        state->keyBuf[state->keyBufIn] = keyPress;
        state->keyBufIn = (state->keyBufIn + 1) % state->keyBufMax;
        }
    }

/*
 **--------------------------------------------------------------------------
 ** Fetch the next key press from the buffer, return 0 if the buffer
 ** is empty.
 **--------------------------------------------------------------------------
 */

xkb_keysym_t
getQueuedKey(WlClientState *state)
    {
    if (state->keyBuf == NULL)
        {
        return 0;
        }

    xkb_keysym_t tmp;
    if (state->keyBufOut == state->keyBufIn)
        {
        tmp = 0;
        }
    else
        {
        tmp = state->keyBuf[state->keyBufOut];
        state->keyBufOut = (state->keyBufOut + 1) % state->keyBufMax;
        }
    return tmp;
    }

/*
 **--------------------------------------------------------------------------
 ** Return the next key press in the buffer, but do not remove it. Return 0
 ** if the buffer is empty.
 **--------------------------------------------------------------------------
 */

xkb_keysym_t
peekQueuedKey(WlClientState *state)
    {
    if (state->keyBuf == NULL)
        {
        return 0;
        }

    xkb_keysym_t tmp;
    if (state->keyBufOut == state->keyBufIn)
        {
        tmp = 0;
        }
    else
        {
        tmp = state->keyBuf[state->keyBufOut];
        }
    return tmp;
    }

/*
 **--------------------------------------------------------------------------
 ** Check if the key press buffer is empty
 **--------------------------------------------------------------------------
 */

bool
isKeyBufEmpty(WlClientState *state)
    {
    return (state->keyBufIn == state->keyBufOut);
    }

/*
 **--------------------------------------------------------------------------
 ** Check if the key press buffer is full
 **--------------------------------------------------------------------------
 */

bool
isKeyBufFull(WlClientState *state)
    {
    return (((state->keyBufIn + 1) % state->keyBufMax) == state->keyBufOut);
    }

/*--------------------------------------------------------------------------
**  Frame buffer processing routines.
**------------------------------------------------------------------------*/

/*--------------------------------------------------------------------------
**  Purpose:        Frame buffer handling functions.
**                  Search the frame buffer cache to locate the entry index
**                  for the supplied wl_buffer structure.
**
**  Parameters:     Name        Description.
**                  data        Client state data pointer handed back to us.
**                  wlBuffer    The buffer structure being released.
**
**  Returns:        The index number for the matching buffer entry or
**                  -1 if the supplied buffer is not found.
**
**------------------------------------------------------------------------*/
int
getBufferIndex(WlClientState *state, struct wl_buffer *buffer)
{
    for (int n = 0; n < state->maxBuffers; n++)
        {
        if (state->buffers[n].frameBuffer == buffer)
            {
            return n;
            }
        }
    return -1;
}

/*--------------------------------------------------------------------------
**  Purpose:        Frame buffer handling functions.
**                  Search the frame buffer cache to locate any configured
**                  buffer that is available for use. Part of available for
**                  is that the frame buffer has the current active pixel
**                  size. If the frame buffer pixel size is different we
**                  destroy the buffer. This is part of the support for
**                  dynamic resizing.
**
**  Parameters:     Name        Description.
**                  data        Client state data pointer handed back to us.
**
**  Returns:        The index number for the available buffer entry or
**                  -1 if the supplied buffer is not found.
**
**------------------------------------------------------------------------*/
int
findAvailableBuffer(WlClientState *state)
{
    for (int n = 0; n < state->maxBuffers; n++)
        {
        if ((state->buffers[n].frameBuffer != NULL) &&
            (state->buffers[n].framePixels != NULL)  &&
            (state->buffers[n].frameBufferAvailable))
            {
            if (state->buffers[n].pixelBufferSize == state->pixelBufferSize)
                {
                return n;
                }
                /*--------------------------------------------------------------
                **  We have found an available buffer but of the wrong pixel
                **  size. We destroy the buffer and clear the cache index so
                **  that the dynamic creation code and create a new buffer
                **  of the new correct size.
                **------------------------------------------------------------*/
                wl_buffer_destroy(state->buffers[n].frameBuffer);
                state->buffers[n].frameBuffer = NULL;
                munmap(state->buffers[n].framePixels, state->buffers[n].pixelBufferSize);
                state->buffers[n].framePixels = NULL;
                state->buffers[n].pixelBufferSize = 0;
                state->buffers[n].frameBufferAvailable = true;
            }
        }
    return -1;
}

/*--------------------------------------------------------------------------
**  Purpose:        Frame buffer handling functions.
**                  Search the frame buffer cache to locate any configured
**                  buffer that is available for use.
**
**  Parameters:     Name        Description.
**                  data        Client state data pointer handed back to us.
**                  wlBuffer    The buffer structure being released.
**
**  Returns:        The index number for the available buffer entry or
**                  -1 if the supplied buffer is not found.
**
**------------------------------------------------------------------------*/
int
findEmptyBufferSlot(WlClientState *state)
{
    for (int n = 0; n < state->maxBuffers; n++)
        {
        if (state->buffers[n].frameBuffer == NULL &&
            state->buffers[n].framePixels == NULL)
            {
            return n;
            }
        }
    return -1;
}

/*--------------------------------------------------------------------------
**  Purpose:        Frame buffer handling functions.
**                  This event reports that the compositor has completed
**                  using a buffer and it may be reused.
**
**  Parameters:     Name        Description.
**                  data        Client state data pointer handed back to us.
**                              The shared memory buffer hands us 0 for
**                              this, so we have to use a static reference.
**                  wlBuffer    The buffer structure being released.
**
**  Returns:        Nothing, we locate the buffer in our cache array
**                  and flag it as available for reuse. If we are not
**                  able to locate the index we just destroy the buffer.
**
**------------------------------------------------------------------------*/
static void
wlBufferRelease(void *data, struct wl_buffer *wlBuffer)
    {
    WlClientState *state = data;
    int index = getBufferIndex(state, wlBuffer);
    if (index >= 0)
        {
        state->buffers[index].frameBufferAvailable = true;
        }
    else
        {
        wl_buffer_destroy(wlBuffer);
        }
    }

/*--------------------------------------------------------------------------
**  Purpose:        Frame buffer handling functions.
**                  The listener structure that receives all incoming
**                  frame buffer events.
**
**------------------------------------------------------------------------*/
static const struct wl_buffer_listener wlBufferListener = {
    .release = wlBufferRelease,
};

/*--------------------------------------------------------------------------
**  Purpose:        Frame buffer handling functions.
**                  Calculate the pixel buffer byte size properly aligned
**                  to the system page size. This method guarantees that
**                  pizel buffers allocated at increasing offsets in the
**                  shared memory area all remain properly page boundary
**                  aligned.
**
**  Parameters:     Name        Description.
**                  width       The row width of the buffer structure in
**                              pixels.
**                  height      The number of pixel rows to allocate.
**                  state       Client state data pointer.
**
**  Returns:        The size in bytes of the shared memory segement for a
**                  pixel buffer. Note: width and height are explicitly
**                  passed in so that this routine can be used for resizing
**                  calculations before a new shared memory area is
**                  comitted.
**
**------------------------------------------------------------------------*/
int
calculatePixelBufferSize(int width, int height, WlClientState *state)
    {
    int size = (width * height * sizeof(PixelARGB)); size = ((size / state->pageSize) + 1) * state->pageSize;
    return size;
    }

/*--------------------------------------------------------------------------
**  Purpose:        Frame buffer handling functions.
**                  Calculate and populate the Y co-ordinate mapping
**                  table used to translate the 6612 / CC545 console
**                  co-ordinate to the real window size in use.
**
**  Parameters:     Name        Description.
**                  state       Client state data pointer.
**
**  Returns:        Nothing. The offset mapping table in the client state 
**                  object is populated with non-zero values.
**
**------------------------------------------------------------------------*/
void
populateYOffsetMap(WlClientState *state)
    {
    float factor = (state->height * 1.0) / (MaxY * 1.0);
    for(int y = 0; y <= MaxY; y++)
        {
        state->offsetMapY[y] = (u16)(roundf(factor * (y * 1.0)));
        if ((y < 11) || (y >= (MaxY - 10)))
            {
            wayDebug(1, LogErrorLocation, "Populated mapping for line = %d as %d.\n",
            y, state->offsetMapY[y]);
            }
        }
    return;
    }

/*--------------------------------------------------------------------------
**  Purpose:        Frame buffer handling functions.
**                  Create a new cached pixel buffer / frame buffer pair
**                  in the buffer cache slot index we are passed. The buffer
**                  is sized according to the size parameter contained in
**                  the client state object.
**
**  Parameters:     Name        Description.
**                  index       The buffer cache slot index to use.
**                  state       Client state data pointer.
**
**  Returns:        An error flag of -1 if the creation fails.
**
**------------------------------------------------------------------------*/
int
createCachedFrameBuffer(int index, WlClientState *state)
    {
    int height = state->height;
    int width = state->width;
    int stride = width * sizeof(PixelARGB);
    int size = state->pixelBufferSize;

    int fd = allocateShmFile(size);
    if (fd == -1)
        {
        logDtError(LogErrorLocation, "createCachedFrameBuffer allocate shm file creation failed.\n");
        return -1;
        }

    /*--------------------------------------------------------------------------
    **  Now create the shared memory buffer pool
    **------------------------------------------------------------------------*/
    struct wl_shm_pool *pool = wl_shm_create_pool(state->wlShm, fd, size);

    wayDebug(2, LogErrorLocation, "createCachedFrameBuffer Creating a new frame buffer in slot %d "
            "of size %d at offset 0.\n", index, size);
    state->buffers[index].framePixels = mmap(NULL, size, PROT_READ | PROT_WRITE,
         MAP_SHARED, fd, 0);
    if (state->buffers[index].framePixels == MAP_FAILED)
        {
        logDtError(LogErrorLocation, "createCachedFrameBuffer unable to mmap the shm frame buffer,"
                " errno = %d aborting.\n", errno);
        state->buffers[index].framePixels = NULL;
        state->buffers[index].frameBuffer = NULL;
        state->buffers[index].frameBufferAvailable = true;
        return -1;
        }
    state->buffers[index].pixelBufferSize = size;
    wayDebug(2, LogErrorLocation, "createCachedFrameBuffer pixel buffer located at address 0x%x\n",
        state->buffers[index].framePixels);

    wayDebug(2, LogErrorLocation, "createCachedFrameBuffer now create the buffer structure\n");
    state->buffers[index].frameBuffer = wl_shm_pool_create_buffer(pool,
         0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_buffer_add_listener(state->buffers[index].frameBuffer, &wlBufferListener, state);

    wayDebug(2, LogErrorLocation, "createCacheFrameBuffer removing shm pool and closing fd.\n");
    wl_shm_pool_destroy(pool);
    close(fd);

    return 0;
    }

/*--------------------------------------------------------------------------
**  Purpose:        Display global handling functions.
**                  This event reports a fatal error from the Wayland
**                  system.
**
**  Parameters:     Name        Description.
**                  data        Client state data pointer handed back to us.
**                  wlDisplay   The display object reporting the error.
**                  objectId    The identifier of the object in error.
**                  code        The error code reported for the object.
**                  message     A chracter string error message for debugging.
**
**  Returns:        Nothing.
**
**------------------------------------------------------------------------*/
static void
wlDisplayError(void *data, struct wl_display *wlDisplay, void *objectId,
		      uint32_t code, const char *message)
    {
    WlClientState *state = data;

    logDtError(LogErrorLocation, "Display error: %s\n", message);
    }

/*--------------------------------------------------------------------------
**  Purpose:        Display global handling functions.
**                  This event acknowledges deletion of a Wayland object 
**                  with the passed identifier.
**
**  Parameters:     Name        Description.
**                  data        Client state data pointer handed back to us.
**                  wlDisplay   The display object reporting the error.
**                  id          The identifier of the deleted object.
**
**  Returns:        Nothing.
**
**------------------------------------------------------------------------*/
static void
wlDisplayDeleteId(void *data, struct wl_display *wlDisplay, uint32_t id)
    {
    WlClientState *state = data;

    wayDebug(1, LogErrorLocation, "Display delete identifier for id %d\n", id);
    }

/*--------------------------------------------------------------------------
**  Purpose:        Display handling functions.
**                  The listener structure that receives all incoming
**                  display events.
**
**------------------------------------------------------------------------*/
static const struct wl_display_listener wlDisplayListener = {
    .error = wlDisplayError,
    .delete_id = wlDisplayDeleteId,
};

/*--------------------------------------------------------------------------
**  Purpose:        Wayland data device routines for paste.
**                  This event communicates a MIME type available to receive
**                  from the data offer.
**
**  Parameters:     Name         Description.
**                  data         Client state data pointer handed back to us.
**                  offer        The data offering the communicated MIME type.
**                  mimeType     A character string name for the MIME type.
**
**  Returns:        No return value.
**
**------------------------------------------------------------------------*/
static void wlDataOfferHandleOffer(void *data, struct wl_data_offer *offer,
		const char *mime_type)
    {
    WlClientState *state = data;
    if ((strcmp("text/plain", mime_type) == 0) ||
        (strcmp("text/plain;charset=utf-8", mime_type) == 0))
        {
        state->ddOfferedTextPlain = true;
        }
	wayDebug(1, LogErrorLocation, "Clipboard supports MIME type: %s\n", mime_type);
    }

/*--------------------------------------------------------------------------
**  Purpose:        Wayland data device routines for paste.
**                  The listener structure that receives all incoming
**                  Wayland seat events.
**
**------------------------------------------------------------------------*/
static const struct wl_data_offer_listener wlDataOfferListener = {
	.offer = wlDataOfferHandleOffer,
};

/*--------------------------------------------------------------------------
**  Purpose:        Wayland data device routines for paste.
**                  This event communicates that a new data source has
**                  been created for this device to read.
**
**  Parameters:     Name         Description.
**                  data         Client state data pointer handed back to us.
**                  wlDataDevice The data device for which the new source is
**                               available.
**                  offer        The newly available data offer.
**
**  Returns:        No return value. We register a listener for the offer
**                  in order to get more information or receive the data.
**
**------------------------------------------------------------------------*/
static void wlDataDeviceHandleDataOffer(void *data,
		struct wl_data_device *wlDatadevice,
        struct wl_data_offer *offer)
    {
    WlClientState *state = data;
    wayDebug(1, LogErrorLocation, "Received a data offer event.\n");
	wl_data_offer_add_listener(offer, &wlDataOfferListener, state);
    }

/*--------------------------------------------------------------------------
**  Purpose:        Wayland data device routines for paste.
**                  This event communicates that the clipboard data is
**                  available to read and should now be processed.
**
**  Parameters:     Name         Description.
**                  data         Client state data pointer handed back to us.
**                  wlDataDevice The data device for which the new source is
**                               available.
**                  offer        The newly available data offer.
**
**  Returns:        No return value. We read the incoming data from the
**                  clipboard and clean up.
**
**------------------------------------------------------------------------*/
static void wlDataDeviceHandleSelection(void *data,
		struct wl_data_device *wlDataDevice,
        struct wl_data_offer *offer)
{
    WlClientState *state = data;

    if (offer != NULL)
        {
            if (state->pasteActive && state->ddOfferedTextPlain)
            {
            /*--------------------------------------------------------------------------
            **  Set up and receive the data from the clipboard if we have an open
            **  past request on our side. Otherwise just destroy the offer.
            **------------------------------------------------------------------------*/
            int fds[2];
            if(pipe(fds) == -1)
                {
                //  Provide some error handling here.
                };
            wl_data_offer_receive(offer, "text/plain", fds[1]);
            close(fds[1]);

            wl_display_roundtrip(state->wlDisplay);

            while (true)
                {
                char buf[1024];
                ssize_t n = read(fds[0], buf, sizeof(buf));
                if (n <= 0)
                    {
                    break;
                    }
                for (int ndx = 0; ndx < n; ndx++)
                    {
                    xkb_keysym_t keySym = xkb_utf32_to_keysym((uint32_t)buf[ndx]);
                    /*------------------------------------------------------------------
                    **  If we receive a character that does not have a matching keyboard
                    **  symbol we just silently drop it. This should be highly unlikely
                    **  for a MIME type of 'text/plain'.
                    **----------------------------------------------------------------*/
                    if (keySym != XKB_KEY_NoSymbol )
                        {
                        queueKey(state, keySym);
                        }
                    }
                }
            close(fds[0]);
            }
            /*--------------------------------------------------------------------------
            **  Cleanup and release the data offer.
            **------------------------------------------------------------------------*/
            wl_data_offer_destroy(offer);
            wayDebug(1, LogErrorLocation, "Destroyed data offer.\n");
        }
    else
        {
        /*--------------------------------------------------------------------------
        **  The clipboard was cleared and contains no data.
        **------------------------------------------------------------------------*/
	    wayDebug(1, LogErrorLocation, "Clipboard is empty\n");
        }

    /*--------------------------------------------------------------------------
    **  Reset our local state to not accepting a paste event.
    **------------------------------------------------------------------------*/
    state->pasteActive = false;
    state->ddOfferedTextPlain = false;
}

/*--------------------------------------------------------------------------
**  Purpose:        Wayland data device routines for paste.
**                  The listener structure that receives all incoming
**                  Wayland data offer events.
**
**------------------------------------------------------------------------*/
static const struct wl_data_device_listener wlDataDeviceListener = {
	.data_offer = wlDataDeviceHandleDataOffer,
    .selection = wlDataDeviceHandleSelection,
};

/*--------------------------------------------------------------------------
**  Purpose:        This routine does the heavy lifting needed to a 
**                  resize of the visible window. We expect these resize
**                  events to be rare, so use resources here in order to
**                  better optimize the frequent frame drawing operations.
**
**  Parameters:     Name         Description.
**                  state        Client state data pointer handed back to us.
**
**  Returns:        No return value. The data objects are updated through the
**                  client state data object. The set of resize operations
**                  are:
**                  1) Delete all frame drawing buffers currently defined.
**                     Note that the frame processing will automatically
**                     create new buffers of the new size as needed.
**                  2) Delete the Wayland shared memory pool from which the
**                     frame drawing buffers are allocated. This will also
**                     release the shared memory back to the O/S.
**                  3) Calculate the new shared memory size needed for
**                     frame buffers.
**                  4) Create the new shared memory area for frame buffers.
**                  5) Create the new Wayland shared memory pool.
**                  6) Allocate a new image buffer according to the new 
**                     size and initialize it to the desired background.
**                  7) Copy the appropriate part of the existing image
**                     buffer into the new image buffer.
**                  8) Release the old image buffer memory back to the O/S.
**                  9) Update all state data in the client state object as
**                     appropriate.
**
**------------------------------------------------------------------------*/
void
resizeBuffers(WlClientState *state)
    {

    /*----------------------------------------------------------------------
    **  Sanity check that a resize is actually needed.
    **--------------------------------------------------------------------*/
    if ((state->width == state->pendingWidth) &&
        (state->height == state->pendingHeight))
        {
        state->pendingWidth = 0;
        state->pendingHeight = 0;
        wayDebug(1, LogErrorLocation, "Resize request to exactly the same size, ignoring.\n");
        return;
        }

    /*----------------------------------------------------------------------
    **  Delete all currently created and available frame buffers
    **--------------------------------------------------------------------*/
    wayDebug(2, LogErrorLocation, "resizeBuffers about to remove current and available buffers.\n");
    for (int n = 0; n < state->maxBuffers; n++)
        {
        if ((state->buffers[n].frameBuffer != NULL) &&
            (state->buffers[n].framePixels != NULL)  &&
            (state->buffers[n].frameBufferAvailable))
            {
            wayDebug(2, LogErrorLocation, "Destroying the frame buffer for slot %d\n", n);
            wl_buffer_destroy(state->buffers[n].frameBuffer);
            state->buffers[n].frameBuffer = NULL;
            wayDebug(2, LogErrorLocation, "Unmapping the pixel buffer for buffer %d\n", n);
            munmap(state->buffers[n].framePixels, state->pixelBufferSize);
            state->buffers[n].framePixels = NULL;
            state->buffers[n].pixelBufferSize = 0;
            state->buffers[n].frameBufferAvailable = true;
            }
        }

    /*----------------------------------------------------------------------
    **  Calculate the required new pixel buffer size.
    **--------------------------------------------------------------------*/
    wayDebug(2, LogErrorLocation, "resizeBuffers about to create pixel buffer size.\n");
    state->pixelBufferSize = calculatePixelBufferSize(state->pendingWidth,
            state->pendingHeight, state);

    /*----------------------------------------------------------------------
    **  Calculate the required new shared memory size and new image buffer,
    **  and copy over the appropriate parts of the existing image buffer.
    **  Then release the old image buffer.
    **--------------------------------------------------------------------*/
    wayDebug(2, LogErrorLocation, "resizeBuffers about to create new image buffer.\n");
    PixelARGB pix;
    pix.alpha = 255;
    pix.red = 0;
    pix.green = 0;
    pix.blue = 0;

    int imageSize = state->pendingWidth * state->pendingHeight * sizeof(PixelARGB);
    PixelARGB *image = calloc((state->pendingWidth * state->pendingHeight),
            sizeof(PixelARGB));
    if (image == NULL)
    {
        logDtError(LogErrorLocation, "resizeBuffers Unable to allocate new image buffer of %d x %d pixels.\n",
            state->pendingWidth, state->pendingHeight);
        exit(1);
    }
    wayDebug(2, LogErrorLocation, "resizeBuffers new image size is %d bytes.\n", imageSize);

    wayDebug(2, LogErrorLocation, "resizeBuffers about to set background black.\n");
    for (int y = 0; y < state->pendingHeight; ++y)
        {
        for (int x = 0; x < state->pendingWidth; ++x)
            {
            image[(y * state->pendingWidth) + x] = pix;
            }
        }

    wayDebug(2, LogErrorLocation, "resizeBuffers completed setting background, about to copy in old image.\n");
    for (int y = 0; y < MIN(state->height, state->pendingHeight); ++y)
        {
        for (int x = 0; x < MIN(state->width, state->pendingWidth); ++x)
            {
            image[(y * state->pendingWidth) + x] = state->image[(y * state->width) + x];
            }
        }
    wayDebug(2, LogErrorLocation, "resizeBuffers completed copying in old image.\n");

    wayDebug(2, LogErrorLocation, "resizeBuffers freeing the existing image buffer space.\n");
    free((void *)state->image);
    state->image = image;
    state->imageSize = imageSize;
    state->width = state->pendingWidth;
    state->height = state->pendingHeight;
    state->pendingWidth = 0;
    state->pendingHeight = 0;
    wayDebug(2, LogErrorLocation, "resizeBuffers processing complete, returning.\n");
    }

/*--------------------------------------------------------------------------
**  Purpose:        This routine draws a single pixel point into the
**                  image buffer. The point is located at the supplied
**                  pen X and Y positions.
**
**  Parameters:     Name         Description.
**                  state        Client state data pointer handed back to us.
**                  pen          The display pen position for the character.
**
**  Returns:        Nothing. The image buffer is updated with the pixel
**                  drawn in the foreground color.
**
**------------------------------------------------------------------------*/
void
drawPoint(WlClientState *state, FT_Vector pen)
    {
    int x, y;

    PixelARGB pix;
    pix.alpha = 255;
    pix.red = 0;
    pix.green = 0;
    pix.blue = 0;

    x = (pen.x >> 6);
    y = (pen.y >> 6);
    wayDebug(3, LogErrorLocation, "Drawing point at pen position x = %d, y = %d.\n",
        x, y);

    /*--------------------------------------------------------------------------
    **  If the point does not fit on the screen silently ignore it
    **------------------------------------------------------------------------*/
    if ( x < 0      ||
         y < 0      ||
         x >= state->width ||
         y >= state->height )
        {
        wayDebug(3, LogErrorLocation, "Skipping off screen pixel, x = %d, y = %d.\n",
            x, y);
        return;
        }

    pix.green = 255;
    state->image[y * state->width + x] = pix;
    }

/*--------------------------------------------------------------------------
**  Purpose:        This routine draws the supplied character into the
**                  image buffer. The character is located at the supplied
**                  pen X and Y positions, and using the supplied font.
**
**  Parameters:     Name         Description.
**                  state        Client state data pointer handed back to us.
**                  character    The utf-32 character code to be rendered.
**                  pen          The display pen position for the character.
**                  font         The cached font to be used for drawing.
**
**  Returns:        An updated pen position ready for the next character.
**
**------------------------------------------------------------------------*/
FT_Vector
drawCharacter(WlClientState *state, uint32_t character, FT_Vector pen,
    DtCyberFont *font)
    {
    FT_Glyph tmpGlyph = NULL;
    FT_Int  i, j, p, q, x, y;
    FT_Int  x_max;
    FT_Int  y_max;
    FT_Int  error;
    FT_Size fontSize = font->face->size;
    FT_Vector newPen = pen;

    PixelARGB pix;
    pix.alpha = 255;
    pix.red = 0;
    pix.green = 0;
    pix.blue = 0;

    if (character < MAXGLYPHS)
        {
        tmpGlyph = font->glyphCache[character];
        if (tmpGlyph == NULL)
            {
            error = FT_Load_Char(font->face, character, FT_LOAD_RENDER);
            if (error != FT_Err_Ok) return newPen;
            error = FT_Get_Glyph(font->face->glyph, &font->glyphCache[character]);
            if (error != FT_Err_Ok) return newPen;
            tmpGlyph = font->glyphCache[character];
            wayDebug(3, LogErrorLocation, "drawCharacter caching a new glyph for '%x'\n",
                character);
            }
            else
            {
            wayDebug(3, LogErrorLocation, "drawCharacter reusing cached glyph for '%x'\n",
                character);
            }
        }
    else
        {
        error = FT_Load_Char(font->face, character, FT_LOAD_RENDER);
        if (error != FT_Err_Ok) return newPen;
        error = FT_Get_Glyph(font->face->glyph, &tmpGlyph);
        if (error != FT_Err_Ok) return newPen;
        }

    if (tmpGlyph->format != FT_GLYPH_FORMAT_BITMAP)
        {
        wayDebug(1, LogErrorLocation, "Glyph is not bitmap format for character %d.\n", character);
        return newPen;
        }

    /*--------------------------------------------------------------------------
    **  Process printable characters
    **  Now go ahead and process the bitmap glyph we have.
    **------------------------------------------------------------------------*/
    /*--------------------------------------------------------------------------
    **  The Wayland surface buffer is indexed from (0,0) in the upper
    **  left hand corner and (max X, max Y) in the lower right hand corner.
    **  So Y values start at 0 and increment to place text from the
    **  top of the screen descending.
    **
    **  The font glyph bitmap is indexed from (0, 0) in the lower
    **  left hand corner and (max X, max Y) in the upper right hand corner.
    **  So bitmap Y values start at target line and decrement to paint
    **  the character correctly.
    **------------------------------------------------------------------------*/

    FT_BitmapGlyph bmGlyph = (FT_BitmapGlyph)tmpGlyph;
    wayDebug(3, LogErrorLocation, "drawCharacter processing bitmap glyph for keypress '%x'\n",
        character);

    x = (pen.x >> 6) + bmGlyph->left;
    if (bmGlyph->top < 0)
        {
        y = (pen.y >> 6) - bmGlyph->top;
        }
    else
        {
        /*--------------------------------------------------------------------------
        ** Old X11 bitmap fonts appear to have positive bitmap top values here.
        **  We have to position the bitmap correctly in the bounding box.
        **------------------------------------------------------------------------*/
        y = (pen.y >> 6) +
            ((fontSize->metrics.height >> 6) - bmGlyph->top);
        }
    x_max = x + bmGlyph->bitmap.width;
    y_max = y + bmGlyph->bitmap.rows;
    wayDebug(3, LogErrorLocation, "Writing character at pen position x = %d, y = %d.\n",
        x, y);
    wayDebug(3, LogErrorLocation, "Bitmap bearing is: left = %d top = %d.\n",
        bmGlyph->left, bmGlyph->top);

    /*--------------------------------------------------------------------------
    ** for simplicity, we assume that 'bitmap->pixel_mode'
    ** is either 'FT_PIXEL_MODE_GRAY' (i.e., not a bitmap font) or
    ** 'FT_PIXEL_MODE_MONO' (i.e. a bitmap font like old X11).
    ** 
    ** i and j index over the target buffer pixel array
    ** p and q index over the character bitmap array
    **------------------------------------------------------------------------*/

    if (bmGlyph->bitmap.pixel_mode == FT_PIXEL_MODE_GRAY)
        {
        for ( i = x, p = 0; i < x_max; i++, p++ )
            {
            for ( j = y, q = 0; j < y_max; j++, q++ )
                {
                /*--------------------------------------------------------------------------
                **  If the glyph does not fit on the screen silently ignore the out of
                **  size parts.
                **------------------------------------------------------------------------*/
                if ( i < 0      ||
                     j < 0      ||
                     i >= state->width ||
                     j >= state->height )
                    {
                    return newPen;
                    }

                pix.green = state->gammaTable[bmGlyph->bitmap.buffer[q * bmGlyph->bitmap.width + p]];
                state->image[j * state->width + i] = pix;
                }
            }
        }
    else if (bmGlyph->bitmap.pixel_mode == FT_PIXEL_MODE_MONO)
        {
        wayDebug(3, LogErrorLocation, "Monochrome bitmap, rows = %d, width = %d, pitch = %d.\n",
            bmGlyph->bitmap.rows, bmGlyph->bitmap.width, bmGlyph->bitmap.pitch);
        /*
        wayDebug(3, LogErrorLocation, "Bitmap is:\n");
        for (int b = 0; b < bmGlyph->bitmap.rows; b++)
            {
            for (int a = 0; a < bmGlyph->bitmap.pitch; a++)
                {
                wayDebug(3, LogErrorLocation, " %#02x",
                    bmGlyph->bitmap.buffer[(b * bmGlyph->bitmap.pitch) + a]);
                }
            wayDebug(3, LogErrorLocation, "\n");
            }
        */
        uint8_t off;
        int pitchBits = bmGlyph->bitmap.pitch * 8;
        uint8_t mask;
        for ( j = y, p = 0; j < y_max; j++, p++ )
            {
            for ( i = x, q = 0; i < x_max; i++, q++ )
                {
                /*--------------------------------------------------------------------------
                **  If the glyph does not fit on the screen silently ignore the out of
                **  size parts.
                **------------------------------------------------------------------------*/
                if ( i < 0      ||
                     j < 0      ||
                     i >= state->width ||
                     j >= state->height )
                    {
                    wayDebug(3, LogErrorLocation, "Skipping off screen pixel, width = %d, height = %d, i = %d, j = %d.\n",
                        state->width, state->height, i, j);
                    return newPen;
                    }

                wayDebug(3, LogErrorLocation, "Bitmap loop 2 i = %d, j = %d, p = %d, q = %d\n",
                    i, j, p, q);
                off  = ((p * pitchBits) + q) / 8;
                mask = (0x80 >> (q % 8));
                wayDebug(3, LogErrorLocation, "Bitmap loop 3 i = %d, j = %d, p = %d, q = %d\n",
                    i, j, p, q);
                wayDebug(3, LogErrorLocation, "Checking bitmap byte 0x%x at offset %d with mask 0x%x.\n",
                    bmGlyph->bitmap.buffer[off], off, mask);
                wayDebug(3, LogErrorLocation, "Bitmap bit at off = %d, mask = 0x%02x, is %d\n",
                    off, mask, (bmGlyph->bitmap.buffer[off] & mask));
                if (bmGlyph->bitmap.buffer[off] & mask) 
                    {
                    wayDebug(3, LogErrorLocation, "Found 1 bit at p = %d, q = %d, off = %d.\n",
                        p, q, off);
                    pix.green = 255;
                    }
                else
                    {
                    wayDebug(3, LogErrorLocation, "Found 0 bit at p = %d, q = %d, off = %d.\n",
                        p, q, off);
                    pix.green = 0;
                    }
                wayDebug(3, LogErrorLocation, "Setting image pixel at j = %d i = %d to green = %d\n",
                    j, i, pix.green);
                state->image[j * state->width + i] = pix;
                }
            }
        }
    else
        {
        wayDebug(3, LogErrorLocation, "Bitmap pixel mode is 0x%x.\n",
             bmGlyph->bitmap.pixel_mode);
        }

    /*--------------------------------------------------------------------------
    **  Increment pen position for next character, always increment x, wrap
    **  to a new line if we reach the end of the row. Remember that the pen
    **  position is always expressed in 26.6 fractional pixels.
    **  Note: for our extracted glyph structures the advance values are in
    **  16.16 format and so need to be shifted right by 10 bits (16 - 6) to
    **  convert to the 26.6 format of the pen position.
    **------------------------------------------------------------------------*/

    wayDebug(3, LogErrorLocation, "Character pen advance value = %d max = %d.\n",
        bmGlyph->root.advance.x >> 16,
        fontSize->metrics.max_advance >> 6);
    wayDebug(3, LogErrorLocation, "Face metrics x_ppem = %d y_ppem = %d.\n",
        font->face->size->metrics.x_ppem >> 6,
        font->face->size->metrics.y_ppem >> 6);
    newPen.x = pen.x + (bmGlyph->root.advance.x >> 10);
    newPen.y = pen.y;
    if ((newPen.x >> 6) >= state->width)
        {
        newPen.x = 0;
        newPen.y = pen.y + fontSize->metrics.height;
        }
    return newPen;
    }

/*--------------------------------------------------------------------------
**  Purpose:        This routine draws the supplied character into the
**                  image buffer. The character is located at the supplied
**                  pen X and Y positions, and using the supplied font.
**
**  Parameters:     Name         Description.
**                  state        Client state data pointer handed back to us.
**                  buf          The string buffer to be output.
**                  buflen       The number of characters to be output.
**                  pen          The starting display pen position.
**                  font         The cached font to be used for drawing.
**
**  Returns:        Nothing. The client state pen position is updated 
**                  suitable for a following character output.
**
**------------------------------------------------------------------------*/
void
drawString(WlClientState *state, char *buf, int buflen, FT_Vector pen,
    DtCyberFont *font)
    {
    if (buflen <= 0)
        {
        return;
        }
    FT_Vector strPen;
    strPen.x = pen.x;
    strPen.y = pen.y;
    for (int ndx = 0; ndx < buflen; ndx++)
        {
        strPen = drawCharacter(state, (uint32_t)buf[ndx], strPen, font);
        }
    }

/*--------------------------------------------------------------------------
**  Purpose:        This routine draws output characters into a backing
**                  image pixel buffer. The incoming characters are
**                  fetched from the key press circular buffer. Drawing
**                  terminates when the key press buffer is empty. This
**                  is only called after a check that the key press buffer
**                  is not empty.
**
**  Parameters:     Name         Description.
**                  state        Client state data pointer handed back to us.
**
**  Returns:        No return value. The data objects are updated through the
**                  client state data object.
**
**------------------------------------------------------------------------*/
void
drawText(WlClientState *state)
    {
    int width = state->width;
    int height = state->height;
    int stride = width * sizeof(PixelARGB);
    int size = stride * height;
    DtCyberFont *currFont = state->currFont;

    FT_Size fontSize = currFont->face->size;
    FT_Int  targetHeight = height;
    FT_Int  i, j, x, y;
    FT_Int  x_max;
    FT_Int  y_max;
    FT_Int  error;
    FT_Vector     pen;

    /*--------------------------------------------------------------------------
    **  Allocate an initial screen image pixel buffer if we do not have one yet.
    **  Set an opaque, black background for the initial image buffer.
    **------------------------------------------------------------------------*/

    PixelARGB pix;
    pix.alpha = 255;
    pix.red = 0;
    pix.green = 0;
    pix.blue = 0;

    wayDebug(2, LogErrorLocation, "drawText entered with image at 0x%x\n", state->image);
    if (state->image == NULL)
        {
        int imageSize = width * height * sizeof(PixelARGB);
        state->image = calloc((width * height), sizeof(PixelARGB));
        if (state->image == NULL)
            {
            logDtError(LogErrorLocation, "Unable to allocate image buffer of %d x %d pixels.\n",
                width, height);
            return;
            }
        state->imageSize = imageSize;
        wayDebug(2, LogErrorLocation, "drawText created image buffer of %d bytes,\n",
            state->imageSize);

        for (int y = 0; y < height; ++y)
            {
            for (int x = 0; x < width; ++x)
                {
                state->image[y * width + x] = pix;
                }
             }
       }

#if CcCycleTime
        {
        extern double cycleTime;
        char          buf[80];

        sprintf(buf, "Cycle time: %.3f", cycleTime);
        pen.x = (0  <<6);
        pen.y = (state->offsetMapY[10] << 6);
        drawString(state, buf, strlen(buf), pen, &state->fonts[FontNdxSmall]);
        }
#endif

#if CcDebug == 1
        {
        char      buf[160];

        /*
        **  Display P registers of PPUs and CPUs and current trace mask.
        */
        sprintf(buf, "Refresh: %-10d  PP P-reg: %04o %04o %04o %04o %04o %04o %04o %04o %04o %04o   CPU P-reg: %06o",
                refreshCount++,
                ppu[0].regP, ppu[1].regP, ppu[2].regP, ppu[3].regP, ppu[4].regP,
                ppu[5].regP, ppu[6].regP, ppu[7].regP, ppu[8].regP, ppu[9].regP,
                cpus[0].regP);
        if (cpuCount > 1)
            {
            sprintf(buf + strlen(buf), " %06o", cpus[1].regP);
            }

        sprintf(buf + strlen(buf), "   Trace: %c%c%c%c%c%c%c%c%c%c%c%c",
                (traceMask >> 0) & 1 ? '0' : '_',
                (traceMask >> 1) & 1 ? '1' : '_',
                (traceMask >> 2) & 1 ? '2' : '_',
                (traceMask >> 3) & 1 ? '3' : '_',
                (traceMask >> 4) & 1 ? '4' : '_',
                (traceMask >> 5) & 1 ? '5' : '_',
                (traceMask >> 6) & 1 ? '6' : '_',
                (traceMask >> 7) & 1 ? '7' : '_',
                (traceMask >> 8) & 1 ? '8' : '_',
                (traceMask >> 9) & 1 ? '9' : '_',
                (traceMask >> 14) & 1 ? 'C' : '_',
                (traceMask >> 15) & 1 ? 'E' : '_');

        pen.x = (0  <<6);
        pen.y = (state->offsetMapY[10] << 6);
        drawString(state, buf, strlen(buf), pen, &state->fonts[FontNdxSmall]);
        }
#endif

        if (opPaused)
            {
            /*
            **  Display pause message.
            */
            static char opMessage[] = "Emulation paused";
            pen.x = (20  <<6);
            pen.y = (state->offsetMapY[256] << 6);
            drawString(state, opMessage, strlen(opMessage), pen, &state->fonts[FontNdxLarge]);
            }
        else if (consoleIsRemoteActive())
            {
            /*
            **  Display indication that rmeote console is active.
            */
            static char opMessage[] = "Remote console active";
            pen.x = (20  <<6);
            pen.y = (state->offsetMapY[256] << 6);
            drawString(state, opMessage, strlen(opMessage), pen, &state->fonts[FontNdxLarge]);
            }


        if (usageDisplayCount != 0)
            {
            /*
            **  Display usage note when user attempts to close window.
            */
            static char usageMessage1[] = "Please don't just close the window, but instead first cleanly halt the operating system and";
            static char usageMessage2[] = "then use the 'shutdown' command in the operator interface to terminate the emulation.";
            pen.x = (20  <<6);
            pen.y = (state->offsetMapY[256] << 6);
            drawString(state, usageMessage1, strlen(usageMessage1), pen, &state->fonts[FontNdxMedium]);
            pen.x = (20  <<6);
            pen.y = (state->offsetMapY[275] << 6);
            drawString(state, usageMessage2, strlen(usageMessage2), pen, &state->fonts[FontNdxMedium]);
            listEnd            = 0;
            if (usageDisplayCount > 0) usageDisplayCount -= 1;
            return;
            }

        /*--------------------------------------------------------------------------
        **  Process the display list received from the PPU.
        **  Protect display list because we are running in a separate thread.
        **------------------------------------------------------------------------*/
        pthread_mutex_lock(&mutexDisplay);

        curr = display;
        end  = display + listEnd;

        wayDebug(2, LogErrorLocation, "drawText about to process the display list at 0x%x ending 0x%x\n",
            curr, end);
        for (curr = display; curr < end; curr++)
            {
            /*
            **  Setup new font if necessary.
            */
            if (oldFont != curr->fontSize)
                {
                oldFont = curr->fontSize;
                wayDebug(2, LogErrorLocation, "drawText switching to font size %d\n", curr->fontSize);

                switch (oldFont)
                    {
                case FontSmall:
                    /*------------------------------------------------------------------
                    **  Set font size small
                    **----------------------------------------------------------------*/
                    state->currFont = &state->fonts[FontNdxSmall];
                    state->currFontNdx = FontNdxSmall;
                    break;

                case FontMedium:
                    /*------------------------------------------------------------------
                    **  Set font size medium
                    **----------------------------------------------------------------*/
                    state->currFont = &state->fonts[FontNdxMedium];
                    state->currFontNdx = FontNdxMedium;
                    break;

                case FontLarge:
                    /*------------------------------------------------------------------
                    **  Set font size large
                    **----------------------------------------------------------------*/
                    state->currFont = &state->fonts[FontNdxLarge];
                    state->currFontNdx = FontNdxLarge;
                    break;
                    }
                }

            /*
            **  Draw dot or character.
            */
            state->pen.x = curr->xPos << 6;
            state->pen.y = state->offsetMapY[curr->yPos] << 6;
            wayDebug(3, LogErrorLocation, "Drawing font %d at pen.x %d pen.y %d\n",
                curr->fontSize, state->pen.x >> 6, state->pen.y >> 6);
            if (curr->fontSize == FontDot)
                {
                drawPoint(state, state->pen);
                }
            else
                {
                drawCharacter(state, (uint32_t)curr->ch, state->pen, state->currFont);
                }
            }

        listEnd  = 0;
        currentX = -1;
        currentY = -1;

        /*
        **  Release display list.
        */
        pthread_mutex_unlock(&mutexDisplay);
    }

/*--------------------------------------------------------------------------
**  Purpose:        Toplevel desktop surface handling functions.
**                  This event reports hints for the siae and state of a 
**                  toplevel desktop surface.
**
**  Parameters:     Name        Description.
**                  data        Client state data pointer handed back to us.
**                  xdgTopLevel The top level surface being configured.
**                  width       The compositor suggested surface width.
**                  height      The compositor suggested surface height.
**                  states      An array of top level surface states
**                              currently known to the compositor.
**
**  Returns:        Nothing. We ignore or update our toplevel surface 
**                  configuration as we desire in response to receiving
**                  this event.
**
**------------------------------------------------------------------------*/
void
xdgToplevelConfigure(void *data, struct xdg_toplevel *xdgToplevel,
                       int32_t width, int32_t height, struct wl_array *states)
    {
    WlClientState *state = data;

    /*--------------------------------------------------------------------------
    **  If either width or height is 0 the compositor is deferring to us and
    **  we ignore the incoming information.
    **------------------------------------------------------------------------*/
    if (width == 0 || height == 0)
        {
        return;
        }

    /*--------------------------------------------------------------------------
    **  Accept the incoming size suggestion and resize our buffer management
    **  structures if the new size differs from the current size in any
    **  dimension.
    **------------------------------------------------------------------------*/
    if ((state->width != width) || (state->height != height))
        {
        state->pendingWidth = width;
        state->pendingHeight = height;
        }
    }

/*--------------------------------------------------------------------------
**  Purpose:        Toplevel desktop surface handling functions.
**                  This event requests that we close our desktop window.
**
**  Parameters:     Name        Description.
**                  data        Client state data pointer handed back to us.
**                  xdgTopLevel The top level surface being configured.
**
**  Returns:        Nothing. We flag the close request in our client state
**                  structure for possible furthur processing.
**
**------------------------------------------------------------------------*/
void
xdgToplevelClose(void *data, struct xdg_toplevel *xdgToplevel)
    {
    WlClientState *state = data;
    state->closed = true;
    /*
    **  Initiate display of usage note because user attempts to close the window.
    */
    usageDisplayCount = 5 * FramesPerSecond;
    }

 const struct xdg_toplevel_listener xdgToplevelListener = {
    .configure = xdgToplevelConfigure,
    .close = xdgToplevelClose,
};

/*--------------------------------------------------------------------------
**  Purpose:        Toplevel desktop decoration handling functions.
**                  This routine provides a printable string translation
**                  for the available decoration modes.
**
**  Parameters:     Name        Description.
**                  mode        The decoration mode to be translated.
**
**  Returns:        A character string description for the passed in mode.
**
**------------------------------------------------------------------------*/
const char *getModeName(enum zxdg_toplevel_decoration_v1_mode mode) {
	switch (mode)
    {
	case ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE:
		return "client-side decorations";
	case ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE:
		return "server-side decorations";
    default:
	    return "invalid decoration mode number";
	}
}

/*--------------------------------------------------------------------------
**  Purpose:        Toplevel desktop decoration handling functions.
**                  This event requests that we close our desktop window.
**
**  Parameters:     Name        Description.
**                  data        Client state data pointer handed back to us.
**                  decoration  The top level decoration object.
**                  mode        The mode of decoration used.
**
**  Returns:        Nothing. We remember the decoration mode in our client
**                  state structure for possible furthur processing.
**
**------------------------------------------------------------------------*/
void
zxdgDecorationV1HandleConfigure(void *data,
    struct zxdg_toplevel_decoration_v1 *decoration,
    enum zxdg_toplevel_decoration_v1_mode mode)
    {
	WlClientState *state = data;
	state->decorationMode = mode;
	printf("Using %s\n", getModeName(mode));
    }

/*--------------------------------------------------------------------------
**  Purpose:        Toplevel desktop decoration handling functions.
**                  The listener structure that receives all incoming
**                  toplevel desktop decoration events.
**
**------------------------------------------------------------------------*/
const struct zxdg_toplevel_decoration_v1_listener zxdgToplevelDecorationListener = {
	.configure = zxdgDecorationV1HandleConfigure,
};

/*--------------------------------------------------------------------------
**  Purpose:        Surface frame handling functions.
**                  Establish a frame buffer and copy the current image
**                  buffer into it.
**
**  Parameters:     Name        Description.
**                  state       Client state data pointer.
**
**  Returns:        The frame buffer slot index. On an error return -1.
**
**------------------------------------------------------------------------*/
int
populateFrameBuffer(WlClientState *state)
    {
    /*--------------------------------------------------------------------------
    **  Copy the current frame image to a buffer.
    **  Reuse an available buffer from the cache if we have one, otherwise
    **  create a reusable frame buffer if we have an emoty cache slot.
    **  We cycle amongst content buffers based upon proper buffer release calls.
    **------------------------------------------------------------------------*/
    int n = findAvailableBuffer(state);
    if (n >= 0)
        {
        wayDebug(2, LogErrorLocation, "populateFrameBuffer Reusing the buffer in slot %d\n", n);
        memcpy(state->buffers[n].framePixels, state->image, state->imageSize);
        state->buffers[n].frameBufferAvailable = false;
        }
    else
        {
        n = findEmptyBufferSlot(state);
        if (n >= 0)
            {
            wayDebug(2, LogErrorLocation, "populateFrameBuffer creating a new buffer in slot %d\n", n);
            int success = createCachedFrameBuffer(n, state);
            if (success == 0)
                {
                wayDebug(3, LogErrorLocation, "populateFrameBuffer copy the pixel image for %d bytes "
                    "from address 0x%x to address 0x%x\n", state->imageSize,
                    state->image, state->buffers[n].framePixels);
                memcpy(state->buffers[n].framePixels, state->image, state->imageSize);
                wayDebug(3, LogErrorLocation, "populateFrameBuffer memcpy complete.\n");
                state->buffers[n].frameBufferAvailable = false;
                }
            else
                {
                logDtError(LogErrorLocation, "populateFrameBuffer unable to create a new buffer.\n");
                return -1;
                }
            }
        else
            {
            logDtError(LogErrorLocation, "populateFrameBuffer unable to locate an unused buffer slot.\n");
            return -1;
            }
        }
    return n;
    }

/*--------------------------------------------------------------------------
**  Purpose:        Desktop surface handling functions.
**                  This event completes configuration of the desktop surface.
**
**  Parameters:     Name        Description.
**                  data        Client state data pointer handed back to us.
**                  xdgSurface  The top level decoration object.
**                  mode        The mode of decoration used.
**
**  Returns:        Nothing. We remember the decoration mode in our client
**                  state structure for possible furthur processing.
**
**------------------------------------------------------------------------*/
void
xdgSurfaceConfigure(void *data,
        struct xdg_surface *xdgSurface, uint32_t serial)
    {
    WlClientState *state = data;

    xdg_surface_ack_configure(xdgSurface, serial);

    /*--------------------------------------------------------------------------
    **  Reconfigure the surface size and buffers if we need to. Afterwards
    **  restart the frame refresh processing logic.
    **------------------------------------------------------------------------*/
    if ((state->pendingWidth > 0)  &&
        (state->pendingHeight > 0))
        {
        resizeBuffers(state);
        state->processConfigure = false;
        }
    }

/*--------------------------------------------------------------------------
**  Purpose:        Desktop surface handling functions.
**                  The listener structure that receives all incoming
**                  desktop surface events.
**------------------------------------------------------------------------*/
const struct xdg_surface_listener xdgSurfaceListener = {
    .configure = xdgSurfaceConfigure,
};

/*--------------------------------------------------------------------------
**  Purpose:        Window manager keep alive processing.
**                  The window manager issues the Ping event and we 
**                  respond with the Pong request to say we are alive.
**------------------------------------------------------------------------*/
void
xdgWmBasePing(void *data, struct xdg_wm_base *xdgWmBase, uint32_t serial)
{
    xdg_wm_base_pong(xdgWmBase, serial);
}

const struct xdg_wm_base_listener xdgWmBaseListener = {
    .ping = xdgWmBasePing,
};

/*--------------------------------------------------------------------------
**  Purpose:        Surface frame handling functions.
**                  The frame painting complete event.
**
**                  We pause to pace frame refreshes at our desired rate
**                  and then prepare a new content buffer and push it out
**                  to the compositor.
**                  NOTE: by the time we arrive here we "should" have 
**                  received the Wayland buffer release event and so
**                  "should" be able to just fill the existing content
**                  buffer with new content and send it back out.
**
**  Parameters:     Name        Description.
**                  data        Client state data pointer handed back to us.
**                  cb          The callback object used to call us.
**                  time        A millisecond timestamp for this frame
**                              paint completion.
**
**  Returns:        Nothing, we prepare and dispatch a new output frame.
**                  Thus the core iterative loop painting frames flows
**                  through this callback / event framework.
**
**------------------------------------------------------------------------*/
void
wlSurfaceFrameDone(void *data, struct wl_callback *cb, uint32_t time)
    {
    WlClientState *state = data;
    static bool sendPPChar = false;
    wayDebug(2, LogErrorLocation, "Entering surface frame done at time = %d.\n", time);

    /*--------------------------------------------------------------------------
    **  Destroy the passed in callback because it can only be used once and
    **  we are given a new one when we request the next frame.
    **------------------------------------------------------------------------*/
    wl_callback_destroy(cb);

    /*--------------------------------------------------------------------------
    **  Terminate the frame refresh processing if we have an outstanding window
    **  size reconfiguration. This is necessary so that all frame buffers are
    **  properly released for resizing. The resize logic is responsible for
    **  restarting the frame refresh processing once all resize operations are
    **  complete.
    **------------------------------------------------------------------------*/
    if ((state->pendingWidth > 0) && (state->pendingHeight > 0))
        {
        wayDebug(2, LogErrorLocation, "SurfaceFrameDone entered with a pending resize.\n");
        wayDebug(2, LogErrorLocation, "  new width = %d, new height = %d.\n",
            state->pendingWidth, state->pendingHeight);
        state->lastFrame = time;
        return;
        }

    /*--------------------------------------------------------------------------
    **  Paint the screen at about the configured frames per second, which is
    **  about 10ms. Delay if we have come back in less time. The returned time
    **  values are in increasing milliseconds but not necesssarily synchronized
    **  to the time if day clock.
    **------------------------------------------------------------------------*/
    int delay = time - state->lastFrame;
    int waitTime = (FrameTime / 1000) - delay;
    if (waitTime > 0)
        {
        sleepMsec((u32)waitTime);
        }

    /*--------------------------------------------------------------------------
    **  Send any queued key presses to the PPU. Note we send a single character
    **  every alternate frame refresh if the target PPU character buffer is
    **  empty (value 0) to try and pace the PPU load.
    **------------------------------------------------------------------------*/
    sendPPChar = !sendPPChar;
    if (clipToKeyboardDelay > 0)
        {
            clipToKeyboardDelay--;
        }
    else
        {
        if (!isKeyBufEmpty(state) && (ppKeyIn == 0) && sendPPChar)
            {
            uint32_t keySym = getQueuedKey(state);
            /*--------------------------------------------------------------------------
            ** Process new line semantics
            **------------------------------------------------------------------------*/
            if (keySym == XKB_KEY_Linefeed)
                {
                /*
                **  Ignore the line feed because we only run this code on Unix type
                **  platforms.
                */
                wayDebug(2, LogErrorLocation, "Received key press symbol XKB_KEY_Linefeed\n");
                ppKeyIn = 0;
                }
            else if (keySym == XKB_KEY_Return)
                {
                wayDebug(2, LogErrorLocation, "Received key press symbol XKB_KEY_Return\n");
                ppKeyIn = '\r';
                /*
                **  Short delay to allow PP program to process the line. This may
                **  require customisation.
                */
                clipToKeyboardDelay = 30;
                }
            else if (keySym == XKB_KEY_F1)
                {
                /*----------------------------------------------------------------------
                **  Process the debug level 1 request (key F1). If we are in level 1
                **  cancel all debug otherwise set level 1.
                **--------------------------------------------------------------------*/
                logDtError(LogErrorLocation, "Received key press symbol XKB_KEY_F1\n");
                if (debugWayland == 1)
                    {
                    debugWayland = 0;
                    }
                else
                    {
                    debugWayland = 1;
                    }
                }
            else if (keySym == XKB_KEY_F2)
                {
                /*----------------------------------------------------------------------
                **  Process the debug level 2 request (key F2). If we are in level 2
                **  cancel all debug otherwise set level 2.
                **--------------------------------------------------------------------*/
                logDtError(LogErrorLocation, "Received key press symbol XKB_KEY_F2\n");
                if (debugWayland == 2)
                    {
                    debugWayland = 0;
                    }
                else
                    {
                    debugWayland = 2;
                    }
                }
            else if (keySym == XKB_KEY_F3)
                {
                /*----------------------------------------------------------------------
                **  Process the debug level 3 request (key F3). If we are in level 3
                **  cancel all debug otherwise set level 3.
                **--------------------------------------------------------------------*/
                logDtError(LogErrorLocation, "Received key press symbol XKB_KEY_F3\n");
                if (debugWayland == 3)
                    {
                    debugWayland = 0;
                    }
                else
                    {
                    debugWayland = 3;
                    }
                }
            else if (keySym == XKB_KEY_XF86Paste)
                {
                /*----------------------------------------------------------------------
                **  Process paste request (META_L or ALT_L) followed by 'p'.
                **  Set up the data device for paste operations.
                **  Note: the heavy lifting processing happens down in the listeners.
                **  The net result of this processing is to have the character content
                **  of the clipboard pushed into the keyboard buffer.
                **  We set the pasteActive flag here so that the offer listeners will
                **  read the incoming offer. Once the incoming offer has been read the
                **  pasteActive flag is reset in the event handler to prevent us 
                **  re-reading the clipboard if we loose and regain focus.
                **--------------------------------------------------------------------*/
                state->wlDataDevice = wl_data_device_manager_get_data_device(
                state->wlDataDeviceManager, state->wlSeat);
                wayDebug(1, LogErrorLocation, "Created data device for paste.\n");
                wl_data_device_add_listener(state->wlDataDevice, &wlDataDeviceListener,
                    state);
                wayDebug(1, LogErrorLocation, "Created data device listener for paste.\n");
                state->pasteActive = true;
                }
            else
                {
                uint32_t keyPress = xkb_keysym_to_utf32(keySym);
                ppKeyIn = (char)keyPress;
                }
            }
        }

    /*--------------------------------------------------------------------------
    **  The original console hardware used a CRT screen, which is not persistent.
    **  To try and emulate this behavior we fade the image buffer by 75% on
    **  each frame update and rely on the PPU refresh processing to repaint
    **  the needed information.
    **------------------------------------------------------------------------*/
    if (state->image != NULL)
        {
        int height = state->height;
        int width = state->width;
        for (int y = 0; y < height; ++y)
            {
            for (int x = 0; x < width; ++x)
                {
                state->image[y * width + x].green = state->image[y * width + x].green >> 2;
                }
             }
        }
    /*--------------------------------------------------------------------------
    **  Prepare the next frame image by processing the incoming display list
    **  from the PPU.
    **------------------------------------------------------------------------*/
    wayDebug(2, LogErrorLocation, "SurfaceFrameDone calling drawText.\n");
    drawText(state);
    if (state->image == NULL)
        {
        logDtError(LogErrorLocation, "Unable to update the frame image buffer, aborting.\n");
        return;
        }

    /*--------------------------------------------------------------------------
    **  Copy the current frame image to a buffer. Abort the frame refresh
    **  processing if we are unable to complete the populate action.
    **------------------------------------------------------------------------*/
    int n = populateFrameBuffer(state);
    if (n < 0)
        {
        return;
        }

    /*--------------------------------------------------------------------------
    **  Request another frame and configure the returned callback object.
    **  Attach the updated buffer, set the changed are to the full screen
    **  and commit the new contents to the display.
    **------------------------------------------------------------------------*/
    struct wl_callback *cb1 = wl_surface_frame(state->wlSurface);
    wl_callback_add_listener(cb1, &wlSurfaceFrameListener, state);
    wl_surface_attach(state->wlSurface, state->buffers[n].frameBuffer, 0, 0);
    wl_surface_damage_buffer(state->wlSurface, 0, 0, state->width, state->height);
    wl_surface_commit(state->wlSurface);

    /*--------------------------------------------------------------------------
    **  capture the time stamp from this frame completion to control pacing.
    **------------------------------------------------------------------------*/
    state->lastFrame = time;
    }

/*--------------------------------------------------------------------------
**  Purpose:        Surface frame handling functions.
**                  The listener structure that receives all incoming
**                  frame callback events.
**
**------------------------------------------------------------------------*/
const struct wl_callback_listener wlSurfaceFrameListener = {
    .done = wlSurfaceFrameDone,
};

/*--------------------------------------------------------------------------
**  Purpose:        Pointer event handling functions.
**                  Pointer has entered our surface.
**
**  Parameters:     Name        Description.
**                  data        Client state data pointer handed back to us.
**                  wlPointer   Abstraction of the pointer device.
**                  serial      Event serial number.
**                  surface     The surface upon which the pointer operates.
**                  surfaceX    The X co-ordinate of the pointer on the surface.
**                  surfaceY    The Y co-ordinate of the pointer on the surface.
**
**  Returns:        Nothing, we do not directly implement usage of the
**                  pointer at this time.
**
**------------------------------------------------------------------------*/
void
wlPointerEnter(void *data, struct wl_pointer *wlPointer,
               uint32_t serial, struct wl_surface *surface,
               wl_fixed_t surfaceX, wl_fixed_t surfaceY)
    {
    WlClientState *state = data;
    state->pointerEvent.eventMask |= POINTER_EVENT_ENTER;
    state->pointerEvent.serial = serial;
    state->pointerEvent.surfaceX = surfaceX,
    state->pointerEvent.surfaceY = surfaceY;
    wl_pointer_set_cursor(wlPointer, serial, state->cursorSurface,
        state->cursorImage->hotspot_x, state->cursorImage->hotspot_y);
    }

/*--------------------------------------------------------------------------
**  Purpose:        Pointer event handling functions.
**                  Pointer has left our surface.
**
**  Parameters:     Name        Description.
**                  data        Client state data pointer handed back to us.
**                  wlPointer   Abstraction of the pointer device.
**                  serial      Event serial number.
**                  surface     The surface upon which the pointer operates.
**
**  Returns:        Nothing, we do not directly implement usage of the
**                  pointer at this time.
**
**------------------------------------------------------------------------*/
void
wlPointerLeave(void *data, struct wl_pointer *wlPointer,
               uint32_t serial, struct wl_surface *surface)
    {
    WlClientState *state = data;
    state->pointerEvent.serial = serial;
    state->pointerEvent.eventMask |= POINTER_EVENT_LEAVE;
    }

/*--------------------------------------------------------------------------
**  Purpose:        Pointer event handling functions.
**                  Pointer is moving over our surface.
**
**  Parameters:     Name        Description.
**                  data        Client state data pointer handed back to us.
**                  wlPointer   Abstraction of the pointer device.
**                  time        A millisecond timestamp for this pointer
**                              location.
**                  surfaceX    The X co-ordinate of the pointer on the surface.
**                  surfaceY    The Y co-ordinate of the pointer on the surface.
**
**  Returns:        Nothing, we do not directly implement usage of the
**                  pointer at this time.
**
**------------------------------------------------------------------------*/
void
wlPointerMotion(void *data, struct wl_pointer *wlPointer, uint32_t time,
               wl_fixed_t surfaceX, wl_fixed_t surfaceY)
    {
    WlClientState *state = data;
    state->pointerEvent.eventMask |= POINTER_EVENT_MOTION;
    state->pointerEvent.time = time;
    state->pointerEvent.surfaceX = surfaceX,
    state->pointerEvent.surfaceY = surfaceY;
    }

/*--------------------------------------------------------------------------
**  Purpose:        Pointer event handling functions.
**                  Pointer button has been activated.
**
**  Parameters:     Name        Description.
**                  data        Client state data pointer handed back to us.
**                  wlPointer   Abstraction of the pointer device.
**                  serial      Event serial number.
**                  time        A millisecond timestamp for this pointer
**                              location.
**                  button      Identification of which pointer button activated.
**                  eventState  The action activated for the button.
**
**  Returns:        Nothing, we do not directly implement usage of the
**                  pointer at this time.
**
**------------------------------------------------------------------------*/
void
wlPointerButton(void *data, struct wl_pointer *wlPointer, uint32_t serial,
               uint32_t time, uint32_t button, uint32_t eventState)
    {
    WlClientState *state = data;
    state->pointerEvent.eventMask |= POINTER_EVENT_BUTTON;
    state->pointerEvent.time = time;
    state->pointerEvent.serial = serial;
    state->pointerEvent.button = button,
    state->pointerEvent.ptrState = eventState;
    }

/*--------------------------------------------------------------------------
**  Purpose:        Pointer event handling functions.
**                  Pointer scroll wheel has been activated.
**
**  Parameters:     Name        Description.
**                  data        Client state data pointer handed back to us.
**                  wlPointer   Abstraction of the pointer device.
**                  time        A millisecond timestamp for this pointer
**                              location.
**                  axis        The scroll wheel axis activated.
**                  value       The change for the scroll wheel.
**
**  Returns:        Nothing, we do not directly implement usage of the
**                  pointer at this time.
**
**------------------------------------------------------------------------*/
void
wlPointerAxis(void *data, struct wl_pointer *wlPointer, uint32_t time,
               uint32_t axis, wl_fixed_t value)
    {
    WlClientState *state = data;
    state->pointerEvent.eventMask |= POINTER_EVENT_AXIS;
    state->pointerEvent.time = time;
    state->pointerEvent.axes[axis].valid = true;
    state->pointerEvent.axes[axis].value = value;
    }

/*--------------------------------------------------------------------------
**  Purpose:        Pointer event handling functions.
**                  Inform us which pointer device was activated.
**
**  Parameters:     Name        Description.
**                  data        Client state data pointer handed back to us.
**                  wlPointer   Abstraction of the pointer device.
**                  axisSource  The scroll device used.
**
**  Returns:        Nothing, we do not directly implement usage of the
**                  pointer at this time.
**
**------------------------------------------------------------------------*/
void
wlPointerAxisSource(void *data, struct wl_pointer *wlPointer,
               uint32_t axisSource)
    {
    WlClientState *state = data;
    state->pointerEvent.eventMask |= POINTER_EVENT_AXIS_SOURCE;
    state->pointerEvent.axisSource = axisSource;
    }

/*--------------------------------------------------------------------------
**  Purpose:        Pointer event handling functions.
**                  Inform us that usage of a scroll device has terminated.
**
**  Parameters:     Name        Description.
**                  data        Client state data pointer handed back to us.
**                  wlPointer   Abstraction of the pointer device.
**                  time        A millisecond timestamp for this pointer
**                              location.
**                  axis        The scroll device which has stopped.
**
**  Returns:        Nothing, we do not directly implement usage of the
**                  pointer at this time.
**
**------------------------------------------------------------------------*/
void
wlPointerAxisStop(void *data, struct wl_pointer *wlPointer,
               uint32_t time, uint32_t axis)
    {
    WlClientState *state = data;
    state->pointerEvent.time = time;
    state->pointerEvent.eventMask |= POINTER_EVENT_AXIS_STOP;
    state->pointerEvent.axes[axis].valid = true;
    }

/*--------------------------------------------------------------------------
**  Purpose:        Pointer event handling functions.
**                  Inform us of the discrete step information for the scroll.
**
**  Parameters:     Name        Description.
**                  data        Client state data pointer handed back to us.
**                  wlPointer   Abstraction of the pointer device.
**                  axis        The scroll device being reported.
**                  discrete    The step information for the scroll.
**
**  Returns:        Nothing, we do not directly implement usage of the
**                  pointer at this time.
**
**------------------------------------------------------------------------*/
void
wlPointerAxisDiscrete(void *data, struct wl_pointer *wlPointer,
               uint32_t axis, int32_t discrete)
    {
    WlClientState *state = data;
    state->pointerEvent.eventMask |= POINTER_EVENT_AXIS_DISCRETE;
    state->pointerEvent.axes[axis].valid = true;
    state->pointerEvent.axes[axis].discrete = discrete;
    }

/*--------------------------------------------------------------------------
**  Purpose:        Pointer event handling functions.
**                  The end of a sequence of pointer actions has been 
**                  encountered so we can now process the overall pointer
**                  input action.
**
**  Parameters:     Name        Description.
**                  data        Client state data pointer handed back to us.
**                  wlPointer   Abstraction of the pointer device.
**
**  Returns:        Nothing, we do not directly implement usage of the
**                  pointer at this time.
**
**------------------------------------------------------------------------*/
void
wlPointerFrame(void *data, struct wl_pointer *wlPointer)
    {
    WlClientState *state = data;
    struct pointerEvent *event = &state->pointerEvent;
 
    if (event->eventMask & POINTER_EVENT_ENTER)
        {
        wayDebug(2, LogErrorLocation, "entered %f, %f ",
            wl_fixed_to_double(event->surfaceX),
            wl_fixed_to_double(event->surfaceY));
        }
 
    if (event->eventMask & POINTER_EVENT_LEAVE)
        {
        wayDebug(2, LogErrorLocation, "leave");
        }
 
    if (event->eventMask & POINTER_EVENT_MOTION)
        {
        wayDebug(2, LogErrorLocation, "motion %f, %f ",
            wl_fixed_to_double(event->surfaceX),
            wl_fixed_to_double(event->surfaceY));
        }
 
    if (event->eventMask & POINTER_EVENT_BUTTON)
        {
        state->processConfigure = (event->ptrState == WL_POINTER_BUTTON_STATE_RELEASED);
        wayDebug(2, LogErrorLocation, "button %d %s ", event->button,
            event->ptrState == WL_POINTER_BUTTON_STATE_RELEASED ? "released" : "pressed");
        }
 
    if (event->eventMask & axis_events)
        {
        char line[150] = "\0";
        char tmp[30] = "\0";
        for (size_t i = 0; i < 2; ++i)
            {
            if (!event->axes[i].valid)
                {
                continue;
                }
            snprintf(tmp, 30, "%s axis ", axis_name[i]);
            strcat(line, tmp);
            if (event->eventMask & POINTER_EVENT_AXIS)
                {
                snprintf(tmp, 30, "value %f ", wl_fixed_to_double(event->axes[i].value));
                strcat(line, tmp);
                }
            if (event->eventMask & POINTER_EVENT_AXIS_DISCRETE)
                {
                snprintf(tmp, 30, "discrete %d ", event->axes[i].discrete);
                strcat(line, tmp);
                }
            if (event->eventMask & POINTER_EVENT_AXIS_SOURCE)
                {
                snprintf(tmp, 30, "via %s ", axis_source[event->axisSource]);
                strcat(line, tmp);
                }
            if (event->eventMask & POINTER_EVENT_AXIS_STOP)
                {
                strcat(line, "(stopped)\n");
                }
            wayDebug(2, LogErrorLocation, line);
            }
        }
 
    memset(event, 0, sizeof(*event));
    }

/*--------------------------------------------------------------------------
**  Purpose:        Pointer event handling functions.
**                  The listener structure that receives all incoming
**                  pointer events.
**
**------------------------------------------------------------------------*/
const struct wl_pointer_listener wlPointerListener =
    {
    .enter = wlPointerEnter,
    .leave = wlPointerLeave,
    .motion = wlPointerMotion,
    .button = wlPointerButton,
    .axis = wlPointerAxis,
    .frame = wlPointerFrame,
    .axis_source = wlPointerAxisSource,
    .axis_stop = wlPointerAxisStop,
    .axis_discrete = wlPointerAxisDiscrete,
    };

/*--------------------------------------------------------------------------
**  Purpose:        Keyboard event handling functions.
**                  This event communicates a keyboard translation map
**                  from the Wayland compositor to our client code.
**
**  Parameters:     Name        Description.
**                  data        Client state data pointer handed back to us.
**                  wlKeyboard  Abstraction of the keyboard device.
**                  format      The incoming keyboard map format code.
**                  fd          The file descriptor from which to read the 
**                              incoming keyboard map data.
**                  size        The size of the incoming keyboard map
**                              in bytes.
**
**  Returns:        No return value, however we read the new keyboard map
**                  and initialize a new xkbcommon state to handle the 
**                  key events according to the updated map.
**
**------------------------------------------------------------------------*/
void
wlKeyboardKeymap(void *data, struct wl_keyboard *wlKeyboard,
               uint32_t format, int32_t fd, uint32_t size)
    {
    WlClientState *state = data;
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1)
        {
        logDtError(LogErrorLocation, "Ignoring incoming keyboard map of unsupported type code %d\n",
            format);
        return;
        }

    char *mapShm = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    if (mapShm == MAP_FAILED)
        {
        logDtError(LogErrorLocation, "Unable to memory map incoming keyboard map, error %d\n",
            errno);
        return;
        }

    struct xkb_keymap *xkbKeymap = xkb_keymap_new_from_string(
            state->xkbContext, mapShm,
            XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(mapShm, size);
    close(fd);

    struct xkb_state *xkbState = xkb_state_new(xkbKeymap);
    xkb_keymap_unref(state->xkbKeymap);
    xkb_state_unref(state->xkbState);
    state->xkbKeymap = xkbKeymap;
    state->xkbState = xkbState;
    }

/*--------------------------------------------------------------------------
**  Purpose:        Keyboard event handling functions.
**                  This event informs us that keyboard focus has been
**                  assigned to the identified Wayland surface.
**
**  Parameters:     Name        Description.
**                  data        Client state data pointer handed back to us.
**                  wlKeyboard  Abstraction of the keyboard device.
**                  serial      A serial number for the event.
**                  surface     The Wayland surface to which the keyboard
**                              focus has been assigned.
**                  keys        An array of key active (pressed) keys at the
**                              time focus is gained.
**
**  Returns:        No return value
**
**------------------------------------------------------------------------*/
void
wlKeyboardEnter(void *data, struct wl_keyboard *wlKeyboard,
               uint32_t serial, struct wl_surface *surface,
               struct wl_array *keys)
    {
    WlClientState *state = data;
    wayDebug(2, LogErrorLocation, "keyboard enter; keys pressed are:\n");
    uint32_t *key;
    wl_array_for_each(key, keys)
        {
        char buf[40];
        char tmp[50] = "\0";
        char line[150] = "\0";
        xkb_keysym_t sym = xkb_state_key_get_one_sym(
                    state->xkbState, *key + 8);
        xkb_keysym_get_name(sym, buf, sizeof(buf));
        snprintf(line, 150, "sym: %-12s (%d), ", buf, sym);
        xkb_state_key_get_utf8(state->xkbState,
                    *key + 8, buf, sizeof(buf));
        snprintf(tmp, 50, "utf8: '%s'\n", buf);
        strcat(line, tmp);
        wayDebug(2, LogErrorLocation, line);
        }
    }

/*--------------------------------------------------------------------------
**  Purpose:        Keyboard event handling functions.
**                  Key action (pressed or released) event.
**
**  Parameters:     Name        Description.
**                  data        Client state data pointer handed back to us.
**                  wlKeyboard  Abstraction of the keyboard device.
**                  serial      A serial number for the event.
**                  time        A millisecond timestamp for this key event.
**                  key         The keycode for the key that has changed state.
**                  keyState    The state (pressed or released) for the key. 
**
**  Returns:        No return value. We capture and queue key release codes.
**
**------------------------------------------------------------------------*/
void
wlKeyboardKey(void *data, struct wl_keyboard *wlKeyboard,
               uint32_t serial, uint32_t time, uint32_t key, uint32_t keyState)
    {
    WlClientState *state = data;
    uint32_t keyCode = key + 8;   /* Translate to xkb code from common input code */
    xkb_keysym_t sym = xkb_state_key_get_one_sym( state->xkbState, keyCode);
    if (debugWayland > 1)
        {
        char buf[128];
        char pline[MaxPline] = "\0";
        xkb_keysym_get_name(sym, buf, sizeof(buf));
        const char *action =
            keyState == WL_KEYBOARD_KEY_STATE_PRESSED ? "press" : "release";
        snprintf(pline, MaxPline, "key %s: sym: %-12s (0x%x), ", action, buf, sym);
        strcat(pline, "utf8: '%s'\n");
        xkb_state_key_get_utf8(state->xkbState, keyCode, buf, sizeof(buf));
        wayDebug(2,LogErrorLocation, pline, buf);
        }
    /*--------------------------------------------------------------------------
    **  DtCyber uses the sequence (META_L or ALT_L) 'p' to indicate a paste from
    **  the clipboard operation. Here we check which modifier keys are active
    **  and if we have this sequence we continue forward with a borrowed PASTE
    **  key symbol code from the xkbcommon set. This makes the processing of
    **  the paste action later simpler and more reliable.
    **------------------------------------------------------------------------*/
    if (keyState == WL_KEYBOARD_KEY_STATE_RELEASED)
        {
        if (xkb_state_mod_name_is_active(state->xkbState, XKB_MOD_NAME_ALT,
                XKB_STATE_MODS_EFFECTIVE) || isMeta)
            {
            uint32_t keyPress = xkb_keysym_to_utf32(sym);
            switch (keyPress)
                {
            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9':
                traceMask ^= (1 << (keyPress - '0'));
                break;

            case 'c':
                traceMask ^= (1 << 14);
                break;

            case 'e':
                traceMask ^= (1 << 15);
                break;

            case 'x':
                if (traceMask == 0)
                    {
                    traceMask = ~0;
                    }
                else
                    {
                    traceMask = 0;
                    }
                break;

            case 'p':
                sym = XKB_KEY_XF86Paste;
                break;

                }
            }

        if ((sym != XKB_KEY_NoSymbol) && (sym != XKB_KEY_Alt_L))
            {
            wayDebug(3, LogErrorLocation, "wlKeyboardKey queueing keypress symbol '0x%0x'.\n", sym);
            queueKey(state, sym);
            }
        }
    else
        if (keyState == WL_KEYBOARD_KEY_STATE_PRESSED)
            {
            if (key == XKB_KEY_Meta_L)
                {
                isMeta = TRUE;
                }
            }
    }

/*--------------------------------------------------------------------------
**  Purpose:        Keyboard event handling functions.
**                  Keyboard focus leaves our Wayland surface
**
**  Parameters:     Name        Description.
**                  data        Client state data pointer handed back to us.
**                  wlKeyboard  Abstraction of the keyboard device.
**                  serial      A serial number for the event.
**                  surface     The Wayland surface that lost keyboard focus.
**
**  Returns:        No return value. We flush any queued and unprocessed 
**                  key press data.
**
**------------------------------------------------------------------------*/
void
wlKeyboardLeave(void *data, struct wl_keyboard *wlKeyboard,
               uint32_t serial, struct wl_surface *surface)
    {
    wayDebug(3, LogErrorLocation, "Keyboard leave, flush any queued key data.\n");
    WlClientState *state = data;
    while (!isKeyBufEmpty(state))
        {
        uint32_t discard = getQueuedKey(state);
        }
    }

/*--------------------------------------------------------------------------
**  Purpose:        Keyboard event handling functions.
**                  Modifier key (shift etc) status for the keyboard
**
**  Parameters:     Name          Description.
**                  data          Client state data pointer handed back to us.
**                  wlKeyboard    Abstraction of the keyboard device.
**                  serial        A serial number for the event.
**                  modsDepressed Modifier keys pressed.
**                  modsLatched   Modifier keys latched.
**                  modsLocked    Modifier keys locked.
**
**  Returns:        No return value. We pass the modifier codes on to 
**                  the xkb code.
**
**------------------------------------------------------------------------*/
void
wlKeyboardModifiers(void *data, struct wl_keyboard *wlKeyboard,
               uint32_t serial, uint32_t modsDepressed,
               uint32_t modsLatched, uint32_t modsLocked,
               uint32_t group)
    {
    WlClientState *state = data;
    xkb_state_update_mask(state->xkbState,
        modsDepressed, modsLatched, modsLocked, 0, 0, group);
    }

/*--------------------------------------------------------------------------
**  Purpose:        Keyboard event handling functions.
**                  Key auto-repeat event
**
**  Parameters:     Name          Description.
**                  data          Client state data pointer handed back to us.
**                  wlKeyboard    Abstraction of the keyboard device.
**                  rate          Auto-repeat key rate.
**                  delay         Delay before beginning auto-repeat
**
**  Returns:        No return value.
**
**------------------------------------------------------------------------*/
void
wlKeyboardRepeatInfo(void *data, struct wl_keyboard *wlKeyboard,
               int32_t rate, int32_t delay)
    {
    /* Left as an exercise for the reader */
    }

/*--------------------------------------------------------------------------
**  Purpose:        Keyboard event handling functions.
**                  The listener structure that receives all incoming
**                  keyboard events.
**
**------------------------------------------------------------------------*/
const struct wl_keyboard_listener wlKeyboardListener =
    {
    .keymap = wlKeyboardKeymap,
    .enter = wlKeyboardEnter,
    .leave = wlKeyboardLeave,
    .key = wlKeyboardKey,
    .modifiers = wlKeyboardModifiers,
    .repeat_info = wlKeyboardRepeatInfo,
    };

/*--------------------------------------------------------------------------
**  Purpose:        Touch device event handling functions.
**                  Retrieve a touch point from our list keyed by the
**                  supplied touch point identifier.
**
**                  Note that a single touch device may have many touch
**                  points active simoultaneously (think of multi-finger
**                  swiping). Hence we keep a structure to track specific
**                  touch points. The code will arbitarily stop after 10
**                  touch points.
**
**  Parameters:     Name        Description.
**                  state       Our client state structure pointer.
**                  id          The identifier of the desired touch point.
**
**  Returns:        A pointer to our local touch point data for the requested
**                  identifier.
**
**------------------------------------------------------------------------*/
struct touch_point *
getTouchPoint(WlClientState *state, int32_t id)
    {
    struct touch_event *touch = &state->touchEvent;
    const size_t nmemb = sizeof(touch->points) / sizeof(struct touch_point);
    int invalid = -1;
    for (size_t i = 0; i < nmemb; ++i)
        {
        if (touch->points[i].id == id)
            {
            return &touch->points[i];
            }
        if (invalid == -1 && !touch->points[i].valid)
            {
            invalid = i;
            }
        }
    if (invalid == -1)
        {
        return NULL;
        }
    touch->points[invalid].valid = true;
    touch->points[invalid].id = id;
    return &touch->points[invalid];
    }

/*--------------------------------------------------------------------------
**  Purpose:        Touch device event handling functions.
**                  This event communicates the creation of a new touch
**                  point on an identified Wayland surface.
**
**  Parameters:     Name        Description.
**                  data        Client state data pointer handed back to us.
**                  wlTouch     Abstraction of the touch device.
**                  serial      Event serial number.
**                  time        A millisecond timestamp for this pointer
**                              location.
**                  surface     The Wayland surface up on which the x and y
**                              co-ordinates are valid.
**                  id          The touch point identifier just created.
**                  x           The x location of the touch point.
**                  y           The y location of the touch point.
**
**  Returns:        No return value.
**
**------------------------------------------------------------------------*/
void
wlTouchDown(void *data, struct wl_touch *wlTouch, uint32_t serial,
               uint32_t time, struct wl_surface *surface, int32_t id,
               wl_fixed_t x, wl_fixed_t y)
    {
    WlClientState *state = data;
    struct touch_point *point = getTouchPoint(state, id);
    if (point == NULL)
        {
        return;
        }
    point->eventMask |= TOUCH_EVENT_DOWN;
    point->surfaceX = wl_fixed_to_double(x),
    point->surfaceY = wl_fixed_to_double(y);
    state->touchEvent.time = time;
    state->touchEvent.serial = serial;
    }

/*--------------------------------------------------------------------------
**  Purpose:        Touch device event handling functions.
**                  This event communicates the deletion of a touch point
**                  from an identified Wayland surface.
**
**  Parameters:     Name        Description.
**                  data        Client state data pointer handed back to us.
**                  wlTouch     Abstraction of the touch device.
**                  serial      Event serial number.
**                  time        A millisecond timestamp for this pointer
**                              location.
**                  id          The touch point identifier just created.
**
**  Returns:        No return value.
**
**------------------------------------------------------------------------*/
void
wlTouchUp(void *data, struct wl_touch *wlTouch, uint32_t serial,
            uint32_t time, int32_t id)
    {
    WlClientState *state = data;
    struct touch_point *point = getTouchPoint(state, id);
    if (point == NULL)
        {
        return;
        }
    point->eventMask |= TOUCH_EVENT_UP;
    }

/*--------------------------------------------------------------------------
**  Purpose:        Touch device event handling functions.
**                  This event communicates the movement of a touch point
**                  over it's Wayland surface.
**
**  Parameters:     Name        Description.
**                  data        Client state data pointer handed back to us.
**                  wlTouch     Abstraction of the touch device.
**                  time        A millisecond timestamp for this pointer
**                              location.
**                  id          The touch point identifier just created.
**                  x           The x location of the touch point.
**                  y           The y location of the touch point.
**
**  Returns:        No return value.
**
**------------------------------------------------------------------------*/
void
wlTouchMotion(void *data, struct wl_touch *wlTouch, uint32_t time,
               int32_t id, wl_fixed_t x, wl_fixed_t y)
    {
    WlClientState *state = data;
    struct touch_point *point = getTouchPoint(state, id);
    if (point == NULL)
        {
        return;
        }
    point->eventMask |= TOUCH_EVENT_MOTION;
    point->surfaceX = x;
    point->surfaceY = y;
    state->touchEvent.time = time;
    }

/*--------------------------------------------------------------------------
**  Purpose:        Touch device event handling functions.
**                  This event communicates the end of focus for the touch
**                  device on it's Wayland surface. Any existing touch 
**                  points are invalidated.

**
**  Parameters:     Name        Description.
**                  data        Client state data pointer handed back to us.
**                  wlTouch     Abstraction of the touch device.
**
**  Returns:        No return value.
**
**------------------------------------------------------------------------*/
void
wlTouchCancel(void *data, struct wl_touch *wlTouch)
    {
    WlClientState *state = data;
    state->touchEvent.eventMask |= TOUCH_EVENT_CANCEL;
    }

/*--------------------------------------------------------------------------
**  Purpose:        Touch device event handling functions.
**                  This event communicates an update to the ellipse defining
**                  the shape of the touch area for a touch point.
**
**  Parameters:     Name        Description.
**                  data        Client state data pointer handed back to us.
**                  wlTouch     Abstraction of the touch device.
**                  id          The identifier of the touch point.
**                  major       The major axis length for the touch point.
**                  minor       The minor axiz length for the touch point.
**
**  Returns:        No return value.
**
**------------------------------------------------------------------------*/
void
wlTouchShape(void *data, struct wl_touch *wlTouch,
               int32_t id, wl_fixed_t major, wl_fixed_t minor)
    {
    WlClientState *state = data;
    struct touch_point *point = getTouchPoint(state, id);
    if (point == NULL)
        {
        return;
        }
    point->eventMask |= TOUCH_EVENT_SHAPE;
    point->major = major;
    point->minor = minor;
    }

/*--------------------------------------------------------------------------
**  Purpose:        Touch device event handling functions.
**                  This event communicates a change to the orientation of
**                  the ellipse defining the shape of the touch area for
**                  a touch point.
**
**  Parameters:     Name        Description.
**                  data        Client state data pointer handed back to us.
**                  wlTouch     Abstraction of the touch device.
**                  id          The identifier of the touch point.
**                  orientation The angle between the major axis and the
**                              surface y-axis for the touch point.
**
**  Returns:        No return value.
**
**------------------------------------------------------------------------*/
void
wlTouchOrientation(void *data, struct wl_touch *wlTouch,
                     int32_t id, wl_fixed_t orientation)
    {
    WlClientState *state = data;
    struct touch_point *point = getTouchPoint(state, id);
    if (point == NULL)
        {
        return;
        }
    point->eventMask |= TOUCH_EVENT_ORIENTATION;
    point->orientation = orientation;
    }

/*--------------------------------------------------------------------------
**  Purpose:        Touch device event handling functions.
**                  This event communicates the end of a set of events
**                  defining a logical touch action on a touch device.
**
**  Parameters:     Name        Description.
**                  data        Client state data pointer handed back to us.
**                  wlTouch     Abstraction of the touch device.
**
**  Returns:        No return value.
**
**------------------------------------------------------------------------*/
void
wlTouchFrame(void *data, struct wl_touch *wlTouch)
    {
    WlClientState *state = data;
    struct touch_event *touch = &state->touchEvent;
    const size_t nmemb = sizeof(touch->points) / sizeof(struct touch_point);
    wayDebug(3, LogErrorLocation, "touch event @ %d:\n", touch->time);

    for (size_t i = 0; i < nmemb; ++i)
        {
        struct touch_point *point = &touch->points[i];
        if (!point->valid)
            {
                continue;
            }
//        logDtError(LogErrorLocation, "point %d: ", touch->points[i].id);

        if (point->eventMask & TOUCH_EVENT_DOWN)
            {
//            logDtError(LogErrorLocation, "down %f,%f ",
//                    wl_fixed_to_double(point->surfaceX),
//                    wl_fixed_to_double(point->surfaceY));
            }

        if (point->eventMask & TOUCH_EVENT_UP)
            {
//            logDtError(LogErrorLocation, "up ");
            }

        if (point->eventMask & TOUCH_EVENT_MOTION)
            {
//            logDtError(LogErrorLocation, "motion %f,%f ",
//                    wl_fixed_to_double(point->surfaceX),
//                    wl_fixed_to_double(point->surfaceY));
            }

        if (point->eventMask & TOUCH_EVENT_SHAPE)
            {
//            logDtError(LogErrorLocation, "shape %fx%f ",
//                    wl_fixed_to_double(point->major),
//                    wl_fixed_to_double(point->minor));
            }

        if (point->eventMask & TOUCH_EVENT_ORIENTATION)
            {
//            logDtError(LogErrorLocation, "orientation %f ",
//                    wl_fixed_to_double(point->orientation));
            }

        point->valid = false;
//        logDtError(LogErrorLocation, "\n");
        }
    }

/*--------------------------------------------------------------------------
**  Purpose:        Touch device event handling functions.
**                  The listener structure that receives all incoming
**                  touch device events.
**
**------------------------------------------------------------------------*/
const struct wl_touch_listener wlTouchListener = {
    .down = wlTouchDown,
    .up = wlTouchUp,
    .motion = wlTouchMotion,
    .frame = wlTouchFrame,
    .cancel = wlTouchCancel,
    .shape = wlTouchShape,
    .orientation = wlTouchOrientation,
};

/*--------------------------------------------------------------------------
**  Purpose:        Wayland seat event handling functions.
**                  The Wayland seat abstraction covers all input devices
**                  available to the Wayland client from the Wayland
**                  compositor.
**
**                  This event communicates what input device capabilities
**                  are available to the Wayland client.
**
**  Parameters:     Name         Description.
**                  data         Client state data pointer handed back to us.
**                  wlSeat       Abstraction of the collection of input
**                               devices.
**                  capabilities A bit mask of available devices.
**
**  Returns:        No return value, however we initialize event handling
**                  functionality for each available input device and clean
**                  up our support if an input device has disappeared..
**
**------------------------------------------------------------------------*/
void
wlSeatCapabilities(void *data, struct wl_seat *wlSeat, uint32_t capabilities)
    {
    WlClientState *state = data;

    bool have_pointer = capabilities & WL_SEAT_CAPABILITY_POINTER;

    if (have_pointer && state->wlPointer == NULL)
        {
        wayDebug(1, LogErrorLocation, "Adding pointer input capability.\n");
        state->wlPointer = wl_seat_get_pointer(state->wlSeat);
        wl_pointer_add_listener(state->wlPointer, &wlPointerListener, state);
        }
    else if (!have_pointer && state->wlPointer != NULL)
        {
        wayDebug(1, LogErrorLocation, "Removing pointer input capability.\n");
        wl_pointer_release(state->wlPointer);
        state->wlPointer = NULL;
        }

    bool have_keyboard = capabilities & WL_SEAT_CAPABILITY_KEYBOARD;

    if (have_keyboard && state->wlKeyboard == NULL)
        {
        wayDebug(1, LogErrorLocation, "Adding keyboard input capability.\n");
        state->wlKeyboard = wl_seat_get_keyboard(state->wlSeat);
        wl_keyboard_add_listener(state->wlKeyboard,
            &wlKeyboardListener, state);
        }
    else if (!have_keyboard && state->wlKeyboard != NULL)
        {
        wayDebug(1, LogErrorLocation, "Removing keyboard input capability.\n");
        wl_keyboard_release(state->wlKeyboard);
        state->wlKeyboard = NULL;
        }

    bool have_touch = capabilities & WL_SEAT_CAPABILITY_TOUCH;

    if (have_touch && state->wlTouch == NULL)
        {
        wayDebug(1, LogErrorLocation, "Adding touch input capability.\n");
        state->wlTouch = wl_seat_get_touch(state->wlSeat);
        wl_touch_add_listener(state->wlTouch,
                &wlTouchListener, state);
        }
    else if (!have_touch && state->wlTouch != NULL)
        {
        wayDebug(1, LogErrorLocation, "Removing touch input capability.");
        wl_touch_release(state->wlTouch);
        state->wlTouch = NULL;
        }
    }

/*--------------------------------------------------------------------------
**  Purpose:        Wayland seat event handling functions.
**                  This event communicates a name string for the seat.
**
**  Parameters:     Name         Description.
**                  data         Client state data pointer handed back to us.
**                  wlSeat       Abstraction of the collection of input
**                               devices.
**                  name         A character string name for the input seat.
**
**  Returns:        No return value.
**
**------------------------------------------------------------------------*/
void
wlSeatName(void *data, struct wl_seat *wlSeat, const char *name)
{
    WlClientState *state = data;
    wayDebug(1, LogErrorLocation, "seat name: %s\n", name);
}

/*--------------------------------------------------------------------------
**  Purpose:        Wayland seat event handling functions.
**                  The listener structure that receives all incoming
**                  Wayland seat events.
**
**------------------------------------------------------------------------*/
const struct wl_seat_listener wlSeatListener = {
   .capabilities = wlSeatCapabilities,
   .name = wlSeatName,
};

/*--------------------------------------------------------------------------
**  Purpose:        Wayland global object registration events.
**                  This event communicates the availability of a Wayland
**                  global object interface at some version level.
**
**  Parameters:     Name         Description.
**                  data         Client state data pointer handed back to us.
**                  wlRegistry   The Wayland registry repoting availability.
**                               devices.
**                  name         The registry index for the interface.
**                  interface    The readable interface name.
**                  version      The version level supported by the interface.
**
**  Returns:        No return value, however we capture an interface pointer
**                  for each Wayland object interface we are interested in.
**
**------------------------------------------------------------------------*/
void
registryGlobal(void *data, struct wl_registry *wlRegistry,
        uint32_t name, const char *interface, uint32_t version)
    {
    WlClientState *state = data;
    if (strcmp(interface, wl_shm_interface.name) == 0)
        {
        wayDebug(1, LogErrorLocation, "Found Wayland interface %s at version %d\n",
            wl_shm_interface.name, version);
        state->wlShm = wl_registry_bind(
                wlRegistry, name, &wl_shm_interface, 1);
        wayDebug(1, LogErrorLocation, "Bound Wayland interface %s at version %d\n",
            wl_shm_interface.name, 1);
        }
    else if (strcmp(interface, wl_compositor_interface.name) == 0)
        {
        wayDebug(1, LogErrorLocation, "Found Wayland interface %s at version %d\n",
            wl_compositor_interface.name, version);
        state->wlCompositor = wl_registry_bind(
                wlRegistry, name, &wl_compositor_interface, 4);
        wayDebug(1, LogErrorLocation, "Bound Wayland interface %s at version %d\n",
            wl_compositor_interface.name, 4);
        }
    if (strcmp(interface, wl_data_device_manager_interface.name) == 0)
        {
        wayDebug(1, LogErrorLocation, "Found Wayland interface %s at version %d\n",
            wl_data_device_manager_interface.name, version);
		state->wlDataDeviceManager = wl_registry_bind(
                wlRegistry, name, &wl_data_device_manager_interface, 3);
        wayDebug(1, LogErrorLocation, "Bound Wayland interface %s at version %d\n",
            wl_data_device_manager_interface.name, 3);
        }
    else if (strcmp(interface, xdg_wm_base_interface.name) == 0)
        {
        wayDebug(1, LogErrorLocation, "Found Wayland interface %s at version %d\n",
            xdg_wm_base_interface.name, version);
        state->xdgWmBase = wl_registry_bind(
                wlRegistry, name, &xdg_wm_base_interface, 1);
        wayDebug(1, LogErrorLocation, "Bound Wayland interface %s at version %d\n",
            xdg_wm_base_interface.name, 1);
        xdg_wm_base_add_listener(state->xdgWmBase,
                &xdgWmBaseListener, state);
        }
    else if (strcmp(interface, wl_seat_interface.name) == 0)
        {
        wayDebug(1, LogErrorLocation, "Found Wayland interface %s at version %d\n",
            wl_seat_interface.name, version);
        state->wlSeat = wl_registry_bind(
                wlRegistry, name, &wl_seat_interface, 7);
        wayDebug(1, LogErrorLocation, "Bound Wayland interface %s at version %d\n",
            wl_seat_interface.name, 7);
        wl_seat_add_listener(state->wlSeat, &wlSeatListener, state);
        }
    else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0)
        {
        wayDebug(1, LogErrorLocation, "Found Wayland interface %s at version %d\n",
            zxdg_decoration_manager_v1_interface.name, version);
	    state->zxdgDecorationManagerV1 = wl_registry_bind(
                wlRegistry, name, &zxdg_decoration_manager_v1_interface, 1);
        wayDebug(1, LogErrorLocation, "Bound Wayland interface %s at version %d\n",
            zxdg_decoration_manager_v1_interface.name, 1);
        }
    else
        wayDebug(1, LogErrorLocation, "Found Wayland interface %s at version %d\n",
            interface, version);
    }

/*--------------------------------------------------------------------------
**  Purpose:        Wayland global object registration events.
**                  This event communicates the removal of a Wayland
**                  global object interface.
**
**  Parameters:     Name         Description.
**                  data         Client state data pointer handed back to us.
**                  wlRegistry   The Wayland registry repoting availability.
**                               devices.
**                  name         The registry index for the interface.
**
**  Returns:        No return value, however we capture an interface pointer
**                  for each Wayland object interface we are interested in.
**
**------------------------------------------------------------------------*/
void
registryGlobalRemove(void *data,
        struct wl_registry *wl_registry, uint32_t name)
{
    /*--------------------------------------------------------------------------
    **  This space deliberately left blank
    **
    **  We probably need to enhance our code to track interface index numbers
    **  and perform some cleanup in response to this event if we have a live
    **  pointer to the removed interface.
    **
    **------------------------------------------------------------------------*/
}

/*--------------------------------------------------------------------------
**  Purpose:        Wayland global registry event handling functions.
**                  The listener structure that receives all incoming
**                  Wayland global registry events.
**
**------------------------------------------------------------------------*/
const struct wl_registry_listener wlRegistryListener = {
    .global = registryGlobal,
    .global_remove = registryGlobalRemove,
};

/*--------------------------------------------------------------------------
**  Purpose:        Freetype font processing routines
**                  This function initilaizes a DtCyberFont structure to 
**                  an unused state.
**
**  Parameters:     Name         Description.
**                  state        Client state data pointer.
**                  ndx          The DtCyberFont entry to be initialized.
**                               If the entry number is invalid we return
**                               having done nothing.
**
**  Returns:        No return value, however the indicated DtCyberFont 
**                  entry is unconditionally set to an initialized state.
**
**------------------------------------------------------------------------*/
void
initDtCyberFont(WlClientState *state, int ndx)
    {
    if ((ndx < 0) || (ndx >= MAXFONTS))
        {
        return;
        }
    DtCyberFont *font = &state->fonts[ndx];
    font->pointSize = 0;
    font->fontFamily = NULL;
    font->filePath = NULL;
    font->face = NULL;
    font->bsAdvance = 0;
    for (int gndx = 0; gndx < MAXGLYPHS; gndx++)
        {
        font->glyphCache[gndx] = NULL;
        }
    }

/*--------------------------------------------------------------------------
**  Purpose:        Freetype font processing routines
**                  This function loads a DtCyberFont structure with 
**                  state needed to use the requested font family at the
**                  requested point size.
**
**  Parameters:     Name         Description.
**                  state        Client state data pointer.
**                  ndx          The DtCyberFont entry to be initialized.
**                               If the entry number is invalid we return
**                               having done nothing.
**                  fontFamily   A character string containing the desired
**                               font family name.
**                  pointSize    A double precision number spaeifying the
**                               desirec font size.
**
**  Returns:        TRUE on success or FALSE on failure. If we encounter
**                  a failure condition the DtCyberFont structure is 
**                  reset to an initialized state.
**
**------------------------------------------------------------------------*/
bool
loadDtCyberFont(WlClientState *state, int ndx, char *fontFamily,
        double pointSize)
    {
    if ((ndx < 0) || (ndx >= MAXFONTS))
        {
        initDtCyberFont(state, ndx);
        return false;
        }

    DtCyberFont *font = &state->fonts[ndx];
    font->pointSize = pointSize;
    font->fontFamily = fontFamily;
    wayDebug(1, LogErrorLocation, "About to locate the font file.\n");
    font->filePath = findFontFile(font->fontFamily, font->pointSize);
    if (font->filePath == NULL)
        {
        logDtError(LogErrorLocation, "Unable to locate font definition file for family %s at size %e\n",
            fontFamily, pointSize);
        initDtCyberFont(state, ndx);
        return false;
        }
    wayDebug(1, LogErrorLocation, "About to load the font in file %s\n", font->filePath);
    FT_Error error = FT_New_Face(state->library, font->filePath, 0, &font->face);
    if (error == FT_Err_Unknown_File_Format)
        {
        logDtError(LogErrorLocation, "The font in file %s has an unsupported format\n",
            font->filePath);
        initDtCyberFont(state, ndx);
        return false;
        }
    else if (error != FT_Err_Ok)
        {
        logDtError(LogErrorLocation, "Error loading the font face %d\n", error);
        initDtCyberFont(state, ndx);
        return false;
        }

    /*--------------------------------------------------------------------------
    **  To keep keyboard backspace processing simple we constrain the font
    **  used to be mono spaced. If we dont do this we need to keep a shadow
    **  of every character input so that we can correctly compute the pixel
    **  area to be blanked for the backspace.
    **------------------------------------------------------------------------*/
    if (!FT_IS_FIXED_WIDTH(font->face))

        {
        logDtError(LogErrorLocation, "Your selcted font family is not mono space.\n");
        FT_Done_Face(font->face);
        initDtCyberFont(state, ndx);
        return false;
        }

    /*--------------------------------------------------------------------------
    **  Debug print some potentially useful face information.
    **------------------------------------------------------------------------*/
    wayDebug(1, LogErrorLocation, "Your selected font family has %d bitmap strikes available.\n",
        font->face->num_fixed_sizes);
    if (font->face->num_fixed_sizes > 0)
        {
        for (int ndx = 0; ndx < font->face->num_fixed_sizes; ndx++)
            {
            wayDebug(1, LogErrorLocation, "  For size %d we have width %d and height %d.\n",
                ndx, font->face->available_sizes[ndx].width,
                font->face->available_sizes[ndx].height);
            }
        }
    wayDebug(1, LogErrorLocation, "Your selected face has a bbox of: xMin = %d xMax = %d yMin = %d yMax = %d.\n",
        font->face->bbox.xMin >> 6, font->face->bbox.xMax >> 6,
        font->face->bbox.yMin >> 6, font->face->bbox.yMax >> 6);

    /*--------------------------------------------------------------------------
    **  Draw text pattern at the specified point size and DPI value. For now the
    **  DPI value is hard coded in the constants above. If one day we can get
    **  the active screen resolution back from Wayland this code can be updated
    **  to dynamically support the real resolution.
    **
    **  We choose the size here so we can set an initial pen position.
    **------------------------------------------------------------------------*/
    if (font->face->num_fixed_sizes > 0)
        {
        error = FT_Select_Size(font->face, 0);
        if (error != FT_Err_Ok)
            {
            logDtError(LogErrorLocation, "Unable to select bitmap strike index 0, error = %d\n",
                error);
            FT_Done_Face(font->face);
            initDtCyberFont(state, ndx);
            return false;
            }
        }
    else
        {
        error = FT_Set_Char_Size(font->face, font->pointSize * 64.0, 0, DPI, 0);
        if (error != FT_Err_Ok)
            {
            logDtError(LogErrorLocation, "Unable to set character font size for output, error = %d\n",
                error);
            FT_Done_Face(font->face);
            initDtCyberFont(state, ndx);
            return false;
            }
        }

    /*--------------------------------------------------------------------------
    **  Some mono spaced fonts have an incorrect max_advance size value. Work
    **  around that here by loading a glyph for the character 'w' and
    **  extracting the glyph advance as our base space advance value.
    **  Note: the glyph advance is in 16.16 format and needs to be shifted
    **  right by 10 bit (16 -6) to convert it to 26.6 format for the pen.
    **------------------------------------------------------------------------*/
    uint32_t keyPress = 'w';
    error = FT_Load_Char(font->face, keyPress, FT_LOAD_RENDER);
    if (error == FT_Err_Ok)
        {
        error = FT_Get_Glyph(font->face->glyph, &font->glyphCache[keyPress]);
        if (error == FT_Err_Ok)
            {
            wayDebug(1, LogErrorLocation, "Caching a new glyph for keypress '%x'\n",
                keyPress);
            font->bsAdvance = font->glyphCache[keyPress]->advance.x >> 10;
            }
        }
    else
        {
        logDtError(LogErrorLocation, "Unable to load character code '%x', error = %d\n",
            keyPress, error);
        FT_Done_Face(font->face);
        initDtCyberFont(state, ndx);
        return false;
        }
    return true;
    }

/*--------------------------------------------------------------------------
**  Purpose:        Freetype font processing routines
**                  This function clears a DtCyberFont structure by 
**                  releasing all Freetype structures and initializing
**                  the DtCyberFont structure.
**
**  Parameters:     Name         Description.
**                  state        Client state data pointer.
**                  ndx          The DtCyberFont entry to be initialized.
**                               If the entry number is invalid we return
**                               having done nothing.
**
**  Returns:        No return value, however the indicated DtCyberFont 
**                  entry is unconditionally set to an initialized state.
**
**------------------------------------------------------------------------*/
void
clearDtCyberFont(WlClientState *state, int ndx)
    {
    state->fonts[ndx].pointSize = 0;
    state->fonts[ndx].bsAdvance = 0;
    state->fonts[ndx].fontFamily = fontName;
    if (state->fonts[ndx].filePath != NULL)
        {
        free((void *)state->fonts[ndx].filePath);
        state->fonts[ndx].filePath = NULL;
        }
    for (int gndx = 0; gndx < MAXGLYPHS; gndx++)
        {
        if (state->fonts[ndx].glyphCache[gndx] != NULL)
            {
            FT_Done_Glyph(state->fonts[ndx].glyphCache[gndx]);
            state->fonts[ndx].glyphCache[gndx] = NULL;
            }
        }
    if (state->fonts[ndx].face != NULL)
        {
        FT_Done_Face(state->fonts[ndx].face);
        }
    initDtCyberFont(state, ndx);
    }

/*--------------------------------------------------------------------------
**  Purpose:        Windows thread.
**
**  Parameters:     Name        Description.
**
**  Returns:        Nothing.
**
**------------------------------------------------------------------------*/
void *windowThread(void *param)
    {
    unsigned long     bg;
    int               depth;
    unsigned long     fg;
    int               len;
    static int        refreshCount = 0;
    int               retFormat;
    unsigned long     retLength;
    unsigned long     retRemaining;
    int               retStatus;
    int               screen;
    char              str[2] = " ";
    char              windowTitle[132];

    /*--------------------------------------------------------------------------
    **  Initial setup of client state object
    **------------------------------------------------------------------------*/
    wayDebug(1, LogErrorLocation, "Entered windowThread\n");

    state.closed = false;
    state.syncDone = false;
    state.width = 1100;
    state.height = 750;
    state.pendingWidth = 0;
    state.pendingHeight = 0;
    state.processConfigure = false;
    state.pageSize = sysconf(_SC_PAGE_SIZE);
    wayDebug(2, LogErrorLocation, "windowThread calling calculatePixelBufferSize\n");
    state.pixelBufferSize = calculatePixelBufferSize(state.width, state.height, &state);
    wayDebug(2, LogErrorLocation, "windowThread done calculatePixelBufferSize\n");
    state.image = NULL;
    state.imageSize = 0;
    state.maxBuffers = MAXBUFFERS;
    state.wlDisplay = NULL;
    state.wlRegistry = NULL;
    state.wlShm = NULL;
    state.wlCompositor = NULL;
    state.wlDataDeviceManager = NULL;
    state.wlSeat = NULL;
    state.wlDataDevice = NULL;
    state.xdgWmBase = NULL;
    state.zxdgDecorationManagerV1 = NULL;
    wayDebug(2, LogErrorLocation, "windowThread calling wl_display_connect\n");
    state.wlDisplay = wl_display_connect(NULL);
    wayDebug(2, LogErrorLocation, "windowThread done wl_display_connect\n");
    wayDebug(2, LogErrorLocation, "windowThread calling wl_display_get_registry\n");
    state.wlRegistry = wl_display_get_registry(state.wlDisplay);
    wayDebug(2, LogErrorLocation, "windowThread done wl_display_get_registry\n");
    state.xkbContext = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    wayDebug(2, LogErrorLocation, "windowThread calling allocateKeyBuff\n");
    allocateKeyBuff(&state, 256);
    wayDebug(2, LogErrorLocation, "windowThread done allocateKeyBuff\n");
    state.wlDataDevice = NULL;
    state.pasteActive = false;
    state.ddOfferedTextPlain = false;
    populateYOffsetMap(&state);

    wayDebug(1, LogErrorLocation, "windowThread initial state setup done\n");
    /*--------------------------------------------------------------------------
    **  Populate the greyscale gamma corection table for our desired gamma
    **------------------------------------------------------------------------*/
    float exponent = 1.0 / GAMMA;
    float base = 0;
    for (int ndx = 0; ndx < 256; ndx++)
        {
        base = ndx / 255.0;
        state.gammaTable[ndx] = (unsigned char)(powf(base, exponent) * 255.0);
        }

    wayDebug(3, LogErrorLocation, "Our generated gamma table is:\n");
    for (int ndx =0; ndx <256; ndx++)
        {
        wayDebug(3, LogErrorLocation, "    entry %d has value %d\n", ndx, state.gammaTable[ndx]);
        }

    /*--------------------------------------------------------------------------
    **  Initial setup of the shared memory frame buffer cache and buffers
    **  as empty.
    **------------------------------------------------------------------------*/
    wayDebug(1, LogErrorLocation, "About to initialize frame buffer cache structures.\n");

    for (int n = 0; n < state.maxBuffers; n++)
        {
        state.buffers[n].frameBuffer = NULL;
        state.buffers[n].framePixels = NULL;
        state.buffers[n].pixelBufferSize = 0;
        state.buffers[n].frameBufferAvailable = true;
        }
    
    /*--------------------------------------------------------------------------
    **  Setup the font face we need for character output
    **  First just initialize the structures
    **------------------------------------------------------------------------*/

    FT_Error error = FT_Init_FreeType(&state.library);
    if (error != FT_Err_Ok)
        {
        logDtError(LogErrorLocation, "Error initializing Freetype library %d\n", error);
        return (void *)0;
        }

    for (int ndx = 0; ndx < MAXFONTS; ndx++)
        {
        initDtCyberFont(&state, ndx);
        }
    wayDebug(1, LogErrorLocation, "DtCyberFont structure initialized.\n");

    /*--------------------------------------------------------------------------
    **  Setup the font details we need for font number the desired fonts.
    **------------------------------------------------------------------------*/

    wayDebug(1, LogErrorLocation, "Loading font details for font %d.\n", FontNdxSmall);
    if (loadDtCyberFont(&state, FontNdxSmall, fontName, fontSmall))
        {
        wayDebug(1, LogErrorLocation, "Successfully loaded font %d.\n", FontNdxSmall);
        }
    else
        {
        wayDebug(1, LogErrorLocation, "Failed loading font %d.\n", FontNdxSmall);
        if (state.library != NULL) FT_Done_FreeType(state.library);
        return (void *)0;
        }
    wayDebug(1, LogErrorLocation, "Loading font details for font %d.\n", FontNdxMedium);
    if (loadDtCyberFont(&state, FontNdxMedium, fontName, fontMedium))
        {
        wayDebug(1, LogErrorLocation, "Successfully loaded font %d.\n", FontNdxMedium);
        }
    else
        {
        wayDebug(1, LogErrorLocation, "Failed loading font %d.\n", FontNdxMedium);
        if (state.library != NULL) FT_Done_FreeType(state.library);
        return (void *)0;
        }
    wayDebug(1, LogErrorLocation, "Loading font details for font %d.\n", FontNdxLarge);
    if (loadDtCyberFont(&state, FontNdxLarge, fontName, fontLarge))
        {
        wayDebug(1, LogErrorLocation, "Successfully loaded font %d.\n", FontNdxLarge);
        }
    else
        {
        wayDebug(1, LogErrorLocation, "Failed loading font %d.\n", FontNdxLarge);
        if (state.library != NULL) FT_Done_FreeType(state.library);
        return (void *)0;
        }
    state.currFontNdx = 0;
    state.currFont = &state.fonts[state.currFontNdx];

    /*
    **  Window thread loop.
    */
    isMeta = FALSE;

    /*--------------------------------------------------------------------------
    **  Initial pen position in 26.6 (fractional pixel) cartesian space
    **  coordinates. Start at (0, line height) relative to the upper left
    **  corner of the Wayland surface.
    **------------------------------------------------------------------------*/
    state.pen.x = 0;
    state.pen.y = 0;
    wayDebug(1, LogErrorLocation, "Initial pen position x = %d, y=%d.\n",
        (state.pen.x >> 6), (state.pen.y >> 6));

    /*--------------------------------------------------------------------------
    **  General Wayland initialization
    **
    **  First set up global object interface pointers.
    **------------------------------------------------------------------------*/

    wayDebug(1, LogErrorLocation, "windowThread starting Wayland initialization\n");
    wl_registry_add_listener(state.wlRegistry, &wlRegistryListener, &state);
    wl_display_roundtrip(state.wlDisplay);

    /*--------------------------------------------------------------------------
    **  Now set up the drawing surface for output.
    **------------------------------------------------------------------------*/
    state.wlSurface = wl_compositor_create_surface(state.wlCompositor);
    state.xdgSurface = xdg_wm_base_get_xdg_surface(state.xdgWmBase, state.wlSurface);
    xdg_surface_add_listener(state.xdgSurface, &xdgSurfaceListener, &state);
    state.xdgToplevel = xdg_surface_get_toplevel(state.xdgSurface);
    xdg_toplevel_add_listener(state.xdgToplevel, &xdgToplevelListener, &state);

    /*--------------------------------------------------------------------------
    **  Set up the compositor decoration for our window if we have a manager.
    **------------------------------------------------------------------------*/
    if (state.zxdgDecorationManagerV1 != NULL)
        {
        state.zxdgToplevelDecorationV1 = zxdg_decoration_manager_v1_get_toplevel_decoration(
            state.zxdgDecorationManagerV1, state.xdgToplevel);
        zxdg_toplevel_decoration_v1_add_listener(state.zxdgToplevelDecorationV1,
            &zxdgToplevelDecorationListener, &state);
        }

    windowTitle[0] = '\0';
    strcat(windowTitle, displayName);
    strcat(windowTitle, " - " DtCyberVersion);
    strcat(windowTitle, " - " DtCyberBuildDate);
    xdg_toplevel_set_title(state.xdgToplevel, windowTitle);

    /*--------------------------------------------------------------------------
    **  Set up the pointer cursor plumbing.
    **  We make a number of simple assumptions here:
    **    1) We just use the default cursor theme.
    **    2) We have hard wired a cursor image pixel size of 14 pixels.
    **    3) We only load the "left_ptr" cursor image and use that wherever
    **       we are in the surface.
    **------------------------------------------------------------------------*/
    state.cursorTheme = wl_cursor_theme_load(NULL, 14, state.wlShm);
    state.cursor = wl_cursor_theme_get_cursor(state.cursorTheme, "left_ptr");
    state.cursorImage = state.cursor->images[0];
    state.cursorBuffer = wl_cursor_image_get_buffer(state.cursorImage);
    state.cursorSurface = wl_compositor_create_surface(state.wlCompositor);
    wl_surface_attach(state.cursorSurface, state.cursorBuffer, 0, 0);
    wl_surface_commit(state.cursorSurface);

    /*--------------------------------------------------------------------------
    **  Push the configuration to the compositor.
    **------------------------------------------------------------------------*/
    wl_surface_commit(state.wlSurface);
    wl_display_roundtrip(state.wlDisplay);

    /*--------------------------------------------------------------------------
    **  Initiate and display the first frame. First flush any key presses that
    **  have carried in. We appear to get the return character from launching
    **  the program turn up in our input stream.
    **------------------------------------------------------------------------*/
    while (!isKeyBufEmpty(&state))
        {
        uint32_t discard = getQueuedKey(&state);
        }

    /*--------------------------------------------------------------------------
    **  Prepare the new frame image, as a blank screen.
    **------------------------------------------------------------------------*/
    wayDebug(1, LogErrorLocation, "windowThread painting first screen\n");
    drawText(&state);
    if (state.image == NULL)
        {
        logDtError(LogErrorLocation, "Unable to update the frame image buffer, aborting.\n");
        return (void *)0;
        }

    /*--------------------------------------------------------------------------
    **  Start the frame refresh processing logic.
    **------------------------------------------------------------------------*/
    int n = populateFrameBuffer(&state);
    if (n < 0) return (void *)0;
    struct wl_callback *cb = wl_surface_frame(state.wlSurface);
    wl_callback_add_listener(cb, &wlSurfaceFrameListener, &state);
    wl_surface_attach(state.wlSurface, state.buffers[n].frameBuffer, 0, 0);
    wl_surface_damage_buffer(state.wlSurface, 0, 0, state.width, state.height);
    wl_surface_commit(state.wlSurface);

    /*--------------------------------------------------------------------------
    **  Dispatch Wayland events until we get an error or window close request.
    **------------------------------------------------------------------------*/

    wayDebug(1, LogErrorLocation, "windowThread at processing loop\n");
    while (wl_display_dispatch(state.wlDisplay) && displayActive)
        {
        /*----------------------------------------------------------------------
        **  Deliberately empty. Processing happens in event handling routines.
        **  The "main loop" is driven by the surface frame handling routine.
        **--------------------------------------------------------------------*/
        }

    /*--------------------------------------------------------------------------
    **  Clean up keyboard and frame buffers.
    **------------------------------------------------------------------------*/

    releaseKeyBuff(&state);
    if (state.image != NULL)
        {
        wayDebug(1, LogErrorLocation, "Freeing the image pixel buffer space.\n");
        free((void *)state.image);
        state.image = NULL;
        }
    for (int n = 0; n < state.maxBuffers; n++)
        {
        if (state.buffers[n].framePixels != NULL)
            {
            wayDebug(1, LogErrorLocation, "Unmapping the pixel buffer for buffer %d\n", n);
            munmap(state.buffers[n].framePixels, state.buffers[n].pixelBufferSize);
            state.buffers[n].framePixels = NULL;
            }
        if (state.buffers[n].frameBuffer != NULL)
            {
            wayDebug(1, LogErrorLocation, "Destroying the frame buffer for slot %d\n", n);
            wl_buffer_destroy(state.buffers[n].frameBuffer);
            state.buffers[n].frameBuffer = NULL;
            }
        state.buffers[n].frameBufferAvailable = false;
        }
    wayDebug(1, LogErrorLocation, "Buffer cache cleanup completed.\n");

    /*--------------------------------------------------------------------------
    **  Clean up font handling state.
    **------------------------------------------------------------------------*/
    for (int ndx = 0; ndx < MAXFONTS; ndx++)
        {
        clearDtCyberFont(&state, ndx);
        }
    if (state.library != NULL) FT_Done_FreeType(state.library);

    /*--------------------------------------------------------------------------
    **  We need additional cleanup of the Wayland state here.
    **------------------------------------------------------------------------*/
    pthread_mutex_destroy(&mutexDisplay);
    pthread_exit(NULL);
    }

/*---------------------------  End Of File  ------------------------------*/
