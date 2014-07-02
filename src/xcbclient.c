/*
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *
 * From original file xlibclient.c:
 *   Paulo Zanoni <pzanoni@mandriva.com>
 *   Tuan Bui <tuanbui918@gmail.com>
 *   Colin Cornaby <colin.cornaby@mac.com>
 *   Timothy Fleck <tim.cs.pdx@gmail.com>
 *   Colin Hill <colin.james.hill@gmail.com>
 *   Weseung Hwang <weseung@gmail.com>
 *   Nathaniel Way <nathanielcw@hotmail.com>
 *
 * From ported file xcbclient.c:
 *   Laércio de Sousa <laerciosousa@sme-mogidascruzes.sp.gov.br>
 */

#include <stdlib.h>

#include <sys/ipc.h>
#include <sys/shm.h>

#include <X11/Xlib-xcb.h>
#include <X11/XKBlib.h>

#include <xcb/xcb_aux.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_image.h>
#include <xcb/shm.h>
#include <xcb/xkb.h>

#include <xorg-server.h>
#include <xf86.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "client.h"

#include "nested_input.h"

struct NestedClientPrivate {
    Display *display;
    xcb_connection_t *connection;
    int screenNumber;
    xcb_visualtype_t *visual;
    xcb_screen_t *screen;
    xcb_window_t rootWindow;
    xcb_window_t window;
    xcb_image_t *img;
    xcb_gcontext_t gc;
    Bool usingShm;
    xcb_shm_segment_info_t shminfo;
    int scrnIndex; /* stored only for xf86DrvMsg usage */
    DeviceIntPtr dev; // The pointer to the input device.  Passed back to the
                      // input driver when posting input events.
};

/* Checks if a display is open */
Bool
NestedClientCheckDisplay(char *displayName) {
    xcb_connection_t *c;

    c = xcb_connect(displayName, NULL);
    
    if (xcb_connection_has_error(c)) {
        return FALSE;
    }
    else {
        xcb_disconnect(c);
        return TRUE;
    }
}

Bool
NestedClientValidDepth(int depth) {
    /* XXX: implement! */
    return TRUE;
}

static Bool
NestedClientTryXShm(NestedClientPrivatePtr pPriv, int scrnIndex, int width, int height, int depth) {
    const xcb_query_extension_reply_t *shm_rep;
    xcb_generic_error_t *e;
    xcb_shm_query_version_cookie_t shm_version_c;
    xcb_shm_query_version_reply_t *shm_version_r;

    shm_rep = xcb_get_extension_data(pPriv->connection, &xcb_shm_id);

    if (!shm_rep || !shm_rep->present) {
        xf86DrvMsg(scrnIndex, X_INFO, "XShm extension query failed. Dropping XShm support.\n");
        return FALSE;
    }

    shm_version_c = xcb_shm_query_version(pPriv->connection);
    shm_version_r = xcb_shm_query_version_reply(pPriv->connection,
                                                shm_version_c, &e);

    if (e) {
        xf86DrvMsg(scrnIndex, X_INFO, "XShm extension version query failed. Dropping XShm support.\n");
        free(e);
        return FALSE;
    }
    else {
        xf86DrvMsg(scrnIndex, X_INFO,
                   "XShm extension version %d.%d %s shared pixmaps\n",
                   shm_version_r->major_version,
                   shm_version_r->minor_version,
                   shm_version_r->shared_pixmaps ? "with" : "without");

        free(shm_version_r);
    }

    pPriv->img = xcb_image_create_native(pPriv->connection,
                                         width,
                                         height,
                                         XCB_IMAGE_FORMAT_Z_PIXMAP,
                                         depth,
                                         NULL,
                                         ~0,
                                         NULL);

    if (!pPriv->img) {
        xf86DrvMsg(scrnIndex, X_ERROR, "xcb_image_create_native failed. Dropping XShm support.\n");
        return FALSE;
    }

    /* XXX: change the 0777 mask? */
    pPriv->shminfo.shmid = shmget(IPC_PRIVATE,
                                  pPriv->img->stride * pPriv->img->height,
                                  IPC_CREAT | 0777);

    if (pPriv->shminfo.shmid == -1) {
        xf86DrvMsg(scrnIndex, X_ERROR, "shmget failed.  Dropping XShm support.\n");
        xcb_image_destroy(pPriv->img);
        return FALSE;
    }

    pPriv->img->data = shmat(pPriv->shminfo.shmid, 0, 0);
    pPriv->shminfo.shmaddr = pPriv->img->data;

    if (pPriv->shminfo.shmaddr == (uint8_t *) -1) {
        xf86DrvMsg(scrnIndex, X_ERROR, "shmaddr failed.  Dropping XShm support.\n");
        xcb_image_destroy(pPriv->img);
        return FALSE;
    }

    pPriv->shminfo.shmseg = xcb_generate_id(pPriv->connection);
    xcb_shm_attach(pPriv->connection,
                   pPriv->shminfo.shmseg,
                   pPriv->shminfo.shmid,
                   FALSE);
    pPriv->usingShm = TRUE;

    return TRUE;
}

