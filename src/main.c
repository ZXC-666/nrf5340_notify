/*
 * Notification mirror for nRF5340 DK + ILI9341.
 *
 * Phone (Android + Gadgetbridge) -> BLE (Nordic UART Service) -> JSON
 *   {"t":"notify","id":123,"src":"email","sender":"Alice",
 *    "title":"...","body":"..."}
 * -> parsed -> rendered by LVGL onto the ILI9341.
 *
 * The BLE side speaks the same NUS UUID as ZSWatch, so a stock Gadgetbridge
 * with ZSWatch device support can connect and forward Android notifications.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/settings/settings.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <bluetooth/services/nus.h>

#include <hal/nrf_clock.h>

#include <lvgl.h>

LOG_MODULE_REGISTER(notify, LOG_LEVEL_INF);

#define DEVICE_NAME CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

/* SPIM may use 32 MHz only with HFCLK at 128 MHz; must run before the display
 * driver's POST_KERNEL init or its 16 MHz fallback is cached. */
static int enable_128mhz_hfclk(void)
{
	nrf_clock_hfclk_div_set(NRF_CLOCK, NRF_CLOCK_HFCLK_DIV_1);
	return 0;
}
SYS_INIT(enable_128mhz_hfclk, PRE_KERNEL_1, 0);

/* The ILI9341 module is physically wired horizontally mirrored: LVGL renders
 * in logical coordinates, so this flush callback mirrors every row in place
 * and flips the X offset before handing the buffer to the panel driver. */
static const struct device *mirror_disp_dev;

static void mirror_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
	uint16_t w = (uint16_t)(area->x2 - area->x1 + 1);
	uint16_t h = (uint16_t)(area->y2 - area->y1 + 1);
	uint16_t *px = (uint16_t *)px_map;

	/* Mirror every horizontal row in place and emit each RGB565 word high
	 * byte first: the panel latches the MSB first while LVGL renders
	 * little-endian words. */
	for (uint16_t r = 0; r < h; r++) {
		uint16_t *row = px + (size_t)r * w;

		for (uint16_t c = 0; c < (w + 1U) / 2U; c++) {
			uint16_t left = row[c];
			uint16_t right = row[w - 1U - c];

			row[c] = sys_cpu_to_be16(right);
			row[w - 1U - c] = sys_cpu_to_be16(left);
		}
	}

	struct display_buffer_descriptor desc = {
		.buf_size = w * 2U * h,
		.width = w,
		.height = h,
		.pitch = w,
	};

	uint16_t x = (uint16_t)(lv_display_get_horizontal_resolution(disp) - 1U - area->x2);
	uint16_t y = (uint16_t)area->y1;

	if (display_write(mirror_disp_dev, x, y, &desc, px)) {
		LOG_ERR("display_write failed");
	}

	lv_display_flush_ready(disp);
}

/* ---------- UI state ---------- */
static lv_obj_t *icon_bg;
static lv_obj_t *icon_label;
static lv_obj_t *header_label;
static lv_obj_t *title_label;
static lv_obj_t *meta_label;
static lv_obj_t *body_label;

/* Parsed notification, filled by BLE rx context, read by the LVGL main loop. */
static char g_src[64];
static char g_title[256];
static char g_meta[256];
static char g_body[1600];
static atomic_t has_notification;

/* ---------- BLE state ---------- */
static struct bt_conn *current_conn;
static struct k_work adv_work;

#define RX_BUF_MAX 2048
static uint8_t rx_buf[RX_BUF_MAX];
static size_t rx_len;

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NUS_VAL),
};

/* ---------- Minimal JSON field extraction ---------- */
static char *find_key(const char *key, char *data)
{
	return strstr(data, key);
}

static uint32_t extract_uint32(const char *key, char *data)
{
	char *p = strstr(data, key);

	if (p == NULL) {
		return 0;
	}
	return strtoul(p + strlen(key), NULL, 10);
}

