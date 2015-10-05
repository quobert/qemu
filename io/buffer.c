/*
 * QEMU I/O buffers
 *
 * Copyright (c) 2015 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "io/buffer.h"
#include "trace.h"

#define QIO_BUFFER_MIN_INIT_SIZE     4096
#define QIO_BUFFER_MIN_SHRINK_SIZE  65536
/* define the factor alpha for the expentional smoothing
 * that is used in the average size calculation. a shift
 * of 7 results in an alpha of 1/2^7. */
#define QIO_BUFFER_AVG_SIZE_SHIFT       7

static size_t buf_req_size(QIOBuffer *buffer, size_t len)
{
    return MAX(QIO_BUFFER_MIN_INIT_SIZE,
               pow2ceil(buffer->offset + len));
}

static void buf_adj_size(QIOBuffer *buffer, size_t len)
{
    size_t old = buffer->capacity;
    buffer->capacity = buf_req_size(buffer, len);
    buffer->buffer = g_realloc(buffer->buffer, buffer->capacity);
    trace_qio_buffer_resize(buffer->name ?: "unnamed",
                            old, buffer->capacity);

    /* make it even harder for the buffer to shrink, reset average size
     * to currenty capacity if it is larger than the average. */
    buffer->avg_size = MAX(buffer->avg_size,
                           buffer->capacity << QIO_BUFFER_AVG_SIZE_SHIFT);
}

void qio_buffer_init(QIOBuffer *buffer, const char *name, ...)
{
    va_list ap;

    va_start(ap, name);
    buffer->name = g_strdup_vprintf(name, ap);
    va_end(ap);
}

static uint64_t get_buf_avg_size(QIOBuffer *buffer)
{
    return buffer->avg_size >> QIO_BUFFER_AVG_SIZE_SHIFT;
}

void qio_buffer_shrink(QIOBuffer *buffer)
 {
    size_t new;

    /* Calculate the average size of the buffer as
     * avg_size = avg_size * ( 1 - a ) + required_size * a
     * where a is 1 / 2 ^ QIO_BUFFER_AVG_SIZE_SHIFT. */
    buffer->avg_size *= (1 << QIO_BUFFER_AVG_SIZE_SHIFT) - 1;
    buffer->avg_size >>= QIO_BUFFER_AVG_SIZE_SHIFT;
    buffer->avg_size += buf_req_size(buffer, 0);

    /* And then only shrink if the average size of the buffer is much
     * too big, to avoid bumping up & down the buffers all the time.
     * realloc() isn't exactly cheap ...
     */
    new = buf_req_size(buffer, get_buf_avg_size(buffer));
    if (new < buffer->capacity >> 3 &&
        new >= QIO_BUFFER_MIN_SHRINK_SIZE) {
        buf_adj_size(buffer, get_buf_avg_size(buffer));
    }
}


void qio_buffer_reserve(QIOBuffer *buffer, size_t len)
{
    if ((buffer->capacity - buffer->offset) < len) {
        buf_adj_size(buffer, len);
    }
}

gboolean qio_buffer_empty(QIOBuffer *buffer)
{
    return buffer->offset == 0;
}

uint8_t *qio_buffer_end(QIOBuffer *buffer)
{
    return buffer->buffer + buffer->offset;
}

void qio_buffer_reset(QIOBuffer *buffer)
{
    buffer->offset = 0;
    qio_buffer_shrink(buffer);
}

void qio_buffer_free(QIOBuffer *buffer)
{
    trace_qio_buffer_free(buffer->name ?: "unnamed", buffer->capacity);
    g_free(buffer->buffer);
    g_free(buffer->name);
    buffer->offset = 0;
    buffer->capacity = 0;
    buffer->buffer = NULL;
    buffer->name = NULL;
}

void qio_buffer_append(QIOBuffer *buffer, const void *data, size_t len)
{
    memcpy(buffer->buffer + buffer->offset, data, len);
    buffer->offset += len;
}

void qio_buffer_advance(QIOBuffer *buffer, size_t len)
{
    memmove(buffer->buffer, buffer->buffer + len,
            (buffer->offset - len));
    buffer->offset -= len;
    qio_buffer_shrink(buffer);
}

void qio_buffer_move_empty(QIOBuffer *to, QIOBuffer *from)
{
    trace_qio_buffer_move_empty(to->name ?: "unnamed",
                                from->offset,
                                from->name ?: "unnamed");
    assert(to->offset == 0);

    g_free(to->buffer);
    to->offset = from->offset;
    to->capacity = from->capacity;
    to->buffer = from->buffer;

    from->offset = 0;
    from->capacity = 0;
    from->buffer = NULL;
}

void qio_buffer_move(QIOBuffer *to, QIOBuffer *from)
{
    if (to->offset == 0) {
        qio_buffer_move_empty(to, from);
        return;
    }

    trace_qio_buffer_move(to->name ?: "unnamed",
                          from->offset,
                          from->name ?: "unnamed");

    qio_buffer_reserve(to, from->offset);
    qio_buffer_append(to, from->buffer, from->offset);

    g_free(from->buffer);
    from->offset = 0;
    from->capacity = 0;
    from->buffer = NULL;
}
