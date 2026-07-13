#ifndef M5STICK_DISPLAY_H
#define M5STICK_DISPLAY_H

#include "settings.h"

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