static bool hex4(const char *s, uint32_t *val)
{
	uint32_t v = 0;

	for (int i = 0; i < 4; i++) {
		char c = s[i];

		v <<= 4;
		if (c >= '0' && c <= '9') {
			v |= (uint32_t)(c - '0');
		} else if (c >= 'a' && c <= 'f') {
			v |= (uint32_t)(c - 'a' + 10);
		} else if (c >= 'A' && c <= 'F') {
			v |= (uint32_t)(c - 'A' + 10);
		} else {
			return false;
		}
	}
	*val = v;
	return true;
}

static void utf8_append(char *out, size_t *n, size_t out_len, uint32_t cp)
{
	size_t need;

	if (cp < 0x80U) {
		need = 1;
	} else if (cp < 0x800U) {
		need = 2;
	} else if (cp < 0x10000U) {
		need = 3;
	} else {
		need = 4;
	}
	if (*n + need >= out_len) {
		return;
	}

	if (need == 1) {
		out[(*n)++] = (char)cp;
	} else if (need == 2) {
		out[(*n)++] = (char)(0xC0U | (cp >> 6));
		out[(*n)++] = (char)(0x80U | (cp & 0x3FU));
	} else if (need == 3) {
		out[(*n)++] = (char)(0xE0U | (cp >> 12));
		out[(*n)++] = (char)(0x80U | ((cp >> 6) & 0x3FU));
		out[(*n)++] = (char)(0x80U | (cp & 0x3FU));
	} else {
		out[(*n)++] = (char)(0xF0U | (cp >> 18));
		out[(*n)++] = (char)(0x80U | ((cp >> 12) & 0x3FU));
		out[(*n)++] = (char)(0x80U | ((cp >> 6) & 0x3FU));
		out[(*n)++] = (char)(0x80U | (cp & 0x3FU));
	}
}

/* Copies the string value after "key": into out, decoding the JSON escapes
 * Gadgetbridge uses (all non-ASCII text arrives as \uXXXX). */
static bool extract_str(const char *key, char *data, char *out, size_t out_len)
{
	char *p = strstr(data, key);
	size_t n = 0;

	if (p == NULL) {
		return false;
	}
	p += strlen(key);
	if (*p != '"') {
		return false;
	}
	p++;

	while (*p != '\0' && *p != '"' && n < out_len - 1U) {
		if (*p != '\\') {
			out[n++] = *p++;
			continue;
		}

		p++;
		switch (*p) {
		case 'n':
			out[n++] = '\n';
			p++;
			break;
		case 'r':
			out[n++] = '\r';
			p++;
			break;
		case 't':
			out[n++] = '\t';
			p++;
			break;
		case '"':
		case '\\':
		case '/':
			out[n++] = *p++;
			break;
		case 'u': {
			uint32_t cp;

			p++;
			if (!hex4(p, &cp)) {
				break;
			}
			p += 4;
			/* Emoji and other astral characters arrive as a surrogate pair. */
			if (cp >= 0xD800U && cp <= 0xDBFFU && p[0] == '\\' && p[1] == 'u') {
				uint32_t lo;

				if (hex4(p + 2, &lo) && lo >= 0xDC00U && lo <= 0xDFFFU) {
					cp = 0x10000U + ((cp - 0xD800U) << 10) + (lo - 0xDC00U);
					p += 6;
				}
			}
			if (cp != 0U) {
				utf8_append(out, &n, out_len, cp);
			}
			break;
		}
		default:
			p++;
			break;
		}
	}
	out[n] = '\0';
	return true;
}

static void handle_notify(char *json)
{
	uint32_t id = extract_uint32("\"id\":", json);

	extract_str("\"title\":", json, g_title, sizeof(g_title));
	extract_str("\"body\":", json, g_body, sizeof(g_body));

	char sender[128] = "";

	extract_str("\"src\":", json, g_src, sizeof(g_src));
	extract_str("\"sender\":", json, sender, sizeof(sender));

	if (g_title[0] == '\0') {
		const char *fallback = sender[0] ? sender : g_src;

		strncpy(g_title, fallback, sizeof(g_title) - 1U);
		g_title[sizeof(g_title) - 1U] = '\0';
	}

	/* Do not repeat the same text on the header, title and subtitle lines. */
	if (sender[0] != '\0' && strcmp(sender, g_title) != 0) {
		snprintf(g_meta, sizeof(g_meta), "%s · %s", g_src, sender);
	} else if (g_src[0] != '\0' && strcmp(g_src, g_title) != 0) {
		strncpy(g_meta, g_src, sizeof(g_meta) - 1U);
		g_meta[sizeof(g_meta) - 1U] = '\0';
	} else {
		g_meta[0] = '\0';
	}

	LOG_INF("notify id=%u src='%s' sender='%s'", id, g_src, sender);
	LOG_INF("  title='%s'", g_title);
	LOG_INF("  body='%s'", g_body);

	atomic_set(&has_notification, 1);
}

