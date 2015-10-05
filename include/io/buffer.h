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

#ifndef QIO_BUFFER_H__
#define QIO_BUFFER_H__

#include "qemu-common.h"

typedef struct QIOBuffer QIOBuffer;

/**
 * QIOBuffer:
 *
 * The QIOBuffer object provides a simple dynamically resizing
 * array, with separate tracking of capacity and usage. This
 * is typically useful when buffering I/O data.
 */

struct QIOBuffer {
    char *name;
    size_t capacity;
    size_t offset;
    uint64_t avg_size;
    uint8_t *buffer;
    size_t base_offs;
    uint8_t *base_ptr;
};

/**
 * qio_buffer_init:
 * @buffer: the buffer object
 * @name: buffer name
 *
 * Optionally attach a name to the buffer, to make it easier
 * to identify in debug traces.
 */
void qio_buffer_init(QIOBuffer *buffer, const char *name, ...)
        GCC_FMT_ATTR(2, 3);

/**
 * qio_buffer_shrink:
 * @buffer: the buffer object
 *
 * Try to shrink the buffer.  Checks current buffer capacity and size
 * and reduces capacity in case only a fraction of the buffer is
 * actually used.
 */
void qio_buffer_shrink(QIOBuffer *buffer);

/**
 * qio_buffer_reserve:
 * @buffer: the buffer object
 * @len: the minimum required free space
 *
 * Ensure that the buffer has space allocated for at least
 * @len bytes. If the current buffer is too small, it will
 * be reallocated, possibly to a larger size than requested.
 */
void qio_buffer_reserve(QIOBuffer *buffer, size_t len);

/**
 * qio_buffer_reset:
 * @buffer: the buffer object
 *
 * Reset the length of the stored data to zero, but do
 * not free / reallocate the memory buffer
 */
void qio_buffer_reset(QIOBuffer *buffer);

/**
 * qio_buffer_free:
 * @buffer: the buffer object
 *
 * Reset the length of the stored data to zero and also
 * free the internal memory buffer
 */
void qio_buffer_free(QIOBuffer *buffer);

/**
 * qio_buffer_append:
 * @buffer: the buffer object
 * @data: the data block to append
 * @len: the length of @data in bytes
 *
 * Append the contents of @data to the end of the buffer.
 * The caller must ensure that the buffer has sufficient
 * free space for @len bytes, typically by calling the
 * qio_buffer_reserve() method prior to appending.
 */
void qio_buffer_append(QIOBuffer *buffer, const void *data, size_t len);

/**
 * qio_buffer_advance:
 * @buffer: the buffer object
 * @len: the number of bytes to skip
 *
 * Remove @len bytes of data from the head of the buffer.
 * The internal buffer will not be reallocated, so will
 * have at least @len bytes of free space after this
 * call completes
 */
void qio_buffer_advance(QIOBuffer *buffer, size_t len);

/**
 * qio_buffer_end:
 * @buffer: the buffer object
 *
 * Get a pointer to the tail end of the internal buffer
 * The returned pointer is only valid until the next
 * call to qio_buffer_reserve().
 *
 * Returns: the tail of the buffer
 */
uint8_t *qio_buffer_end(QIOBuffer *buffer);

/**
 * qio_buffer_empty:
 * @buffer: the buffer object
 *
 * Determine if the buffer contains any current data
 *
 * Returns: true if the buffer holds data, false otherwise
 */
gboolean qio_buffer_empty(QIOBuffer *buffer);

/**
 * qio_buffer_move_empty:
 * @to: destination buffer object
 * @from: source buffer object
 *
 * Moves buffer, without copying data.  'to' buffer must be empty.
 * 'from' buffer is empty and zero-sized on return.
 */
void qio_buffer_move_empty(QIOBuffer *to, QIOBuffer *from);

/**
 * qio_buffer_move:
 * @to: destination buffer object
 * @from: source buffer object
 *
 * Moves buffer, copying data (unless 'to' buffer happens to be empty).
 * 'from' buffer is empty and zero-sized on return.
 */
void qio_buffer_move(QIOBuffer *to, QIOBuffer *from);

#endif /* QIO_BUFFER_H__ */
