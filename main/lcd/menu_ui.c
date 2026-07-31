/*
 * menu_ui.c - Landscape menu UI with icon grid + sub-pages
 *
 * Home screen: 4 icons in a row (Status / RX / SWD / Config)
 * SW1=prev, SW3=next, SW2=enter, SW2-long=exit
 *
 * RX MON page: auto-scroll, 50-line history, SW2 toggle HEX,
 *   SW1/SW3 scroll history (stops auto-scroll), SW2-long exit
 */
#include "menu_ui.h"
#include "lcd_driver.h"
#include "serial_bridge.h"
#include "wifi_manager.h"
#include "swd_bridge.h"
#include "pinout.h"
#include "pin_config.h"
#include "pwm_mon.h"
#include "spi_mon.h"
#include "i2c_mon.h"
#include <string.h>
#include <stdio.h>
#include "esp_timer.h"
#include "driver/uart.h"

/* ---- RX line history ---- */
#define RX_LINE_MAX   60    /* bytes per line (room for UTF-8 CJK + prefix) */
#define RX_LINE_CAP   100   /* compile-time cap of stored lines */
static char  s_rx_lines[RX_LINE_CAP][RX_LINE_MAX + 1];
static int   s_rx_line_len[RX_LINE_CAP];  /* actual length */
static volatile int s_rx_line_w = 0;  /* write index (next free slot) */
static volatile int s_rx_line_n = 0;  /* number of lines stored */
static volatile int s_rx_hist_max = 50;  /* effective history depth */
static int   s_rx_cur_col = 0;        /* current column in partial line */
static int   s_rx_view_offset = 0;    /* how many lines back from latest (0=live) */
static bool  s_rx_auto_scroll = true; /* auto-scroll to latest */
static bool  s_rx_hex_mode = false;   /* HEX display mode */

static menu_page_t s_current_page = MENU_HOME;
static int s_selected = 0;
static const int s_num_icons = 8;

static const char *s_icon_labels[] = {"RX", "I2C", "SPI", "PWM", "PIN", "STA", "CFG", "AI"};
static const uint16_t s_icon_colors[] = {COLOR_GREEN, COLOR_YELLOW, COLOR_WHITE,
                                         COLOR_CYAN, COLOR_YELLOW, COLOR_CYAN,
                                         COLOR_MAGENTA, COLOR_RED};
/* Visual icon index → menu page (row1: RX,I2C,SPI,PWM  row2: PIN,STA,CFG,AI) */
static const menu_page_t s_icon_pages[] = {
    MENU_RX_MON, MENU_I2C, MENU_SPI, MENU_PWM,
    MENU_SWD,    MENU_STATUS, MENU_CONFIG, MENU_AI,
};

void menu_init(void)
{
    s_current_page = MENU_SWD;  /* Boot into SWD pinout page */
    s_selected = 0;
    s_rx_line_w = 0;
    s_rx_line_n = 0;
    s_rx_cur_col = 0;
    s_rx_view_offset = 0;
    s_rx_auto_scroll = true;
    s_rx_hex_mode = false;
    memset(s_rx_lines, 0, sizeof(s_rx_lines));
    memset(s_rx_line_len, 0, sizeof(s_rx_line_len));
}

/* Start a new line in the ring buffer */
static void rx_new_line(void)
{
    s_rx_line_w = (s_rx_line_w + 1) % s_rx_hist_max;
    if (s_rx_line_n < s_rx_hist_max) s_rx_line_n++;
    s_rx_lines[s_rx_line_w][0] = '\0';
    s_rx_line_len[s_rx_line_w] = 0;
    s_rx_cur_col = 0;
}

static volatile int s_spi_view_offset = 0;
static bool s_spi_hex_mode = true;   /* SPI/I2C HEX display mode (false=ASCII) */
static bool s_i2c_hex_mode = true;
static volatile int s_i2c_view_offset = 0;

/* ---- CFG settings list state ---- */
/* Items: 0=Baud 1=Bright 2=SBuf 3=SHist 4=IHist 5=RHist 6=NetRst */
typedef enum { CFG_LIST, CFG_EDIT } cfg_state_t;
static cfg_state_t s_cfg_state = CFG_LIST;
static int  s_cfg_sel = 0;
static int  s_cfg_baud_idx   = 1;
static int  s_cfg_bright_idx = 4;
static int  s_cfg_buf_idx    = 1;
static int  s_cfg_shist_idx  = 2;   /* SPI history preset index */
static int  s_cfg_ihist_idx  = 2;   /* I2C history preset index */
static int  s_cfg_rhist_idx  = 2;   /* RX  history preset index */
static const uint32_t cfg_bauds[]  = {9600, 115200, 460800, 921600};
static const int cfg_bright[]      = {10, 25, 50, 75, 100};       /* percent */
static const int cfg_bufs[]        = {1024, 2048, 4096, 8192};    /* bytes */
static const int cfg_hist[]        = {10, 30, 50, 100};           /* entries */
#define CFG_N_BAUD   ((int)(sizeof(cfg_bauds)/sizeof(cfg_bauds[0])))
#define CFG_N_BRIGHT ((int)(sizeof(cfg_bright)/sizeof(cfg_bright[0])))
#define CFG_N_BUF    ((int)(sizeof(cfg_bufs)/sizeof(cfg_bufs[0])))
#define CFG_N_HIST   ((int)(sizeof(cfg_hist)/sizeof(cfg_hist[0])))
#define CFG_N_ITEMS  8

