insert_sources = ['gstinsertbin.c']
insert_headers = ['gstinsertbin.h']
install_headers(insert_headers, subdir : 'gstreamer-1.0/gst/insertbin')

gstinsertbin = library('gstinsertbin-' + api_version,
  insert_sources,
  c_args : gst_plugins_bad_args + ['-DGST_USE_UNSTABLE_API', '-DBUILDING_GST_INSERT_BIN'],
  include_directories : [configinc, libsinc],
  version : libversion,
  soversion : soversion,
  darwin_versions : osxversion,
  install : true,
  dependencies : [gst_dep],
)

gen_sources = []
if build_gir
  insertbin_gir = gnome.generate_gir(gstinsertbin,
    sources : insert_sources + insert_headers,
    namespace : 'GstInsertBin',
    nsversion : api_version,
    identifier_prefix : 'Gst',
    symbol_prefix : 'gst',
    export_packages : 'gstreamer-insertbin-1.0',
    includes : ['Gst-1.0'],
    install : true,
    extra_args : gir_init_section + ['-DGST_USE_UNSTABLE_API'] + ['--c-include=gst/insertbin/gstinsertbin.h'],
    dependencies : [gst_dep]
  )
  gen_sources += insertbin_gir
endif

gstinsertbin_dep = declare_dependency(link_with : gstinsertbin,
  include_directories : [libsinc],
  sources: gen_sources,
  dependencies : [gst_dep])
