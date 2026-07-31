/*
 * cjk_font.c - HZK16 GB2312 16x16 Chinese font with Unicode->GB2312 mapping
 *
 * Two binary blobs embedded in flash:
 *   HZK16     - 256KB, 16x16 dot-matrix font (GB2312 encoded)
 *   uni2gb.bin - 29KB, Unicode->GB2312 lookup table (7445 entries, sorted by Unicode)
 */
#include "cjk_font.h"
#include <string.h>

/* HZK16 font data (embedded via CMake EMBED_FILES) */
extern const uint8_t hzk16_start[] asm("_binary_HZK16_start");
extern const uint8_t hzk16_end[]   asm("_binary_HZK16_end");

/* Unicode->GB2312 mapping table (embedded) */
extern const uint8_t uni2gb_start[] asm("_binary_uni2gb_bin_start");
extern const uint8_t uni2gb_end[]   asm("_binary_uni2gb_bin_end");

/* Table entry: 4 bytes (uint16_t unicode, uint16_t gb2312) */
typedef struct { uint16_t uni; uint16_t gb; } uni_gb_t;

#define MAP_ENTRIES (*(const uint32_t *)uni2gb_start)
#define MAP_TABLE   ((const uni_gb_t *)(uni2gb_start + 4))

/* Binary search for Unicode codepoint -> GB2312 code */
static bool uni_to_gb(uint16_t uni, uint8_t *gh, uint8_t *gl)
{
    int lo = 0, hi = (int)MAP_ENTRIES - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (MAP_TABLE[mid].uni == uni) {
            *gh = (uint8_t)(MAP_TABLE[mid].gb >> 8);
            *gl = (uint8_t)(MAP_TABLE[mid].gb & 0xFF);
            return true;
        }
        if (MAP_TABLE[mid].uni < uni) lo = mid + 1;
        else hi = mid - 1;
    }
    return false;
}

const uint8_t *cjk_get_bitmap_utf8(const uint8_t *utf8, int len)
{
    if (len < 3 || utf8 == NULL) return NULL;

    /* Extract Unicode from 3-byte UTF-8 */
    uint16_t uni = ((uint16_t)(utf8[0] & 0x0F) << 12) |
                   ((uint16_t)(utf8[1] & 0x3F) << 6) |
                   ((uint16_t)(utf8[2] & 0x3F));

    uint8_t gh, gl;
    if (!uni_to_gb(uni, &gh, &gl)) return NULL;

    /* Look up in HZK16: (row * 94 + col) * 32 */
    if (gh < 0xA1 || gh > 0xF7 || gl < 0xA1 || gl > 0xFE) return NULL;
    int row = gh - 0xA1;
    int col = gl - 0xA1;
    int offset = (row * 94 + col) * 32;

    /* Bounds check */
    if (offset + 32 > (int)(hzk16_end - hzk16_start)) return NULL;
    return &hzk16_start[offset];
}