static void handle_json(char *json, size_t len)
{
	char type[16] = "";

	LOG_INF("NUS JSON len=%u: '%s'", (unsigned int)len, json);
	if (!extract_str("\"t\":", json, type, sizeof(type))) {
		LOG_WRN("no type field");
		return;
	}

	if (strcmp(type, "notify") == 0) {
		handle_notify(json);
	} else if (strcmp(type, "notify-remove") == 0) {
		LOG_INF("notify-remove ignored");
	} else {
		LOG_INF("unhandled type '%s'", type);
	}
}

static void nus_received(struct bt_conn *conn, const uint8_t *data, uint16_t len)
{
	ARG_UNUSED(conn);
	LOG_INF("NUS rx: %u byte(s)", len);
	LOG_HEXDUMP_INF(data, len, "NUS rx data");

	for (uint16_t i = 0; i < len; i++) {
		/* Gadgetbridge normally sends newline-delimited JSON.  Treat NUL as
		 * a delimiter too: some phone-side transports use C-string framing. */
		if (data[i] == '\n' || data[i] == '\r' || data[i] == '\0') {
			if (rx_len > 0) {
				rx_buf[rx_len] = '\0';
				handle_json((char *)rx_buf, rx_len);
				rx_len = 0;
			}
		} else if (rx_len < RX_BUF_MAX - 1U) {
			rx_buf[rx_len++] = data[i];
		} else {
			LOG_WRN("NUS JSON exceeds %u bytes; dropping frame", RX_BUF_MAX - 1U);
			rx_len = 0;
		}
	}
}

static struct bt_nus_cb nus_cb = {
	.received = nus_received,
};

static void adv_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad),
				  sd, ARRAY_SIZE(sd));

	if (err) {
		LOG_ERR("advertising failed (err %d)", err);
	} else {
		LOG_INF("advertising started as '%s'", DEVICE_NAME);
	}
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		LOG_ERR("connection failed (err %u)", err);
		return;
	}
	current_conn = bt_conn_ref(conn);
	LOG_INF("connected");
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(reason);
	if (current_conn) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}
	LOG_INF("disconnected, restarting advertising");
	k_work_submit(&adv_work);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
};

/* ---------- UI ---------- */
#define COL_BG     lv_color_hex(0x0B1014)
#define COL_HEADER lv_color_hex(0x16222C)
#define COL_ACCENT lv_color_hex(0x4CC2FF)
#define COL_TITLE  lv_color_hex(0xFFFFFF)
#define COL_META   lv_color_hex(0x8399AD)
#define COL_BODY   lv_color_hex(0xD3DEE9)
#define COL_LINE   lv_color_hex(0x243441)

/* Gadgetbridge only forwards text over the Bangle.js channel, so the real app
 * icon never reaches the watch. ZSWatch solves this by letting the phone send
 * an icon id and shipping the bitmaps in firmware; here we map the app name to
 * one of LVGL's built-in symbol glyphs and fall back to a coloured initial
 * avatar for everything else. */
