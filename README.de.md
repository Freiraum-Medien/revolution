# Revolution

[English](README.md)

Moderner Mail-Client für GNOME mit Mail, Kontakten und Kalender; die Konten kommen aus GNOME Online Accounts.

![Revolution, hell](data/screenshots/mail-hell.png)

![Revolution, dunkel](data/screenshots/mail-dunkel.png)

## Stand

| Meilenstein | Inhalt | Stand |
|---|---|---|
| M2 | Design | fertig |
| M3 | Gerüst | fertig |
| M4 | Mail lesen | fertig |
| M5 | Mail bearbeiten | fertig |
| M6 | Kontakte | offen |
| M7 | Kalender | offen |
| M8 | Release | offen |

## Unterbau

- Evolution-Data-Server 3.56 (Camel, libebook, libecal)
- GTK4 + libadwaita + Blueprint
- C

## Bauen (Ubuntu 26.04)

```sh
sudo apt install meson ninja-build blueprint-compiler gettext libgtk-4-dev libadwaita-1-dev \
  libcamel1.2-dev libedataserver1.2-dev libebook1.2-dev libwebkitgtk-6.0-dev \
  appstream desktop-file-utils libglib2.0-bin
meson setup build
meson compile -C build
GSETTINGS_SCHEMA_DIR=build/data ./build/src/revolution
```

`GSETTINGS_SCHEMA_DIR` zeigt auf das im Build-Ordner kompilierte Schema; nach `meson install` entfällt es.

## Übersetzungen

Die Quelltexte sind Englisch, Übersetzungen liegen in `po/` (Deutsch: `po/de.po`). Aus dem Build-Ordner gestartet nutzt die App die dort gebauten Übersetzungen.

## Mitwirken

Build, Tests, Architektur und Arbeitsweise: [docs/ENTWICKLUNG.md](docs/ENTWICKLUNG.md).

## Lizenz

GPL-3.0-or-later, siehe [COPYING](COPYING). Copyright 2026 Arnd Messer.
