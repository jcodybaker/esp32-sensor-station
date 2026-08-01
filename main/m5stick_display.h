#ifndef M5STICK_DISPLAY_H
#define M5STICK_DISPLAY_H

#include "settings.h"

/**
 * @brief Run M5.begin(), powering on the display and the Grove-port power
 * rail (AXP192 EXTEN on M5StickC Plus).
 *
 * Call this synchronously, before initializing any sensor driver that may
 * depend on Grove-port power (e.g. an RCWL-9620 wired to the Grove pins) --
 * m5stick_display_init() below only starts an async render task, so it does
 * not by itself guarantee the power rail is up before sensor drivers run.
 *
 * When CONFIG_ENABLE_M5STICKC_DISPLAY is disabled this is a no-op, so callers
 * do not need to guard the call themselves.
 */
void m5stick_display_power_init(void);

/**
 * @brief Initialize the M5StickC Plus LCD display and start the render task.
 *
 * When CONFIG_ENABLE_M5STICKC_DISPLAY is disabled this is a no-op, so callers
 * do not need to guard the call themselves.
 *
 * @param settings Pointer to settings structure (must outlive the display task)
 */
void m5stick_display_init(settings_t *settings);

#endif // M5STICK_DISPLAY_H
