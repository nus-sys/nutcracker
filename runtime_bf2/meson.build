project('nc_bf2_pipeline', 'c',
    default_options: ['buildtype=debug'],
    meson_version: '>= 0.61.2'
)

add_project_arguments('-DDOCA_ALLOW_EXPERIMENTAL_API', language: 'c')

pipeline_c = get_option('pipeline_c')

nc_srcs = [
    'nc_bf2_main.c',
    'flow_common.c',
    'common/dpdk_utils.c',
    'common/offload_rules.c',
    pipeline_c,
]

nc_deps = [
    dependency('doca'),
    dependency('libdpdk'),
]

nc_inc = [
    include_directories('.'),
    include_directories('common'),
]

executable('nc_pipeline', nc_srcs,
    c_args: ['-Wno-missing-braces'],
    dependencies: nc_deps,
    include_directories: nc_inc,
    install: false)
