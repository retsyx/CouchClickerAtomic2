/*
 * ScreenKey displays. Each ScreenKey shows a happy face at boot, and each
 * press of its switch (a ZMK key position) toggles it between happy and sad.
 *
 * Key events only flag a redraw; a low-priority thread renders and sends the
 * faces, so ZMK's event processing is never blocked by SPI transfers.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>

#include "face.h"

LOG_MODULE_REGISTER(cca_screenkey, CONFIG_CCA_SCREENKEY_LOG_LEVEL);

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(cca_screenkeys) == 1,
	     "Exactly one cca,screenkeys node is supported");

#define SCREENKEYS_NODE DT_INST(0, cca_screenkeys)

/* Faces are drawn in horizontal strips to keep the DMA buffer small. */
#define STRIP_ROWS 16
BUILD_ASSERT(FACE_HEIGHT % STRIP_ROWS == 0, "Strips must tile the face");

struct screenkey {
	const struct device *display;
	uint32_t position;
};

#define SCREENKEY_ENTRY(node)                                                  \
	{                                                                      \
		.display = DEVICE_DT_GET(DT_PHANDLE(node, display)),           \
		.position = DT_PROP(node, key_position),                       \
	},

static const struct screenkey screenkeys[] = {
	DT_FOREACH_CHILD_STATUS_OKAY(SCREENKEYS_NODE, SCREENKEY_ENTRY)};

BUILD_ASSERT(ARRAY_SIZE(screenkeys) > 0, "No ScreenKeys defined");
BUILD_ASSERT(ARRAY_SIZE(screenkeys) <= ATOMIC_BITS, "Too many ScreenKeys");

/* Bit i: ScreenKey i shows the sad face. */
static atomic_t sad_faces;
/* Bit i: ScreenKey i needs redrawing. */
static atomic_t dirty;
static K_SEM_DEFINE(redraw_sem, 0, 1);

/* Static RAM is required by EasyDMA. */
static uint8_t strip[FACE_WIDTH * STRIP_ROWS * FACE_BYTES_PER_PIXEL] __aligned(4);

#if DT_NODE_HAS_PROP(SCREENKEYS_NODE, reset_gpios)
BUILD_ASSERT(CONFIG_CCA_SCREENKEY_RESET_INIT_PRIORITY < CONFIG_DISPLAY_INIT_PRIORITY,
	     "The shared reset must run before the display drivers initialize");

/*
 * Pulse the shared reset line once, before any display driver runs. Display
 * drivers that each pulsed it would wipe panels initialized before them.
 */
static int screenkey_reset_displays(void)
{
	const struct gpio_dt_spec reset = GPIO_DT_SPEC_GET(SCREENKEYS_NODE, reset_gpios);
	int err;

	if (!gpio_is_ready_dt(&reset)) {
		return -ENODEV;
	}
	err = gpio_pin_configure_dt(&reset, GPIO_OUTPUT_ACTIVE);
	if (err < 0) {
		return err;
	}
	k_sleep(K_MSEC(1));
	err = gpio_pin_set_dt(&reset, 0);
	if (err < 0) {
		return err;
	}
	/* The ST7735 accepts commands 120 ms after reset is released. */
	k_sleep(K_MSEC(120));
	return 0;
}

SYS_INIT(screenkey_reset_displays, POST_KERNEL, CONFIG_CCA_SCREENKEY_RESET_INIT_PRIORITY);
#endif

#if DT_NODE_HAS_PROP(SCREENKEYS_NODE, pwms)
static const struct pwm_dt_spec backlight = PWM_DT_SPEC_GET(SCREENKEYS_NODE);

static int backlight_init(void)
{
	return pwm_is_ready_dt(&backlight) ? 0 : -ENODEV;
}

static int backlight_set(uint32_t percent)
{
	return pwm_set_pulse_dt(&backlight, backlight.period * percent / 100U);
}
#else
static int backlight_init(void)
{
	return 0;
}