static const struct {
	const char *keyword;
	const char *symbol;
} icon_rules[] = {
	{ "mail", LV_SYMBOL_ENVELOPE },
	{ "gmail", LV_SYMBOL_ENVELOPE },
	{ "outlook", LV_SYMBOL_ENVELOPE },
	{ "邮件", LV_SYMBOL_ENVELOPE },
	{ "邮箱", LV_SYMBOL_ENVELOPE },
	{ "sms", LV_SYMBOL_ENVELOPE },
	{ "短信", LV_SYMBOL_ENVELOPE },
	{ "call", LV_SYMBOL_CALL },
	{ "phone", LV_SYMBOL_CALL },
	{ "电话", LV_SYMBOL_CALL },
	{ "通话", LV_SYMBOL_CALL },
	{ "music", LV_SYMBOL_AUDIO },
	{ "音乐", LV_SYMBOL_AUDIO },
	{ "spotify", LV_SYMBOL_AUDIO },
	{ "calendar", LV_SYMBOL_LIST },
	{ "日历", LV_SYMBOL_LIST },
	{ "alarm", LV_SYMBOL_BELL },
	{ "闹钟", LV_SYMBOL_BELL },
};

static const char *icon_symbol_for(const char *src)
{
	char lower[64];
	size_t i = 0;

	for (; src[i] != '\0' && i < sizeof(lower) - 1U; i++) {
		char c = src[i];

		lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
	}
	lower[i] = '\0';

	for (size_t r = 0; r < ARRAY_SIZE(icon_rules); r++) {
		if (strstr(lower, icon_rules[r].keyword) != NULL) {
			return icon_rules[r].symbol;
		}
	}
	return NULL;
}

static void icon_set(const char *src)
{
	static const uint32_t palette[] = {
		0xE4572E, 0x17A398, 0x3A7DDE, 0x9B5DE5,
		0xF2A93B, 0x2E9E5B, 0xD64C7C, 0x5B6EE1,
	};
	const char *symbol = icon_symbol_for(src);
	uint32_t hash = 0;
	char first[5] = {0};
	uint8_t c = (uint8_t)src[0];
	size_t len = 1U;

	for (const char *p = src; *p != '\0'; p++) {
		hash = hash * 31U + (uint8_t)*p;
	}

	if (c >= 0xF0U) {
		len = 4U;
	} else if (c >= 0xE0U) {
		len = 3U;
	} else if (c >= 0xC0U) {
		len = 2U;
	} else if (c >= 'a' && c <= 'z') {
		first[0] = (char)(c - ('a' - 'A'));
	}

	if (len > 1U) {
		memcpy(first, src, MIN(len, strlen(src)));
	}

	lv_obj_set_style_bg_color(icon_bg, lv_color_hex(palette[hash % ARRAY_SIZE(palette)]), 0);

	if (symbol != NULL) {
		lv_obj_set_style_text_font(icon_label, &lv_font_montserrat_20, 0);
		lv_label_set_text(icon_label, symbol);
	} else {
		lv_obj_set_style_text_font(icon_label, &lv_font_source_han_sans_sc_16_cjk, 0);
		lv_label_set_text(icon_label, first[0] ? first : "?");
	}
}

