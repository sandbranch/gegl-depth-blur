project('gegl-depth-blur', 'c',
  version: '0.1',
  license: 'GPL-3.0-or-later',
  meson_version: '>= 0.60.0',
  default_options: ['warning_level=1', 'buildtype=debugoptimized'],
)

gegl_dep = dependency('gegl-0.4', version: '>= 0.4.62')
math_dep = meson.get_compiler('c').find_library('m', required: false)

# GEGL loads user operations from $XDG_DATA_HOME/gegl-0.4/plug-ins, which is
# ~/.local/share/gegl-0.4/plug-ins, or for the Flatpak version of GIMP
# ~/.var/app/org.gimp.GIMP/data/gegl-0.4/plug-ins.
moduledir = get_option('moduledir')
if moduledir == ''
  moduledir = gegl_dep.get_variable(pkgconfig: 'pluginsdir')
endif

# each operation is a module of its own
operations = {
  'depth-blur': ['operations/depth-blur.c'],
}

modules = {}
foreach op, sources : operations
  modules += { op: shared_module(op,
    sources,
    name_prefix: '',
    include_directories: 'operations',
    dependencies: [gegl_dep, math_dep],
    c_args: ['-DGETTEXT_PACKAGE="gegl-depth-blur"'],
    install: true,
    install_dir: moduledir,
  )}
endforeach

# numerical checks of the operation: meson test -C build
check = executable('check', 'tests/check.c',
  dependencies: [gegl_dep, math_dep],
  c_args: ['-DGEGL_PLUGINSDIR="@0@"'.format(
    gegl_dep.get_variable(pkgconfig: 'pluginsdir'))],
  install: false,
)
test('depth-blur', check, args: [modules['depth-blur']], timeout: 600)
