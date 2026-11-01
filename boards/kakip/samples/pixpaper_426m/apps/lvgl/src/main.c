/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * LVGL on the PIXPAPER-426-M (800x480 e-paper) from the Kakip Cortex-M33.
 *
 * Default: an e-paper dashboard (clock, gauge, bar, chart, checkboxes) that
 * changes once a second, so every second is one partial refresh.
 * Build with -DCONFIG_LV_USE_DEMO_WIDGETS=y to run LVGL's own widgets demo.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>
#include <lvgl.h>
#include <lvgl_zephyr.h>
#ifdef CONFIG_LV_USE_DEMO_WIDGETS
#include <lv_demos.h>
#endif

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

#ifndef CONFIG_LV_USE_DEMO_WIDGETS
static lv_obj_t *clock_lbl, *arc, *arc_lbl, *bar, *bar_lbl;
static lv_chart_series_t *ser;
static lv_obj_t *chart;

static lv_obj_t *card(lv_obj_t *parent, const char *title, int x, int y, int w, int h)
{
	lv_obj_t *c = lv_obj_create(parent);

	lv_obj_set_pos(c, x, y);
	lv_obj_set_size(c, w, h);
	lv_obj_set_style_border_width(c, 2, 0);
	lv_obj_set_style_radius(c, 8, 0);
	lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);

	lv_obj_t *t = lv_label_create(c);

	lv_label_set_text(t, title);
	lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, -6);
	return c;
}

static void build_dashboard(void)
{
	lv_obj_t *scr = lv_screen_active();

	lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

	lv_obj_t *title = lv_label_create(scr);

	lv_label_set_text(title, "Kakip RZ/V2H  |  Cortex-M33  |  Zephyr + LVGL " LVGL_VERSION_INFO);
	lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

	/* Clock */
	lv_obj_t *c = card(scr, "Uptime", 16, 60, 372, 150);

	clock_lbl = lv_label_create(c);
	lv_obj_set_style_text_font(clock_lbl, &lv_font_montserrat_48, 0);
	lv_obj_center(clock_lbl);

	/* Gauge */
	c = card(scr, "Gauge", 404, 60, 380, 150);
	arc = lv_arc_create(c);
	lv_obj_set_size(arc, 110, 110);
	lv_obj_align(arc, LV_ALIGN_LEFT_MID, 10, 8);
	lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
	arc_lbl = lv_label_create(c);
	lv_obj_set_style_text_font(arc_lbl, &lv_font_montserrat_48, 0);
	lv_obj_align(arc_lbl, LV_ALIGN_RIGHT_MID, -20, 8);

	/* Bar */
	c = card(scr, "Progress", 16, 226, 372, 100);
	bar = lv_bar_create(c);
	lv_obj_set_size(bar, 320, 24);
	lv_obj_align(bar, LV_ALIGN_LEFT_MID, 0, 0);
	lv_obj_set_style_border_width(bar, 2, LV_PART_MAIN);
	bar_lbl = lv_label_create(c);
	lv_obj_align(bar_lbl, LV_ALIGN_BOTTOM_RIGHT, 0, 8);

	/* Checkboxes */
	c = card(scr, "Tasks", 16, 342, 372, 122);
	static const char *const tasks[] = { "Boot Zephyr on M33", "Drive RSPI0", "Run LVGL" };

	for (int i = 0; i < ARRAY_SIZE(tasks); i++) {
		lv_obj_t *cb = lv_checkbox_create(c);

		lv_checkbox_set_text(cb, tasks[i]);
		lv_obj_add_state(cb, LV_STATE_CHECKED);
		lv_obj_set_pos(cb, 0, 14 + i * 30);
	}

	/* Chart */
	c = card(scr, "Signal", 404, 226, 380, 238);
	chart = lv_chart_create(c);
	lv_obj_set_size(chart, 340, 180);
	lv_obj_align(chart, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
	lv_chart_set_point_count(chart, 30);
	lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
	ser = lv_chart_add_series(chart, lv_color_black(), LV_CHART_AXIS_PRIMARY_Y);
}

static void tick(lv_timer_t *t)
{
	static uint32_t n;
	uint32_t s = k_uptime_get() / 1000;
	int pct = n % 101;

	n++;
	lv_label_set_text_fmt(clock_lbl, "%02u:%02u:%02u", s / 3600, (s / 60) % 60, s % 60);
	lv_arc_set_value(arc, (n * 7) % 100);
	lv_label_set_text_fmt(arc_lbl, "%d", (int)lv_arc_get_value(arc));
	lv_bar_set_value(bar, pct, LV_ANIM_OFF);
	lv_label_set_text_fmt(bar_lbl, "%d %%", pct);
	/* triangle-ish wave, no libm */
	lv_chart_set_next_value(chart, ser, 50 + ((int)(n % 20) - 10) * ((n / 20) % 2 ? -4 : 4));
}
#endif

int main(void)
{
	const struct device *dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

	if (!device_is_ready(dev)) {
		LOG_ERR("display not ready");
		return 0;
	}

	lvgl_lock();
	lv_display_t *disp = lv_display_get_default();

	lv_display_set_theme(disp, lv_theme_mono_init(disp, false, &lv_font_montserrat_14));
#ifdef CONFIG_LV_USE_DEMO_WIDGETS
	lv_demo_widgets();
#else
	build_dashboard();
	lv_timer_create(tick, 1000, NULL);
#endif
	lv_timer_handler();
	lvgl_unlock();

	display_blanking_off(dev);
	LOG_INF("LVGL running");

	while (1) {
		uint32_t ms;

		lvgl_lock();
		ms = lv_timer_handler();
		lvgl_unlock();
		k_msleep(MIN(ms, 100));
	}
	return 0;
}