static int backlight_set(uint32_t percent)
{
	ARG_UNUSED(percent);
	return 0;
}
#endif

static int screenkey_check_display(const struct device *display)
{
	struct display_capabilities caps;

	if (!device_is_ready(display)) {
		LOG_ERR("Display %s is not ready", display->name);
		return -ENODEV;
	}
	display_get_capabilities(display, &caps);
	if (caps.x_resolution != FACE_WIDTH || caps.y_resolution != FACE_HEIGHT ||
	    caps.current_pixel_format != PIXEL_FORMAT_RGB_565) {
		LOG_ERR("Display %s is not a %dx%d RGB565 panel", display->name, FACE_WIDTH,
			FACE_HEIGHT);
		return -ENOTSUP;
	}
	return 0;
}

static int screenkey_draw_face(const struct device *display, bool happy)
{
	const struct display_buffer_descriptor desc = {
		.buf_size = sizeof(strip),
		.width = FACE_WIDTH,
		.height = STRIP_ROWS,
		.pitch = FACE_WIDTH,
	};

	for (int y = 0; y < FACE_HEIGHT; y += STRIP_ROWS) {
		int err;

		face_render_rows(strip, happy, y, STRIP_ROWS);
		err = display_write(display, 0, y, &desc, strip);
		if (err < 0) {
			return err;
		}
	}
	return 0;
}

static void screenkey_redraw_dirty(atomic_val_t usable)
{
	const atomic_val_t pending = atomic_clear(&dirty) & usable;
	const atomic_val_t sad = atomic_get(&sad_faces);

	for (size_t i = 0; i < ARRAY_SIZE(screenkeys); i++) {
		if (!(pending & BIT(i))) {
			continue;
		}
		const int err = screenkey_draw_face(screenkeys[i].display, !(sad & BIT(i)));

		if (err < 0) {
			LOG_ERR("Drawing ScreenKey %zu failed (%d)", i, err);
		}
	}
}

static void screenkey_thread(void)
{
	atomic_val_t usable = 0;

	if (backlight_init() < 0 || backlight_set(0) < 0) {
		LOG_ERR("Backlight PWM is not ready");
	}
	for (size_t i = 0; i < ARRAY_SIZE(screenkeys); i++) {
		if (screenkey_check_display(screenkeys[i].display) == 0) {
			usable |= BIT(i);
		}
	}

	/* Draw every face before turning the displays and backlight on. */
	atomic_set(&dirty, usable);
	screenkey_redraw_dirty(usable);
	for (size_t i = 0; i < ARRAY_SIZE(screenkeys); i++) {
		if ((usable & BIT(i)) && display_blanking_off(screenkeys[i].display) < 0) {
			LOG_ERR("Turning on ScreenKey %zu failed", i);
		}
	}
	if (backlight_set(CONFIG_CCA_SCREENKEY_BACKLIGHT_PERCENT) < 0) {
		LOG_ERR("Setting the backlight failed");
	}

	while (true) {
		k_sem_take(&redraw_sem, K_FOREVER);
		screenkey_redraw_dirty(usable);
	}
}

K_THREAD_DEFINE(cca_screenkey_thread, CONFIG_CCA_SCREENKEY_THREAD_STACK_SIZE, screenkey_thread,
		NULL, NULL, NULL, CONFIG_CCA_SCREENKEY_THREAD_PRIORITY, 0, 0);

static int screenkey_position_listener(const zmk_event_t *eh)
{
	const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

	if (ev == NULL || !ev->state) {
		return ZMK_EV_EVENT_BUBBLE;
	}
	for (size_t i = 0; i < ARRAY_SIZE(screenkeys); i++) {
		if (screenkeys[i].position == ev->position) {
			atomic_xor(&sad_faces, BIT(i));
			atomic_or(&dirty, BIT(i));
			k_sem_give(&redraw_sem);
		}
	}
	return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(cca_screenkey, screenkey_position_listener);
ZMK_SUBSCRIPTION(cca_screenkey, zmk_position_state_changed);