static int cfg_baud_index(void)
{
    uint32_t b = 0;
    uart_get_baudrate(UART1_PORT_NUM, &b);
    for (int i = 0; i < CFG_N_BAUD; i++) if (cfg_bauds[i] == b) return i;
    return 1;
}

/* Adjust the selected setting by dir (+1 next / -1 prev). Applies immediately. */
static void cfg_adjust(int dir)
{
    switch (s_cfg_sel) {
    case 0:  /* Baud */
        s_cfg_baud_idx = (s_cfg_baud_idx + dir + CFG_N_BAUD) % CFG_N_BAUD;
        serial_bridge_set_baud(cfg_bauds[s_cfg_baud_idx]);
        break;
    case 1:  /* Brightness */
        s_cfg_bright_idx = (s_cfg_bright_idx + dir + CFG_N_BRIGHT) % CFG_N_BRIGHT;
        lcd_set_brightness(cfg_bright[s_cfg_bright_idx]);
        break;
    case 2:  /* Serial buffer size */
        s_cfg_buf_idx = (s_cfg_buf_idx + dir + CFG_N_BUF) % CFG_N_BUF;
        serial_bridge_set_bufsize((size_t)cfg_bufs[s_cfg_buf_idx]);
        break;
    case 3:  /* SPI history depth */
        s_cfg_shist_idx = (s_cfg_shist_idx + dir + CFG_N_HIST) % CFG_N_HIST;
        spi_mon_set_history_max(cfg_hist[s_cfg_shist_idx]);
        break;
    case 4:  /* I2C history depth */
        s_cfg_ihist_idx = (s_cfg_ihist_idx + dir + CFG_N_HIST) % CFG_N_HIST;
        i2c_mon_set_history_max(cfg_hist[s_cfg_ihist_idx]);
        break;
    case 5:  /* RX history depth */
        s_cfg_rhist_idx = (s_cfg_rhist_idx + dir + CFG_N_HIST) % CFG_N_HIST;
        s_rx_hist_max = cfg_hist[s_cfg_rhist_idx];
        s_rx_line_n = 0;
        break;
    case 6:  /* NetRst — action only */
        break;
    case 7:  /* Clear — action only */
        break;
    }
}

/* forward decl: defined below (used by CFG clear action) */
void menu_clear_rx(void);

void menu_on_sw1_press(void)
{
    if (s_current_page == MENU_HOME) {
        s_selected--;
        if (s_selected < 0) s_selected = s_num_icons - 1;
    } else if (s_current_page == MENU_RX_MON) {
        /* Scroll up (older) */
        if (s_rx_view_offset < s_rx_line_n - 1) {
            s_rx_view_offset++;
            s_rx_auto_scroll = false;
        }
    } else if (s_current_page == MENU_SPI) {
        if (s_spi_view_offset < 9) {
            s_spi_view_offset++;
        }
    } else if (s_current_page == MENU_I2C) {
        if (s_i2c_view_offset < 9) {
            s_i2c_view_offset++;
        }
    } else if (s_current_page == MENU_CONFIG) {
        if (s_cfg_state == CFG_LIST)
            s_cfg_sel = (s_cfg_sel - 1 + CFG_N_ITEMS) % CFG_N_ITEMS;
        else
            cfg_adjust(-1);
    }
}

void menu_on_sw3_press(void)
{
    if (s_current_page == MENU_HOME) {
        s_selected++;
        if (s_selected >= s_num_icons) s_selected = 0;
    } else if (s_current_page == MENU_RX_MON) {
        /* Scroll down (newer) */
        if (s_rx_view_offset > 0) {
            s_rx_view_offset--;
            if (s_rx_view_offset == 0) s_rx_auto_scroll = true;
        }
    } else if (s_current_page == MENU_SPI) {
        if (s_spi_view_offset > 0) {
            s_spi_view_offset--;
        }
    } else if (s_current_page == MENU_I2C) {
        if (s_i2c_view_offset > 0) {
            s_i2c_view_offset--;
        }
    } else if (s_current_page == MENU_CONFIG) {
        if (s_cfg_state == CFG_LIST)
            s_cfg_sel = (s_cfg_sel + 1) % CFG_N_ITEMS;
        else
            cfg_adjust(+1);
    }
}

void menu_on_sw2_press(void)
{
    if (s_current_page == MENU_HOME) {
        s_current_page = s_icon_pages[s_selected];
        /* Reset RX view when entering */
        if (s_current_page == MENU_RX_MON) {
            s_rx_view_offset = 0;
            s_rx_auto_scroll = true;
        }
        /* CFG always starts in list-selection mode */
        if (s_current_page == MENU_CONFIG) {
            s_cfg_state = CFG_LIST;
        }
    } else if (s_current_page == MENU_RX_MON) {
        /* Toggle HEX mode */
        s_rx_hex_mode = !s_rx_hex_mode;
    } else if (s_current_page == MENU_SPI) {
        s_spi_hex_mode = !s_spi_hex_mode;
    } else if (s_current_page == MENU_I2C) {
        s_i2c_hex_mode = !s_i2c_hex_mode;
    } else if (s_current_page == MENU_CONFIG) {
        if (s_cfg_state == CFG_LIST) {
            /* enter edit: sync indices from current runtime values */
            s_cfg_baud_idx = cfg_baud_index();
            int sv = spi_mon_get_history_max();
            for (int i = 0; i < CFG_N_HIST; i++) if (cfg_hist[i] == sv) s_cfg_shist_idx = i;
            int iv = i2c_mon_get_history_max();
            for (int i = 0; i < CFG_N_HIST; i++) if (cfg_hist[i] == iv) s_cfg_ihist_idx = i;
            int rv = s_rx_hist_max;
            for (int i = 0; i < CFG_N_HIST; i++) if (cfg_hist[i] == rv) s_cfg_rhist_idx = i;
            s_cfg_state = CFG_EDIT;
        } else {
            /* in edit: single SW2 confirms and exits back to list.
             * NetRst (6) and Clear (7) are actions — confirming executes them. */
            if (s_cfg_sel == 6) {
                wifi_manager_clear_credentials();   /* clears WiFi, restarts AP */
            } else if (s_cfg_sel == 7) {
                serial_bridge_clear();
                spi_mon_clear();
                i2c_mon_clear();
                menu_clear_rx();
            }
            s_cfg_state = CFG_LIST;
        }
    }
}

