/* composer.h – Composer-Fenster (Prototyp #337)
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define REVOLUTION_TYPE_COMPOSER (revolution_composer_get_type ())
G_DECLARE_FINAL_TYPE (RevolutionComposer, revolution_composer, REVOLUTION, COMPOSER, AdwApplicationWindow)

GtkWindow *revolution_composer_new (GtkApplication *application);

G_END_DECLS
