project('nc_runtime', 'c',
    default_options: ['buildtype=debug'],
    meson_version: '>= 0.61.2'
)

# Runtime library (for standalone builds/testing only).
# The deploy/ project references these source files directly by relative path.
nc_runtime_sources = [
  'nc_runtime_main.c',
  'nc_doca_flow.c',
  'nc_flexio_host.c',
  'nc_arm_worker.c',
]

nc_runtime_deps = [
  dependency('doca-common'),
  dependency('doca-flow'),
  dependency('doca-argp'),
  dependency('libdpdk'),
  dependency('libflexio'),    # FlexIO host library
  dependency('libibverbs'),   # ibverbs (mlx5dv.h)
  dependency('libmlx5'),      # mlx5 verbs (mlx5dv_*)
]

nc_runtime_inc = include_directories('include')

nc_runtime_lib = static_library('nc_runtime',
  nc_runtime_sources,
  include_directories: nc_runtime_inc,
  dependencies: nc_runtime_deps,
  c_args: ['-DDOCA_ALLOW_EXPERIMENTAL_API', '-Wno-missing-braces'],
)

nc_runtime_dep = declare_dependency(
  include_directories: nc_runtime_inc,
  link_with: nc_runtime_lib,
  dependencies: nc_runtime_deps,
)
