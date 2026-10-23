/* mail-view.h – Mail-Ansicht (Prototyp #336)
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>
#include <libedataserver/libedataserver.h>

G_BEGIN_DECLS

#define REVOLUTION_TYPE_MAIL_VIEW (revolution_mail_view_get_type ())
G_DECLARE_FINAL_TYPE (RevolutionMailView, revolution_mail_view, REVOLUTION, MAIL_VIEW, AdwBreakpointBin)

void revolution_mail_view_set_registry (RevolutionMailView *self,
                                        ESourceRegistry    *registry);
void revolution_mail_view_show_error   (RevolutionMailView *self,
                                        const char         *title,
                                        const char         *detail);

G_END_DECLS