NestedClientPrivatePtr
NestedClientCreateScreen(int scrnIndex,
                         char *displayName,
                         int width,
                         int height,
                         int originX,
                         int originY,
                         int depth,
                         int bitsPerPixel,
                         uint32_t *retRedMask,
                         uint32_t *retGreenMask,
                         uint32_t *retBlueMask) {
    NestedClientPrivatePtr pPriv;
    const xcb_query_extension_reply_t *xkb_rep;
    xcb_size_hints_t sizeHints;
    char windowTitle[32];
    uint32_t attr;

    attr = XCB_EVENT_MASK_EXPOSURE
           | XCB_EVENT_MASK_POINTER_MOTION
           | XCB_EVENT_MASK_ENTER_WINDOW
           | XCB_EVENT_MASK_LEAVE_WINDOW
           | XCB_EVENT_MASK_BUTTON_PRESS
           | XCB_EVENT_MASK_BUTTON_RELEASE
           | XCB_EVENT_MASK_KEY_PRESS
           | XCB_EVENT_MASK_KEY_RELEASE;

    pPriv = malloc(sizeof(struct NestedClientPrivate));
    pPriv->scrnIndex = scrnIndex;
    
    /* XXX: Get rid of pPriv->display as soon as we can
     * port all XKB related calls to XCB. */
    pPriv->display = XOpenDisplay(displayName);
    pPriv->screenNumber = DefaultScreen(pPriv->display);
    pPriv->connection = XGetXCBConnection(pPriv->display);
    XSetEventQueueOwner(pPriv->display, XCBOwnsEventQueue);

    if (xcb_connection_has_error(pPriv->connection))
        return NULL;

    xkb_rep = xcb_get_extension_data(pPriv->connection, &xcb_xkb_id);

    if (!xkb_rep || !xkb_rep->present) {
        xf86DrvMsg(pPriv->scrnIndex, X_ERROR, "Host X server does not support the XKEYBOARD extension.\n");
        XCloseDisplay(pPriv->display);
        return NULL;
    }

    pPriv->screen = xcb_aux_get_screen(pPriv->connection, pPriv->screenNumber);
    pPriv->visual = xcb_aux_find_visual_by_id(pPriv->screen,
                                              pPriv->screen->root_visual);
    pPriv->rootWindow = pPriv->screen->root;
    pPriv->gc = xcb_generate_id(pPriv->connection);
    xcb_create_gc(pPriv->connection,
                  pPriv->gc,
                  pPriv->rootWindow,
                  0, NULL);

    pPriv->window = xcb_generate_id(pPriv->connection);
    xcb_create_window(pPriv->connection,
                      XCB_COPY_FROM_PARENT,
                      pPriv->window,
                      pPriv->rootWindow,
                      0, 0, 100, 100, /* Will move/resize */
                      0,
                      XCB_WINDOW_CLASS_COPY_FROM_PARENT,
                      XCB_COPY_FROM_PARENT,
                      XCB_CW_EVENT_MASK,
                      &attr);

    sizeHints.flags = XCB_ICCCM_SIZE_HINT_P_POSITION
                      | XCB_ICCCM_SIZE_HINT_P_SIZE
                      | XCB_ICCCM_SIZE_HINT_P_MIN_SIZE
                      | XCB_ICCCM_SIZE_HINT_P_MAX_SIZE;
    sizeHints.min_width = width;
    sizeHints.max_width = width;
    sizeHints.min_height = height;
    sizeHints.max_height = height;
    xcb_icccm_set_wm_normal_hints(pPriv->connection,
                                  pPriv->window,
                                  &sizeHints);

    snprintf(windowTitle, sizeof(windowTitle), "Screen %d", scrnIndex);
    xcb_icccm_set_wm_name(pPriv->connection,
                          pPriv->window,
                          XCB_ATOM_STRING,
                          8,
                          strlen(windowTitle),
                          windowTitle);

    {
        uint32_t mask = XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
        uint32_t values[2] = {width, height};
        xcb_configure_window(pPriv->connection, pPriv->window, mask, values);
    }

    xcb_map_window(pPriv->connection, pPriv->window);

    {
        uint32_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
        uint32_t values[2] = {originX, originY};
        xcb_configure_window(pPriv->connection, pPriv->window, mask, values);
    }

    if (!NestedClientTryXShm(pPriv, scrnIndex, width, height, depth)) {
        pPriv->img = xcb_image_create_native(pPriv->connection,
                                width,
                                height,
                                XCB_IMAGE_FORMAT_Z_PIXMAP,
                                depth,
                                NULL,
                                ~0,
                                NULL);

        if (!pPriv->img)
            return NULL;

        pPriv->img->data = malloc(pPriv->img->stride * pPriv->img->height);
        pPriv->usingShm = FALSE;
    }

    if (!pPriv->img->data)
        return NULL;

    NestedClientHideCursor(pPriv); /* Hide cursor */

#if 1
xf86DrvMsg(scrnIndex, X_INFO, "width: %d\n", pPriv->img->width);
xf86DrvMsg(scrnIndex, X_INFO, "height: %d\n", pPriv->img->height);
xf86DrvMsg(scrnIndex, X_INFO, "depth: %d\n", pPriv->img->depth);
xf86DrvMsg(scrnIndex, X_INFO, "bpp: %d\n", pPriv->img->bpp);
xf86DrvMsg(scrnIndex, X_INFO, "red_mask: 0x%x\n", pPriv->visual->red_mask);
xf86DrvMsg(scrnIndex, X_INFO, "gre_mask: 0x%x\n", pPriv->visual->green_mask);
xf86DrvMsg(scrnIndex, X_INFO, "blu_mask: 0x%x\n", pPriv->visual->blue_mask);
#endif

    *retRedMask = pPriv->visual->red_mask;
    *retGreenMask = pPriv->visual->green_mask;
    *retBlueMask = pPriv->visual->blue_mask;

    xcb_generic_event_t *ev;

    while ((ev = xcb_wait_for_event(pPriv->connection))) {
        switch (ev->response_type & ~0x80) {
        case XCB_EXPOSE:
            break;
        }
    }
   
    pPriv->dev = (DeviceIntPtr)NULL;
 
    return pPriv;
}

