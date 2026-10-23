/* mail-sample.h – Listeneintrag des Prototyps (#336), Daten entfernt (#342)
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

#define REVOLUTION_TYPE_SAMPLE_MESSAGE (revolution_sample_message_get_type ())
G_DECLARE_FINAL_TYPE (RevolutionSampleMessage, revolution_sample_message, REVOLUTION, SAMPLE_MESSAGE, GObject)

G_END_DECLS
