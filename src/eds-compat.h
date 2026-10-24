/* eds-compat.h
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <camel/camel.h>

G_BEGIN_DECLS

gboolean revolution_eds_folder_refresh_info_sync (CamelFolder   *folder,
                                                  GCancellable  *cancellable,
                                                  GError       **error);

G_END_DECLS