void NestedClientHideCursor(NestedClientPrivatePtr pPriv) {
    xcb_cursor_t emptyCursor;
    xcb_pixmap_t emptyPixmap;

    emptyPixmap = xcb_generate_id(pPriv->connection);
    xcb_create_pixmap(pPriv->connection,
                      1,
                      emptyPixmap,
                      pPriv->rootWindow,
                      1, 1);

    emptyCursor = xcb_generate_id(pPriv->connection);
    xcb_create_cursor(pPriv->connection,
                      emptyCursor,
                      emptyPixmap, emptyPixmap,
                      0, 0, 0,
                      0, 0, 0,
                      1, 1);

    xcb_change_window_attributes(pPriv->connection,
                                 pPriv->window,
                                 XCB_CW_CURSOR,
                                 &emptyCursor);

    xcb_free_pixmap(pPriv->connection, emptyPixmap);
}

char *
NestedClientGetFrameBuffer(NestedClientPrivatePtr pPriv) {
    return pPriv->img->data;
}

void
NestedClientUpdateScreen(NestedClientPrivatePtr pPriv, int16_t x1,
                         int16_t y1, int16_t x2, int16_t y2) {
    if (pPriv->usingShm) {
        xcb_image_shm_put(pPriv->connection, pPriv->window,
                          pPriv->gc, pPriv->img,
                          pPriv->shminfo,
                          x1, y1, x1, y1, x2 - x1, y2 - y1, FALSE);
    } else {
        xcb_image_put(pPriv->connection, pPriv->window, pPriv->gc, pPriv->img,
                      x1, y1, 0);
    }

    xcb_aux_sync(pPriv->connection);
}

