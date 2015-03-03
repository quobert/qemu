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

void qio_buffer_reserve(QIOBuffer *buffer, size_t len)
{
    if ((buffer->capacity - buffer->offset) < len) {
        buffer->capacity += (len + 1024);
        buffer->buffer = g_realloc(buffer->buffer, buffer->capacity);
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
    g_free(buffer->buffer);
    buffer->offset = 0;
    buffer->capacity = 0;
    buffer->buffer = NULL;
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