static void ui_init(void)
{
	lv_obj_t *scr = lv_screen_active();
	lv_obj_t *header;
	lv_obj_t *divider;

	lv_obj_set_style_bg_color(scr, COL_BG, 0);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
	lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

	/* App name strip. */
	header = lv_obj_create(scr);
	lv_obj_remove_style_all(header);
	lv_obj_set_pos(header, 0, 0);
	lv_obj_set_size(header, 320, 34);
	lv_obj_set_style_bg_color(header, COL_HEADER, 0);
	lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
	lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

	icon_bg = lv_obj_create(header);
	lv_obj_remove_style_all(icon_bg);
	lv_obj_set_pos(icon_bg, 10, 4);
	lv_obj_set_size(icon_bg, 26, 26);
	lv_obj_set_style_radius(icon_bg, 8, 0);
	lv_obj_set_style_bg_opa(icon_bg, LV_OPA_COVER, 0);
	lv_obj_clear_flag(icon_bg, LV_OBJ_FLAG_SCROLLABLE);

	icon_label = lv_label_create(icon_bg);
	lv_obj_center(icon_label);
	lv_obj_set_style_text_color(icon_label, lv_color_hex(0xFFFFFF), 0);

	header_label = lv_label_create(header);
	lv_obj_set_pos(header_label, 46, 9);
	lv_obj_set_width(header_label, 262);
	lv_label_set_long_mode(header_label, LV_LABEL_LONG_DOT);
	lv_obj_set_style_text_color(header_label, COL_ACCENT, 0);

	title_label = lv_label_create(scr);
	lv_obj_set_pos(title_label, 12, 46);
	lv_obj_set_width(title_label, 296);
	lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);
	lv_obj_set_style_text_color(title_label, COL_TITLE, 0);

	meta_label = lv_label_create(scr);
	lv_obj_set_pos(meta_label, 12, 72);
	lv_obj_set_width(meta_label, 296);
	lv_label_set_long_mode(meta_label, LV_LABEL_LONG_DOT);
	lv_obj_set_style_text_color(meta_label, COL_META, 0);

	divider = lv_obj_create(scr);
	lv_obj_remove_style_all(divider);
	lv_obj_set_pos(divider, 12, 96);
	lv_obj_set_size(divider, 296, 1);
	lv_obj_set_style_bg_color(divider, COL_LINE, 0);
	lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);

	body_label = lv_label_create(scr);
	lv_obj_set_pos(body_label, 12, 106);
	lv_obj_set_width(body_label, 296);
	lv_label_set_long_mode(body_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_color(body_label, COL_BODY, 0);
	lv_obj_set_style_text_line_space(body_label, 4, 0);

	lv_label_set_text(header_label, "notification");
	lv_label_set_text(title_label, "No notification yet");
	lv_label_set_text(body_label, "Waiting for Gadgetbridge...");
	lv_obj_add_flag(meta_label, LV_OBJ_FLAG_HIDDEN);
	icon_set("notification");
}

static void ui_apply_notification(void)
{
	lv_label_set_text(header_label, g_src[0] ? g_src : "notification");
	lv_label_set_text(title_label, g_title);
	lv_label_set_text(body_label, g_body);
	icon_set(g_src[0] ? g_src : "notification");

	if (g_meta[0] != '\0') {
		lv_label_set_text(meta_label, g_meta);
		lv_obj_clear_flag(meta_label, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_add_flag(meta_label, LV_OBJ_FLAG_HIDDEN);
	}

	LOG_INF("ui applied: title='%s'", g_title);
	/* Force the refresh now instead of waiting for the next LVGL cycle, so a
	 * stalled refresh timer cannot leave the panel showing stale content. */
	lv_refr_now(lv_display_get_default());
}

int main(void)
{
	const struct device *display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	int err;

	if (!device_is_ready(display)) {
		LOG_ERR("ILI9341 display not ready");
		return -ENODEV;
	}
	err = display_blanking_off(display);
	if (err < 0 && err != -ENOSYS) {
		LOG_ERR("display blanking_off failed (%d)", err);
	}

	/* The ILI9341 module is horizontally mirrored; override LVGL's default
	 * flush callback to un-mirror every update on the way to the panel. */
	mirror_disp_dev = display;
	lv_display_set_flush_cb(lv_display_get_default(), mirror_flush_cb);

	ui_init();

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("bt_enable failed (%d)", err);
		return err;
	}
	LOG_INF("Bluetooth enabled");

	/* CONFIG_BT_SETTINGS is enabled, so the host does not create an identity
	 * address on its own: it has to be restored here before any connectable
	 * advertising is started, otherwise bt_le_adv_start() returns -EAGAIN.
	 */
	if (IS_ENABLED(CONFIG_SETTINGS)) {
		err = settings_load();
		if (err) {
			LOG_ERR("settings_load failed (%d)", err);
			return err;
		}
	}

	err = bt_nus_init(&nus_cb);
	if (err) {
		LOG_ERR("bt_nus_init failed (%d)", err);
		return err;
	}

	k_work_init(&adv_work, adv_work_handler);
	k_work_submit(&adv_work);

	while (1) {
		if (atomic_get(&has_notification) != 0) {
			ui_apply_notification();
			atomic_set(&has_notification, 0);
		}
		lv_timer_handler();
		k_sleep(K_MSEC(10));
	}
}
