prop_dep = meson.get_compiler('c').find_library('prop')

upshared += { 'netbsd': static_library('upshared',
    sources: [
        'up-backend.c',
        'up-native.c',
    ],
    c_args: [ '-DG_LOG_DOMAIN="UPower-NetBSD"' ],
    dependencies: [ gudev_dep, upowerd_deps, upshared_common_dep, prop_dep ],
    build_by_default: false,
)}
