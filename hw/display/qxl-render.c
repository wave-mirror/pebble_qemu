/*
 * qxl local rendering (aka display on sdl/vnc)
 *
 * Copyright (C) 2010 Red Hat, Inc.
 *
 * maintained by Gerd Hoffmann <kraxel@redhat.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 or
 * (at your option) version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qxl.h"
#include "trace.h"

static void qxl_blit(PCIQXLDevice *qxl, QXLRect *rect)
{
    DisplaySurface *surface = qemu_console_surface(qxl->vga.con);
    uint8_t *dst = surface_data(surface);
    uint8_t *src;
    int len, i;

    if (is_buffer_shared(surface)) {
        return;
    }
    trace_qxl_render_blit(qxl->guest_primary.qxl_stride,
            rect->left, rect->right, rect->top, rect->bottom);
    src = qxl->guest_primary.data;
    if (qxl->guest_primary.qxl_stride < 0) {
        /* qxl surface is upside down, walk src scanlines
         * in reverse order to flip it */
        src += (qxl->guest_primary.surface.height - rect->top - 1) *
            qxl->guest_primary.abs_stride;
    } else {
        src += rect->top * qxl->guest_primary.abs_stride;
    }
    dst += rect->top  * qxl->guest_primary.abs_stride;
    src += rect->left * qxl->guest_primary.bytes_pp;
    dst += rect->left * qxl->guest_primary.bytes_pp;
    len  = (rect->right - rect->left) * qxl->guest_primary.bytes_pp;

    for (i = rect->top; i < rect->bottom; i++) {
        memcpy(dst, src, len);
        dst += qxl->guest_primary.abs_stride;
        src += qxl->guest_primary.qxl_stride;
    }
}

void qxl_render_resize(PCIQXLDevice *qxl)
{
    QXLSurfaceCreate *sc = &qxl->guest_primary.surface;

    qxl->guest_primary.qxl_stride = sc->stride;
    qxl->guest_primary.abs_stride = abs(sc->stride);
    qxl->guest_primary.resized++;
    switch (sc->format) {
    case SPICE_SURFACE_FMT_16_555:
        qxl->guest_primary.bytes_pp = 2;
        qxl->guest_primary.bits_pp = 15;
        break;
    case SPICE_SURFACE_FMT_16_565:
        qxl->guest_primary.bytes_pp = 2;
        qxl->guest_primary.bits_pp = 16;
        break;
    case SPICE_SURFACE_FMT_32_xRGB:
    case SPICE_SURFACE_FMT_32_ARGB:
        qxl->guest_primary.bytes_pp = 4;
        qxl->guest_primary.bits_pp = 32;
        break;
    default:
        fprintf(stderr, "%s: unhandled format: %x\n", __FUNCTION__,
                qxl->guest_primary.surface.format);
        qxl->guest_primary.bytes_pp = 4;
        qxl->guest_primary.bits_pp = 32;
        break;
    }
}

static void qxl_set_rect_to_surface(PCIQXLDevice *qxl, QXLRect *area)
{
    area->left   = 0;
    area->right  = qxl->guest_primary.surface.width;
    area->top    = 0;
    area->bottom = qxl->guest_primary.surface.height;
}

static void qxl_render_update_area_unlocked(PCIQXLDevice *qxl)
{
    VGACommonState *vga = &qxl->vga;
    DisplaySurface *surface;
    int i;

    if (qxl->guest_primary.resized) {
        qxl->guest_primary.resized = 0;
        qxl->guest_primary.data = qxl_phys2virt(qxl,
                                                qxl->guest_primary.surface.mem,
                                                MEMSLOT_GROUP_GUEST);
        if (!qxl->guest_primary.data) {
            return;
        }
        qxl_set_rect_to_surface(qxl, &qxl->dirty[0]);
        qxl->num_dirty_rects = 1;
        trace_qxl_render_guest_primary_resized(
               qxl->guest_primary.surface.width,
               qxl->guest_primary.surface.height,
               qxl->guest_primary.qxl_stride,
               qxl->guest_primary.bytes_pp,
               qxl->guest_primary.bits_pp);
        if (qxl->guest_primary.qxl_stride > 0) {
            surface = qemu_create_displaysurface_from
                (qxl->guest_primary.surface.width,
                 qxl->guest_primary.surface.height,
                 qxl->guest_primary.bits_pp,
                 qxl->guest_primary.abs_stride,
                 qxl->guest_primary.data,
                 false);
        } else {
            surface = qemu_create_displaysurface
                (qxl->guest_primary.surface.width,
                 qxl->guest_primary.surface.height);
        }
        dpy_gfx_replace_surface(vga->con, surface);
    }

    if (!qxl->guest_primary.data) {
        return;
    }
    for (i = 0; i < qxl->num_dirty_rects; i++) {
        if (qemu_spice_rect_is_empty(qxl->dirty+i)) {
            break;
        }
        if (qxl->dirty[i].left > qxl->dirty[i].right ||
            qxl->dirty[i].top > qxl->dirty[i].bottom ||
            qxl->dirty[i].right > qxl->guest_primary.surface.width ||
            qxl->dirty[i].bottom > qxl->guest_primary.surface.height) {
            continue;
        }
        qxl_blit(qxl, qxl->dirty+i);
        dpy_gfx_update(vga->con,
                       qxl->dirty[i].left, qxl->dirty[i].top,
                       qxl->dirty[i].right - qxl->dirty[i].left,
                       qxl->dirty[i].bottom - qxl->dirty[i].top);
    }
    qxl->num_dirty_rects = 0;
}

