/* eds-compat.c
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* Die einzige Stelle für Aufrufe, die von der EDS-/Camel-Version abhängen
 * (CLAUDE.md Architektur-Festlegung 6, README „Architektur“). Heute (EDS 3.56.2)
 * gibt es keinen solchen Aufruf; die Datei prüft nur beim Übersetzen die Version.
 * EDS_CHECK_VERSION: /usr/include/evolution-data-server/libedataserver/eds-version.h:63. */

#include <libedataserver/libedataserver.h>

#if !EDS_CHECK_VERSION (3, 56, 0)
#error "Revolution braucht Evolution-Data-Server 3.56 oder neuer"
#endif