void
NestedClientCheckEvents(NestedClientPrivatePtr pPriv) {
    xcb_generic_event_t *ev;
    xcb_expose_event_t *xev;
    xcb_motion_notify_event_t *mev;
    xcb_button_press_event_t *bev;
    xcb_key_press_event_t *kev;

    while ((ev = xcb_poll_for_event(pPriv->connection))) {
        switch (ev->response_type & ~0x80) {
        case XCB_EXPOSE:
            xev = (xcb_expose_event_t *)ev;
            NestedClientUpdateScreen(pPriv,
                                     xev->x,
                                     xev->y,
                                     xev->x + xev->width,
                                     xev->y + xev->height);
            break;
        case XCB_MOTION_NOTIFY:
            if (!pPriv->dev) {
                xf86DrvMsg(pPriv->scrnIndex, X_INFO, "Input device is not yet initialized, ignoring input.\n");
                break;
            }

            mev = (xcb_motion_notify_event_t *)ev;
            NestedInputPostMouseMotionEvent(pPriv->dev,
                                            mev->event_x,
                                            mev->event_y);
            break;
        case XCB_KEY_PRESS:
            if (!pPriv->dev) {
                xf86DrvMsg(pPriv->scrnIndex, X_INFO, "Input device is not yet initialized, ignoring input.\n");
                break;
            }

            kev = (xcb_key_press_event_t *)ev;
            NestedInputPostKeyboardEvent(pPriv->dev, kev->detail, TRUE);
            break;
        case XCB_KEY_RELEASE:
            if (!pPriv->dev) {
                xf86DrvMsg(pPriv->scrnIndex, X_INFO, "Input device is not yet initialized, ignoring input.\n");
                break;
            }

            kev = (xcb_key_press_event_t *)ev;
            NestedInputPostKeyboardEvent(pPriv->dev, kev->detail, FALSE);
            break;
        case XCB_BUTTON_PRESS:
            if (!pPriv->dev) {
                xf86DrvMsg(pPriv->scrnIndex, X_INFO, "Input device is not yet initialized, ignoring input.\n");
                break;
            }

            bev = (xcb_button_press_event_t *)ev;
            NestedInputPostButtonEvent(pPriv->dev, bev->detail, TRUE);
            break;
        case XCB_BUTTON_RELEASE:
            if (!pPriv->dev) {
                xf86DrvMsg(pPriv->scrnIndex, X_INFO, "Input device is not yet initialized, ignoring input.\n");
                break;
            }

            bev = (xcb_button_press_event_t *)ev;
            NestedInputPostButtonEvent(pPriv->dev, bev->detail, FALSE);
            break;
        }

        free(ev);
    }
}