void menu_on_sw2_long_press(void)
{
    /* CFG: double-click SW2 exits edit→list, or list→home */
    if (s_current_page == MENU_CONFIG) {
        if (s_cfg_state == CFG_EDIT) {
            s_cfg_state = CFG_LIST;
            return;
        }
    }
    if (s_current_page != MENU_HOME) {
        s_current_page = MENU_HOME;
    }
}

/* Hold SW2 (true long-press): I2C toggles slave↔passive mode */
void menu_on_sw2_hold(void)
{
    if (s_current_page == MENU_I2C) {
        int sda = pin_config_i2c_sda(), scl = pin_config_i2c_scl();
        i2c_mon_stop();
        if (i2c_mon_mode() == I2C_MON_SLAVE) {
            if (sda >= 0 && scl >= 0) i2c_mon_start_passive(sda, scl);
        } else {
            if (sda >= 0 && scl >= 0) i2c_mon_start(sda, scl, 0);  /* back to slave @0x50 */
        }
    }
}

void menu_simulate_button(int btn_id, const char *action)
{
    bool is_long = (action && strcmp(action, "long") == 0);
    bool is_hold = (action && strcmp(action, "hold") == 0);
    if (btn_id == 1) {
        if (is_long) {
            /* SW1 long = AP mode (handled in main.c, call wifi directly) */
            wifi_manager_start_ap();
        } else {
            menu_on_sw1_press();
        }
    } else if (btn_id == 2) {
        if (is_hold) {
            menu_on_sw2_hold();
        } else if (is_long) {
            menu_on_sw2_long_press();
        } else {
            menu_on_sw2_press();
        }
    } else if (btn_id == 3) {
        if (is_long) {
            /* SW3 long = cycle baud */
            static const uint32_t bauds[] = {9600, 115200, 460800, 921600};
            static int idx = 1;
            idx = (idx + 1) % 4;
            serial_bridge_set_baud(bauds[idx]);
        } else {
            menu_on_sw3_press();
        }
    }
}

/* Display width budget per line.
 * Screen = 160px, text starts at x=2, prefix '> ' = 16px.
 * Available for content = 160 - 2 - 16 = 142px.
 * ASCII char = 8px, CJK = 16px. Use pixel width tracking. */
#define RX_CONTENT_PX  140  /* pixels available for content after prefix+margin */

/* Calculate display pixel width of a character.
 * ASCII=8px, CJK/multibyte=16px */
static int char_pixel_width(unsigned char c)
{
    if (c < 0x80) return 8;
    return 16;  /* all multibyte chars are 16px wide */
}

/* Append one serial event (RX or TX) to the RX-monitor ring buffer. The first
 * line is prefixed with `prefix` ('>' for RX, '<' for TX); wrapped continuation
 * lines are indented with two spaces. */
static void push_serial_data(char prefix, const uint8_t *data, uint32_t len)
{
    /* Each call = one event. Start on a fresh line if mid-line. */
    if (s_rx_cur_col > 0) {
        rx_new_line();
    }
    /* The initial slot (entered before any rx_new_line()) is never counted by
     * rx_new_line(), so without this the very first event would read as
     * "no data" and never render. Count it here. */
    if (s_rx_line_n == 0) {
        s_rx_line_n = 1;
    }

    /* Prefix marker at the beginning of this event's first line */
    s_rx_lines[s_rx_line_w][0] = prefix;
    s_rx_lines[s_rx_line_w][1] = ' ';
    s_rx_cur_col = 2;
    s_rx_line_len[s_rx_line_w] = 2;
    s_rx_lines[s_rx_line_w][s_rx_cur_col] = '\0';
    int display_w = 0;  /* current pixel width of content (excluding prefix) */

    for (uint32_t i = 0; i < len; i++) {
        char c = (char)data[i];

        /* Convert control chars to visible escape sequences */
        const char *esc = NULL;
        int esc_w = 0;
        switch (c) {
            case '\n': esc = "\\n"; esc_w = 2; break;
            case '\r': esc = "\\r"; esc_w = 2; break;
            case '\t': esc = "\\t"; esc_w = 2; break;
            case '\0': esc = "\\0"; esc_w = 2; break;
            case '\x1b': esc = "\\e"; esc_w = 2; break;
            default:
                if ((unsigned char)c < 0x20) {
                    static char hexbuf[5];
                    snprintf(hexbuf, sizeof(hexbuf), "\\x%02X", (unsigned char)c);
                    esc = hexbuf;
                    esc_w = 4;
                }
                break;
        }

        if (esc) {
            /* Insert escape sequence as visible text (each char = 8px) */
            for (int e = 0; e < esc_w; e++) {
                if (display_w + 8 > RX_CONTENT_PX) {
                    rx_new_line();
                    s_rx_lines[s_rx_line_w][0] = ' ';
                    s_rx_lines[s_rx_line_w][1] = ' ';
                    s_rx_cur_col = 2;
                    s_rx_line_len[s_rx_line_w] = 2;
                    s_rx_lines[s_rx_line_w][s_rx_cur_col] = '\0';
                    display_w = 0;
                }
                if (s_rx_line_len[s_rx_line_w] < RX_LINE_MAX) {
                    s_rx_lines[s_rx_line_w][s_rx_cur_col++] = esc[e];
                    s_rx_line_len[s_rx_line_w]++;
                    s_rx_lines[s_rx_line_w][s_rx_cur_col] = '\0';
                    display_w += 8;
                }
            }
        } else {
            int cw = char_pixel_width((unsigned char)c);
            /* Auto-wrap when pixel width exceeds visible area */
            if (display_w + cw > RX_CONTENT_PX) {
                rx_new_line();
                s_rx_lines[s_rx_line_w][0] = ' ';
                s_rx_lines[s_rx_line_w][1] = ' ';
                s_rx_cur_col = 2;
                s_rx_line_len[s_rx_line_w] = 2;
                s_rx_lines[s_rx_line_w][s_rx_cur_col] = '\0';
                display_w = 0;
            }
            /* Store byte if room in buffer */
            if (s_rx_line_len[s_rx_line_w] < RX_LINE_MAX) {
                s_rx_lines[s_rx_line_w][s_rx_cur_col++] = c;
                s_rx_line_len[s_rx_line_w]++;
                s_rx_lines[s_rx_line_w][s_rx_cur_col] = '\0';
                display_w += cw;
            }
        }
    }
}

