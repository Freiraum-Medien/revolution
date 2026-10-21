desktop_file = i18n.merge_file(
        input: application_id + '.desktop.in',
       output: application_id + '.desktop',
         type: 'desktop',
       po_dir: '../po',
      install: true,
  install_dir: get_option('datadir') / 'applications',
)

desktop_utils = find_program('desktop-file-validate', required: false)
if desktop_utils.found()
  test('validate-desktop', desktop_utils, args: [desktop_file])
endif