/*
 * use ssd.lock to protect render_update_cookie_num.
 * qxl_render_update is called by io thread or vcpu thread, and the completion
 * callbacks are called by spice_server thread, defering to bh called from the
 * io thread.
 */
void qxl_render_update(PCIQXLDevice *qxl)
{
    QXLCookie *cookie;

    qemu_mutex_lock(&qxl->ssd.lock);

    if (!runstate_is_running() || !qxl->guest_primary.commands) {
        qxl_render_update_area_unlocked(qxl);
        qemu_mutex_unlock(&qxl->ssd.lock);
        return;
    }

    qxl->guest_primary.commands = 0;
    qxl->render_update_cookie_num++;
    qemu_mutex_unlock(&qxl->ssd.lock);
    cookie = qxl_cookie_new(QXL_COOKIE_TYPE_RENDER_UPDATE_AREA,
                            0);
    qxl_set_rect_to_surface(qxl, &cookie->u.render.area);
    qxl_spice_update_area(qxl, 0, &cookie->u.render.area, NULL,
                          0, 1 /* clear_dirty_region */, QXL_ASYNC, cookie);
}

void qxl_render_update_area_bh(void *opaque)
{
    PCIQXLDevice *qxl = opaque;

    qemu_mutex_lock(&qxl->ssd.lock);
    qxl_render_update_area_unlocked(qxl);
    qemu_mutex_unlock(&qxl->ssd.lock);
}

void qxl_render_update_area_done(PCIQXLDevice *qxl, QXLCookie *cookie)
{
    qemu_mutex_lock(&qxl->ssd.lock);
    trace_qxl_render_update_area_done(cookie);
    qemu_bh_schedule(qxl->update_area_bh);
    qxl->render_update_cookie_num--;
    qemu_mutex_unlock(&qxl->ssd.lock);
    g_free(cookie);
}

static QEMUCursor *qxl_cursor(PCIQXLDevice *qxl, QXLCursor *cursor)
{
    QEMUCursor *c;
    uint8_t *image, *mask;
    size_t size;

    c = cursor_alloc(cursor->header.width, cursor->header.height);
    c->hot_x = cursor->header.hot_spot_x;
    c->hot_y = cursor->header.hot_spot_y;
    switch (cursor->header.type) {
    case SPICE_CURSOR_TYPE_ALPHA:
        size = sizeof(uint32_t) * cursor->header.width * cursor->header.height;
        memcpy(c->data, cursor->chunk.data, size);
        if (qxl->debug > 2) {
            cursor_print_ascii_art(c, "qxl/alpha");
        }
        break;
    case SPICE_CURSOR_TYPE_MONO:
        mask  = cursor->chunk.data;
        image = mask + cursor_get_mono_bpl(c) * c->width;
        cursor_set_mono(c, 0xffffff, 0x000000, image, 1, mask);
        if (qxl->debug > 2) {
            cursor_print_ascii_art(c, "qxl/mono");
        }
        break;
    default:
        fprintf(stderr, "%s: not implemented: type %d\n",
                __FUNCTION__, cursor->header.type);
        goto fail;
    }
    return c;

fail:
    cursor_put(c);
    return NULL;
}


/* called from spice server thread context only */
int qxl_render_cursor(PCIQXLDevice *qxl, QXLCommandExt *ext)
{
    QXLCursorCmd *cmd = qxl_phys2virt(qxl, ext->cmd.data, ext->group_id);
    QXLCursor *cursor;
    QEMUCursor *c;

    if (!cmd) {
        return 1;
    }

    if (!dpy_cursor_define_supported(qxl->vga.con)) {
        return 0;
    }

    if (qxl->debug > 1 && cmd->type != QXL_CURSOR_MOVE) {
        fprintf(stderr, "%s", __FUNCTION__);
        qxl_log_cmd_cursor(qxl, cmd, ext->group_id);
        fprintf(stderr, "\n");
    }
    switch (cmd->type) {
    case QXL_CURSOR_SET:
        cursor = qxl_phys2virt(qxl, cmd->u.set.shape, ext->group_id);
        if (!cursor) {
            return 1;
        }
        if (cursor->chunk.data_size != cursor->data_size) {
            fprintf(stderr, "%s: multiple chunks\n", __FUNCTION__);
            return 1;
        }
        c = qxl_cursor(qxl, cursor);
        if (c == NULL) {
            c = cursor_builtin_left_ptr();
        }
        qemu_mutex_lock(&qxl->ssd.lock);
        if (qxl->ssd.cursor) {
            cursor_put(qxl->ssd.cursor);
        }
        qxl->ssd.cursor = c;
        qxl->ssd.mouse_x = cmd->u.set.position.x;
        qxl->ssd.mouse_y = cmd->u.set.position.y;
        qemu_mutex_unlock(&qxl->ssd.lock);
        break;
    case QXL_CURSOR_MOVE:
        qemu_mutex_lock(&qxl->ssd.lock);
        qxl->ssd.mouse_x = cmd->u.position.x;
        qxl->ssd.mouse_y = cmd->u.position.y;
        qemu_mutex_unlock(&qxl->ssd.lock);
        break;
    }
    return 0;
}
