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

void qio_buffer_init(QIOBuffer *buffer, const char *name, ...)
{
    va_list ap;

    va_start(ap, name);
    buffer->name = g_strdup_vprintf(name, ap);
    va_end(ap);
}

void qio_buffer_shrink(QIOBuffer *buffer)
{
    size_t old;

    /*
     * Only shrink in case the used size is *much* smaller than the
     * capacity, to avoid bumping up & down the buffers all the time.
     * realloc() isn't exactly cheap ...
     */
    if (buffer->offset < (buffer->capacity >> 3) &&
        buffer->capacity > QIO_BUFFER_MIN_SHRINK_SIZE) {
        return;
    }

    old = buffer->capacity;
    buffer->capacity = pow2ceil(buffer->offset);
    buffer->capacity = MAX(buffer->capacity, QIO_BUFFER_MIN_SHRINK_SIZE);
    buffer->buffer = g_realloc(buffer->buffer, buffer->capacity);
    trace_qio_buffer_resize(buffer->name ?: "unnamed",
                            old, buffer->capacity);
}

void qio_buffer_reserve(QIOBuffer *buffer, size_t len)
{
    size_t old;

    if ((buffer->capacity - buffer->offset) < len) {
        old = buffer->capacity;
        buffer->capacity = pow2ceil(buffer->offset + len);
        buffer->capacity = MAX(buffer->capacity, QIO_BUFFER_MIN_INIT_SIZE);
        buffer->buffer = g_realloc(buffer->buffer, buffer->capacity);
        trace_qio_buffer_resize(buffer->name ?: "unnamed",
                                old, buffer->capacity);
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