void menu_push_rx_data(const uint8_t *data, uint32_t len)
{
    push_serial_data('>', data, len);
}

void menu_push_tx_data(const uint8_t *data, uint32_t len)
{
    push_serial_data('<', data, len);
}

/* ---- Icon drawing ---- */
static void draw_icon(int x, int y, int w, int h, const char *label,
                      uint16_t color, bool selected)
{
    uint16_t bg = selected ? color : COLOR_BLACK;
    uint16_t fg = selected ? COLOR_BLACK : color;
    lcd_fill_rect(x, y, w, h, bg);
    lcd_draw_rect(x, y, w, h, color);
    int text_x = x + (w - 24) / 2;
    int text_y = y + (h - 16) / 2;
    lcd_draw_text(text_x, text_y, label, fg, bg);
}

/* ---- Page renderers ---- */

static void render_home(void)
{
    lcd_fill(COLOR_BLACK);
    /* Top bar: title + hint */
    lcd_draw_text(2, 0, "Menu", COLOR_WHITE, COLOR_BLACK);
    lcd_draw_text(50, 0, "SW1< SW2> SW3>", COLOR_GRAY, COLOR_BLACK);
    lcd_draw_hline(0, 16, LCD_WIDTH, COLOR_GRAY);

    const int cols = 4;
    const int icon_w = 34, icon_h = 22, gap_y = 4;
    int gap_x = (LCD_WIDTH - icon_w * cols) / (cols + 1);
    int rows = (s_num_icons + cols - 1) / cols;
    int total_h = rows * icon_h + (rows - 1) * gap_y;
    int y0 = 16 + (LCD_HEIGHT - 16 - total_h) / 2;
    for (int i = 0; i < s_num_icons; i++) {
        int r = i / cols, c = i % cols;
        int x = gap_x + c * (icon_w + gap_x);
        int y = y0 + r * (icon_h + gap_y);
        draw_icon(x, y, icon_w, icon_h, s_icon_labels[i], s_icon_colors[i], i == s_selected);
    }
}

static void render_status(void)
{
    lcd_fill(COLOR_BLACK);
    lcd_draw_text(2, 0, "STATUS", COLOR_CYAN, COLOR_BLACK);
    lcd_draw_hline(0, 16, LCD_WIDTH, COLOR_GRAY);

    char ip[16] = {0};
    wifi_manager_get_ip_str(ip, sizeof(ip));
    wifi_state_t st = wifi_manager_get_state();
    const char *ws = "Disc";
    if (st == WIFI_STATE_CONNECTED_STA) ws = "STA";
    else if (st == WIFI_STATE_AP_MODE) ws = "AP";

    uint32_t baud = 0;
    uart_get_baudrate(UART1_PORT_NUM, &baud);

    char line[32];
    snprintf(line, sizeof(line), "WiFi:%s %s", ws, ip);
    lcd_draw_text(2, 18, line, COLOR_WHITE, COLOR_BLACK);
    snprintf(line, sizeof(line), "Baud:%lu", (unsigned long)baud);
    lcd_draw_text(2, 34, line, COLOR_WHITE, COLOR_BLACK);
    snprintf(line, sizeof(line), "RX:%lu TX:%lu",
             (unsigned long)serial_bridge_get_rx_count(),
             (unsigned long)serial_bridge_get_tx_count());
    lcd_draw_text(2, 50, line, COLOR_GREEN, COLOR_BLACK);
    snprintf(line, sizeof(line), "UP:%lus",
             (unsigned long)(esp_timer_get_time() / 1000000));
    lcd_draw_text(2, 66, line, COLOR_GRAY, COLOR_BLACK);
}

/* Render one transaction's data with a leading direction marker.
 * ascii=false: hex (6 bytes/line). ascii=true: printable chars, '.' for
 * non-printable (18 chars/line). Marker only on the first line.
 * Returns the new y position. Stops at max_y. */