void
NestedClientCloseScreen(NestedClientPrivatePtr pPriv) {
    if (pPriv->usingShm) {
        xcb_shm_detach(pPriv->connection, pPriv->shminfo.shmseg);
        shmdt(pPriv->shminfo.shmaddr);
    }

    xcb_image_destroy(pPriv->img);
    XCloseDisplay(pPriv->display);
}

void
NestedClientSetDevicePtr(NestedClientPrivatePtr pPriv, DeviceIntPtr dev) {
    pPriv->dev = dev;
}

int
NestedClientGetFileDescriptor(NestedClientPrivatePtr pPriv) {
    return xcb_get_file_descriptor(pPriv->connection);
}

Bool NestedClientGetKeyboardMappings(NestedClientPrivatePtr pPriv, KeySymsPtr keySyms, CARD8 *modmap, XkbControlsPtr ctrls) {
    int mapWidth;
    int min_keycode, max_keycode;
    int i, j;
    XkbDescPtr xkb;
    xcb_keysym_t *keymap;
    xcb_keycode_t *modifiermap;
    xcb_get_keyboard_mapping_cookie_t mapping_c;
    xcb_get_keyboard_mapping_reply_t *mapping_r;
    xcb_get_modifier_mapping_cookie_t modifier_c;
    xcb_get_modifier_mapping_reply_t *modifier_r;

    min_keycode = xcb_get_setup(pPriv->connection)->min_keycode;
    max_keycode = xcb_get_setup(pPriv->connection)->max_keycode;

    mapping_c = xcb_get_keyboard_mapping(pPriv->connection,
                                         min_keycode,
                                         max_keycode - min_keycode + 1);
    mapping_r = xcb_get_keyboard_mapping_reply(pPriv->connection,
                                               mapping_c,
                                               NULL);
    mapWidth = mapping_r->keysyms_per_keycode;
    keymap = xcb_get_keyboard_mapping_keysyms(mapping_r);
    free(mapping_r);

    modifier_c = xcb_get_modifier_mapping(pPriv->connection);
    modifier_r = xcb_get_modifier_mapping_reply(pPriv->connection,
                                                modifier_c,
                                                NULL);
    modifiermap = xcb_get_modifier_mapping_keycodes(modifier_r);
    memset(modmap, 0, sizeof(CARD8) * MAP_LENGTH);

    for (j = 0; j < 8; j++)
        for (i = 0; i < modifier_r->keycodes_per_modifier; i++) {
            CARD8 keycode;

            if ((keycode = modifiermap[j * modifier_r->keycodes_per_modifier + i]))
                modmap[keycode] |= 1 << j;
    }

    free(modifier_r);

    keySyms->minKeyCode = min_keycode;
    keySyms->maxKeyCode = max_keycode;
    keySyms->mapWidth = mapWidth;
    keySyms->map = (KeySym *)keymap;

    xkb = XkbGetKeyboard(pPriv->display, XkbGBN_AllComponentsMask, XkbUseCoreKbd);
    if (xkb == NULL || xkb->geom == NULL) {
        xf86DrvMsg(pPriv->scrnIndex, X_ERROR, "Couldn't get XKB keyboard.\n");
        free(keymap);
        return FALSE;
    }

    if(XkbGetControls(pPriv->display, XkbAllControlsMask, xkb) != Success) {
        xf86DrvMsg(pPriv->scrnIndex, X_ERROR, "Couldn't get XKB keyboard controls.\n");
        free(keymap);
        return FALSE;
    }

    memcpy(ctrls, xkb->ctrls, sizeof(XkbControlsRec));
    XkbFreeKeyboard(xkb, 0, False);
    return TRUE;
}
