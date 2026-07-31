#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Look up 16x16 bitmap for a UTF-8 encoded CJK character.
 * utf8: pointer to 3-byte UTF-8 sequence
 * len: length of available bytes (should be >= 3)
 * Returns pointer to 32-byte bitmap data, or NULL if not found. */
const uint8_t *cjk_get_bitmap_utf8(const uint8_t *utf8, int len);

#ifdef __cplusplus
}
#endif
