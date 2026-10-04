#ifndef CORE_UTF8_H
#define CORE_UTF8_H

#include <stddef.h>
#include <stdint.h>

/* Strict UTF-8: overlong forms, surrogates and code points above U+10FFFF are
 * rejected rather than replaced. Content files come from outside the engine, so
 * decoding must fail loudly instead of silently producing mojibake. */

#define UTF8_REPLACEMENT 0xFFFDu
#define UTF8_MAX_BYTES 4

/* Bytes consumed by the sequence at p, or 0 if it is not valid UTF-8.
 * On success cp receives the code point. */
size_t utf8_decode(const uint8_t *p, size_t avail, uint32_t *cp);

/* Bytes written to out (never more than UTF8_MAX_BYTES), or 0 if cp cannot be
 * encoded. */
size_t utf8_encode(uint32_t cp, uint8_t *out);

/* Number of code points in the whole buffer. Invalid bytes count as one each. */
size_t utf8_length(const uint8_t *p, size_t len);

/* Byte offset of the index-th code point, clamped to len. */
size_t utf8_offset(const uint8_t *p, size_t len, size_t index);

/* 1 if the whole buffer is valid UTF-8. */
int utf8_valid(const uint8_t *p, size_t len);

#endif