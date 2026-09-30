dxgi_res = []
if not dxvk_native_ohos
  dxgi_res = wrc_generator.process('version.rc')
endif

dxgi_src = [
  'dxgi_adapter.cpp',
  'dxgi_enums.cpp',
  'dxgi_factory.cpp',
  'dxgi_format.cpp',
  'dxgi_main.cpp',
  'dxgi_monitor.cpp',
  'dxgi_options.cpp',
]

if not dxvk_native_ohos
  dxgi_src += files([
    'dxgi_output.cpp',
    'dxgi_swapchain.cpp',
  ])
endif

if dxvk_native_ohos
  dxgi_src += files('dxgi_ohos_window.cpp', 'dxgi_ohos_perf.cpp')
  dxgi_dll = shared_library('dxvk_dxgi', dxgi_src,
    dependencies        : [ dxvk_dep, lib_native_window ],
    include_directories : dxvk_include_path,
    install             : true,
    version             : dxvk_so_version,
  )
else
  dxgi_dll = shared_library('dxgi'+dll_ext, dxgi_src, dxgi_res,
    name_prefix         : '',
    dependencies        : [ dxvk_dep ],
    include_directories : dxvk_include_path,
    install             : true,
    vs_module_defs      : 'dxgi'+def_spec_ext,
  )
endif

dxgi_dep = declare_dependency(
  link_with           : [ dxgi_dll ],
  include_directories : [ dxvk_include_path ],
)