static int draw_txn_hex(int y, char marker, uint16_t mcolor,
                        const uint8_t *data, int len, int max_y, bool ascii)
{
    const int per_line = ascii ? 18 : 6;
    int off = 0;
    int first = 1;
    while (off < len && y < max_y) {
        if (first) {
            char mk[2] = {marker, '\0'};
            lcd_draw_text(2, y, mk, mcolor, COLOR_BLACK);
        }
        char buf[24] = {0};
        int p = 0;
        int chunk = len - off;
        if (chunk > per_line) chunk = per_line;
        for (int b = 0; b < chunk; b++) {
            uint8_t c = data[off + b];
            if (ascii) buf[p++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
            else       p += snprintf(buf + p, sizeof(buf) - p, "%02X ", c);
        }
        lcd_draw_text(12, y, buf, COLOR_WHITE, COLOR_BLACK);
        off += chunk;
        y += 14;
        first = 0;
    }
    if (first) {
        char mk[2] = {marker, '\0'};
        lcd_draw_text(2, y, mk, mcolor, COLOR_BLACK);
        y += 14;
    }
    return y;
}

static void render_rx_mon(void)
{
    lcd_fill(COLOR_BLACK);

    char title[40];
    if (s_rx_auto_scroll) {
        snprintf(title, sizeof(title), "RX %s LIVE %d/%d",
                 s_rx_hex_mode ? "HEX" : "TXT", s_rx_line_n, s_rx_hist_max);
    } else {
        snprintf(title, sizeof(title), "RX %s -%d %d/%d",
                 s_rx_hex_mode ? "HEX" : "TXT", s_rx_view_offset, s_rx_line_n, s_rx_hist_max);
    }
    lcd_draw_text(2, 0, title, s_rx_auto_scroll ? COLOR_GREEN : COLOR_YELLOW, COLOR_BLACK);
    lcd_draw_hline(0, 16, LCD_WIDTH, COLOR_GRAY);

    int latest = s_rx_line_w;
    if (s_rx_line_n == 0) {
        lcd_draw_text(2, 18, "(no data)", COLOR_GRAY, COLOR_BLACK);
        return;
    }

    /* Two-pass render so wrapped content reads top→bottom (marker then
     * continuation) and the newest event sits at the bottom.
     * back=0 is the newest stored line; larger back = older.
     * Font is 16px tall (ink in rows 2-13), so lh=14 avoids clipping. */
    const int y0 = 20, lh = 14, glyph_bot = LCD_HEIGHT - 16;  /* last valid y */
    int avail = (glyph_bot - y0) / lh + 1;

    /* Pass 1: walk back from newest, count display lines per stored line,
     * find the oldest line that still fits (window_start_back). */
    int newest_back = s_rx_view_offset;
    int window_start_back = newest_back;
    int used = 0;
    for (int back = newest_back; back < s_rx_line_n; back++) {
        int idx = latest - back;
        while (idx < 0) idx += s_rx_hist_max;
        int rawlen = s_rx_line_len[idx];
        int content = rawlen < 2 ? 0 : rawlen - 2;
        int dl;
        if (s_rx_hex_mode) dl = content > 0 ? (content + 5) / 6 : 1;
        else               dl = 1;
        if (used + dl > avail) break;       /* doesn't fit; stop here */
        used += dl;
        window_start_back = back;
    }

    /* Pass 2: render window_start_back (oldest, top) → newest (bottom). */
    int y = y0;
    for (int back = window_start_back; back >= newest_back && y <= glyph_bot; back--) {
        int idx = latest - back;
        while (idx < 0) idx += s_rx_hist_max;

        char *raw = s_rx_lines[idx];
        int rawlen = s_rx_line_len[idx];
        if (rawlen < 2) { y += lh; continue; }

        char marker = raw[0];
        uint16_t mcolor;
        if (marker == '>')      mcolor = COLOR_CYAN;
        else if (marker == '<') mcolor = COLOR_YELLOW;
        else                    mcolor = COLOR_GRAY;

        if (s_rx_hex_mode) {
            int content_len = rawlen - 2;
            const uint8_t *bytes = (const uint8_t *)(raw + 2);
            const int per_line = 6;
            int off = 0, first = 1;
            while (off < content_len && y <= glyph_bot) {
                if (first) {
                    char mk[2] = {marker, '\0'};
                    lcd_draw_text(2, y, mk, mcolor, COLOR_BLACK);
                }
                char hex[24] = {0};
                int p = 0, chunk = content_len - off;
                if (chunk > per_line) chunk = per_line;
                for (int b = 0; b < chunk; b++)
                    p += snprintf(hex + p, sizeof(hex) - p, "%02X ", bytes[off + b]);
                lcd_draw_text(12, y, hex, COLOR_WHITE, COLOR_BLACK);
                off += chunk; y += lh; first = 0;
            }
            if (first) y += lh;
        } else {
            char mk[2] = {marker, '\0'};
            lcd_draw_text(2, y, mk, mcolor, COLOR_BLACK);
            lcd_draw_text(12, y, raw + 2, COLOR_WHITE, COLOR_BLACK);
            y += lh;
        }
    }
}

static uint16_t pin_kind_color(pin_kind_t k)
{
    switch (k) {
    case PIN_5V:    return COLOR_RED;
    case PIN_GND:   return COLOR_GRAY;
    case PIN_DUT:   return COLOR_CYAN;
    case PIN_SWCLK: return COLOR_YELLOW;
    case PIN_SWDIO: return COLOR_YELLOW;
    case PIN_NRST:  return COLOR_MAGENTA;
    case PIN_TX:    return COLOR_GREEN;
    case PIN_RX:    return COLOR_GREEN;
    default:        return COLOR_WHITE;
    }
}

static void render_swd(void)
{
    lcd_fill(COLOR_BLACK);
    lcd_draw_text(2, 0, "J3 Pinout", COLOR_YELLOW, COLOR_BLACK);
    lcd_draw_hline(0, 16, LCD_WIDTH, COLOR_GRAY);

    /* 2x4 diagram. 5V/GND/DUT are fixed power/reference pins; the other five
     * slots are filled from the saved signal order (pin_config).
     *   row0: 5V  GND  sig[0] sig[1]
     *   row1: DUT sig[2] sig[3] sig[4] */
    pin_kind_t grid[2][4] = {
        { PIN_5V, PIN_GND, pin_config_pos_func(0), pin_config_pos_func(1) },
        { PIN_DUT, pin_config_pos_func(2), pin_config_pos_func(3), pin_config_pos_func(4) },
    };

    int box_w = 36, box_h = 20;
    int x0 = 8;
    int rows_y[2] = { 22, 46 };

    for (int r = 0; r < 2; r++) {
        for (int c = 0; c < 4; c++) {
            pin_kind_t k = grid[r][c];
            uint16_t col = pin_kind_color(k);
            int x = x0 + c * (box_w + 2);
            lcd_draw_rect(x, rows_y[r], box_w, box_h, col);
            const char *lbl = pin_config_kind_label(k);
            int tx = x + (box_w - (int)strlen(lbl) * 8) / 2;
            lcd_draw_text(tx, rows_y[r] + 3, lbl, col, COLOR_BLACK);
        }
    }
}

static void render_config(void)
{
    lcd_fill(COLOR_BLACK);
    /* Top: free heap (storage remaining) */
    char cap[28];
    snprintf(cap, sizeof(cap), "CFG  free %luKB",
             (unsigned long)(esp_get_free_heap_size() / 1024));
    lcd_draw_text(2, 0, cap, COLOR_MAGENTA, COLOR_BLACK);
    lcd_draw_hline(0, 16, LCD_WIDTH, COLOR_GRAY);

    if (s_cfg_state == CFG_LIST) s_cfg_baud_idx = cfg_baud_index();

    static const char *names[CFG_N_ITEMS] = {"Baud", "Bright", "SBuf", "SHist", "IHist", "RHist", "NetRst", "Clear"};
    char vals[CFG_N_ITEMS][12];
    snprintf(vals[0], sizeof(vals[0]), "%lu", (unsigned long)cfg_bauds[s_cfg_baud_idx]);
    snprintf(vals[1], sizeof(vals[1]), "%d%%", cfg_bright[s_cfg_bright_idx]);
    snprintf(vals[2], sizeof(vals[2]), "%dK", cfg_bufs[s_cfg_buf_idx] / 1024);
    snprintf(vals[3], sizeof(vals[3]), "%d", cfg_hist[s_cfg_shist_idx]);
    snprintf(vals[4], sizeof(vals[4]), "%d", cfg_hist[s_cfg_ihist_idx]);
    snprintf(vals[5], sizeof(vals[5]), "%d", cfg_hist[s_cfg_rhist_idx]);
    snprintf(vals[6], sizeof(vals[6]), "%s",
             (s_cfg_sel == 6 && s_cfg_state == CFG_EDIT) ? "EXEC" : "reset");
    snprintf(vals[7], sizeof(vals[7]), "%s",
             (s_cfg_sel == 7 && s_cfg_state == CFG_EDIT) ? "EXEC" : "clr");

    /* scrolling window: keep selection visible (4 rows at 15px) */
    const int rows = 4;
    int start = s_cfg_sel - 1;
    if (start < 0) start = 0;
    if (start > CFG_N_ITEMS - rows) start = CFG_N_ITEMS - rows;

    int y = 22;
    for (int r = 0; r < rows; r++) {
        int i = start + r;
        if (i >= CFG_N_ITEMS) break;
        bool sel = (i == s_cfg_sel);
        uint16_t col = sel ? COLOR_YELLOW : COLOR_WHITE;
        lcd_draw_text(2, y, sel ? ">" : " ", col, COLOR_BLACK);
        lcd_draw_text(12, y, names[i], col, COLOR_BLACK);
        bool editing = (sel && s_cfg_state == CFG_EDIT);
        if (editing) {
            lcd_draw_text(80, y, "[", COLOR_GREEN, COLOR_BLACK);
            lcd_draw_text(88, y, vals[i], COLOR_GREEN, COLOR_BLACK);
            int bx = 88 + (int)strlen(vals[i]) * 8;
            lcd_draw_text(bx, y, "]", COLOR_GREEN, COLOR_BLACK);
        } else {
            lcd_draw_text(80, y, vals[i], sel ? COLOR_GREEN : COLOR_GRAY, COLOR_BLACK);
        }
        y += 15;
    }
}

/* Draw an idealized square wave (N cycles) with the given duty (0..1). */
static void draw_wave(int x, int y, int w, int h, float duty, uint16_t color)
{
    if (duty < 0.0f) duty = 0.0f;
    if (duty > 1.0f) duty = 1.0f;
    const int cycles = 6;
    int cw = w / cycles;
    int top_y = y;
    int bot_y = y + h - 4;
    int vh = bot_y - top_y + 3;
    for (int i = 0; i < cycles; i++) {
        int cx = x + i * cw;
        int hi = (int)(cw * duty);
        if (hi < 1) hi = 1;
        lcd_fill_rect(cx, top_y, hi, 3, color);            /* high level */
        lcd_fill_rect(cx + hi, bot_y, cw - hi, 3, color);  /* low level */
        lcd_draw_vline(cx, top_y, vh, color);              /* rising edge */
        lcd_draw_vline(cx + hi, top_y, vh, color);         /* falling edge */
    }
}

static void format_freq(char *buf, size_t n, float f)
{
    if (f < 1.0f)            snprintf(buf, n, "%.3f Hz", f);
    else if (f < 1000.0f)    snprintf(buf, n, "%.1f Hz", f);
    else if (f < 1000000.0f) snprintf(buf, n, "%.2f kHz", f / 1000.0f);
    else                     snprintf(buf, n, "%.2f MHz", f / 1000000.0f);
}

static void render_pwm(void)
{
    lcd_fill(COLOR_BLACK);
    lcd_draw_text(2, 0, "PWM", COLOR_CYAN, COLOR_BLACK);
    lcd_draw_hline(0, 16, LCD_WIDTH, COLOR_GRAY);

    float f, d;
    pwm_mon_get(&f, &d);

    char line[28];
    format_freq(line, sizeof(line), f);
    lcd_draw_text(2, 20, line, COLOR_WHITE, COLOR_BLACK);
    snprintf(line, sizeof(line), "Duty %.1f%%", d);
    lcd_draw_text(2, 36, line, COLOR_GREEN, COLOR_BLACK);

    draw_wave(2, 52, LCD_WIDTH - 4, 22, d / 100.0f, COLOR_CYAN);
}

static void render_spi(void)
{
    lcd_fill(COLOR_BLACK);
    lcd_draw_text(2, 0, "SPI", COLOR_WHITE, COLOR_BLACK);
    lcd_draw_hline(0, 16, LCD_WIDTH, COLOR_GRAY);

    if (!spi_mon_running()) {
        lcd_draw_text(2, 24, "not assigned", COLOR_GRAY, COLOR_BLACK);
        lcd_draw_text(2, 40, "set SCK/MOSI/", COLOR_GRAY, COLOR_BLACK);
        lcd_draw_text(2, 56, "MISO/CS pins", COLOR_GRAY, COLOR_BLACK);
        return;
    }

    spi_txn_t txns[10];
    int total = spi_mon_get_history(txns, 10);

    char hdr[32];
    if (s_spi_view_offset > 0)
        snprintf(hdr, sizeof(hdr), "SPI %s %lu -%d",
                 s_spi_hex_mode ? "HEX" : "TXT", (unsigned long)spi_mon_get_count(), s_spi_view_offset);
    else
        snprintf(hdr, sizeof(hdr), "SPI %s %lu LIVE",
                 s_spi_hex_mode ? "HEX" : "TXT", (unsigned long)spi_mon_get_count());
    lcd_draw_text(2, 18, hdr, s_spi_view_offset > 0 ? COLOR_YELLOW : COLOR_GREEN, COLOR_BLACK);

    if (total == 0) {
        lcd_draw_text(2, 36, "(no txn)", COLOR_GRAY, COLOR_BLACK);
        return;
    }

    /* Two-pass: newest at bottom, wrapped content reads top→bottom.
     * r=0 is newest txn. Font 16px tall, lh=14 avoids clipping. */
    const int y0 = 28, lh = 14, glyph_bot = LCD_HEIGHT - 16;
    int avail = (glyph_bot - y0) / lh + 1;

    int newest_r = s_spi_view_offset;
    int window_start_r = newest_r;
    int used = 0;
    int pl = s_spi_hex_mode ? 6 : 18;
    for (int r = newest_r; r < total; r++) {
        int idx = total - 1 - r;
        if (idx < 0) break;
        spi_txn_t *t = &txns[idx];
        int dl = (t->len + pl - 1) / pl; if (dl == 0) dl = 1;   /* MOSI */
        bool miso_nz = false;
        for (int b = 0; b < t->len; b++) { if (t->miso[b]) { miso_nz = true; break; } }
        if (miso_nz) { int m = (t->len + pl - 1) / pl; if (m == 0) m = 1; dl += m; }
        if (used + dl > avail) break;
        used += dl;
        window_start_r = r;
    }

    int y = y0;
    for (int r = window_start_r; r >= newest_r && y <= glyph_bot; r--) {
        int idx = total - 1 - r;
        if (idx < 0) break;
        spi_txn_t *t = &txns[idx];
        y = draw_txn_hex(y, '<', COLOR_YELLOW, t->mosi, t->len, glyph_bot + 1, !s_spi_hex_mode);
        bool miso_nz = false;
        for (int b = 0; b < t->len; b++) { if (t->miso[b]) { miso_nz = true; break; } }
        if (miso_nz)
            y = draw_txn_hex(y, '>', COLOR_CYAN, t->miso, t->len, glyph_bot + 1, !s_spi_hex_mode);
    }
}

static void render_ai(void)
{
    lcd_fill(COLOR_BLACK);
    lcd_draw_text(2, 0, "AI ASSISTANT", COLOR_RED, COLOR_BLACK);
    lcd_draw_hline(0, 16, LCD_WIDTH, COLOR_GRAY);
    lcd_draw_text(2, 18, "Configure via web:", COLOR_CYAN, COLOR_BLACK);
    lcd_draw_text(2, 34, "Open browser ->", COLOR_WHITE, COLOR_BLACK);
    lcd_draw_text(2, 50, "AI page ->", COLOR_WHITE, COLOR_BLACK);
    lcd_draw_text(2, 66, "Set API/Key/Model", COLOR_YELLOW, COLOR_BLACK);
}

static void render_i2c(void)
{
    lcd_fill(COLOR_BLACK);
    lcd_draw_text(2, 0, "I2C", COLOR_YELLOW, COLOR_BLACK);
    /* show mode + slave address */
    if (i2c_mon_running()) {
        const char *m = (i2c_mon_mode() == I2C_MON_SLAVE) ? "SLV" : "MON";
        char mb[16];
        snprintf(mb, sizeof(mb), "%s 长按切", m);
        lcd_draw_text(108, 0, mb, COLOR_GRAY, COLOR_BLACK);
    }
    lcd_draw_hline(0, 16, LCD_WIDTH, COLOR_GRAY);

    if (!i2c_mon_running()) {
        lcd_draw_text(2, 28, "not assigned", COLOR_GRAY, COLOR_BLACK);
        lcd_draw_text(2, 44, "set SDA/SCL pins", COLOR_GRAY, COLOR_BLACK);
        return;
    }

    i2c_txn_t txns[10];
    int total = i2c_mon_get_history(txns, 10);

    char hdr[32];
    if (s_i2c_view_offset > 0)
        snprintf(hdr, sizeof(hdr), "I2C %s %lu -%d",
                 s_i2c_hex_mode ? "HEX" : "TXT", (unsigned long)i2c_mon_get_count(), s_i2c_view_offset);
    else
        snprintf(hdr, sizeof(hdr), "I2C %s %lu LIVE",
                 s_i2c_hex_mode ? "HEX" : "TXT", (unsigned long)i2c_mon_get_count());
    lcd_draw_text(2, 18, hdr, s_i2c_view_offset > 0 ? COLOR_YELLOW : COLOR_GREEN, COLOR_BLACK);

    if (total == 0) {
        lcd_draw_text(2, 36, "(no txn)", COLOR_GRAY, COLOR_BLACK);
        return;
    }

    /* Two-pass: newest at bottom, wrapped content reads top→bottom.
     * Tag "<50"/">50" on first line; data wraps below.
     * HEX: 5 bytes/line (x=26). ASCII: 16 chars/line (x=26). lh=14. */
    const int bytes_per_line = s_i2c_hex_mode ? 5 : 16;
    const int y0 = 28, lh = 14, glyph_bot = LCD_HEIGHT - 16;
    int avail = (glyph_bot - y0) / lh + 1;

    int newest_r = s_i2c_view_offset;
    int window_start_r = newest_r;
    int used = 0;
    for (int r = newest_r; r < total; r++) {
        int idx = total - 1 - r;
        if (idx < 0) break;
        i2c_txn_t *t = &txns[idx];
        int dl = t->len > 0 ? (t->len + bytes_per_line - 1) / bytes_per_line : 1;
        if (used + dl > avail) break;
        used += dl;
        window_start_r = r;
    }

    int y = y0;
    for (int r = window_start_r; r >= newest_r && y <= glyph_bot; r--) {
        int idx = total - 1 - r;
        if (idx < 0) break;
        i2c_txn_t *t = &txns[idx];

        char tag[4];
        snprintf(tag, sizeof(tag), "%s%02X", t->read ? ">" : "<", t->addr >> 1);
        uint16_t tcol = t->read ? COLOR_CYAN : COLOR_YELLOW;
        lcd_draw_text(2, y, tag, tcol, COLOR_BLACK);

        int off = 0, first = 1;
        while (off < t->len && y <= glyph_bot) {
            char buf[20] = {0};
            int p = 0;
            int chunk = t->len - off;
            if (chunk > bytes_per_line) chunk = bytes_per_line;
            for (int b = 0; b < chunk; b++) {
                uint8_t c = t->data[off + b];
                if (s_i2c_hex_mode) p += snprintf(buf + p, sizeof(buf) - p, "%02X ", c);
                else buf[p++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
            }
            lcd_draw_text(first ? 26 : 12, y, buf, COLOR_WHITE, COLOR_BLACK);
            off += chunk; y += lh; first = 0;
        }
        if (first) y += lh;
    }
}

void menu_render(void)
{
    switch (s_current_page) {
    case MENU_HOME:   render_home();   break;
    case MENU_STATUS: render_status(); break;
    case MENU_RX_MON: render_rx_mon(); break;
    case MENU_SWD:    render_swd();    break;
    case MENU_CONFIG: render_config(); break;
    case MENU_PWM:    render_pwm();    break;
    case MENU_SPI:    render_spi();    break;
    case MENU_I2C:    render_i2c();    break;
    case MENU_AI:     render_ai();     break;
    default:          render_home();   break;
    }
    lcd_flush();
}

int menu_get_current_page(void) { return (int)s_current_page; }
int menu_get_selected(void) { return s_selected; }

int menu_get_rx_hist_n(void)   { return s_rx_line_n; }
int menu_get_rx_hist_max(void) { return s_rx_hist_max; }
void menu_set_rx_hist_max(int m)
{
    if (m < 2) m = 2;
    if (m > RX_LINE_CAP) m = RX_LINE_CAP;
    s_rx_hist_max = m;
    s_rx_line_n = 0;
    s_rx_line_w = 0;
}
void menu_clear_rx(void)
{
    s_rx_line_n = 0;
    s_rx_line_w = 0;
    s_rx_cur_col = 0;
}
