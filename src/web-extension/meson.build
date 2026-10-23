# #354: Web-Process-Extension des Lesebereichs (Sperre aller Netzzugriffe), wie Stamp
# (quelltext/stamp/src/web-process-extension/meson.build). Pfad: WEB_EXTENSIONS_DIR (config.h).
shared_module('revolution-web-extension',
  'web-extension.c',
  dependencies: [
    dependency('webkitgtk-web-process-extension-6.0', version: '>= 2.52'),
  ],
  install: true,
  install_dir: web_extensions_dir,
)
