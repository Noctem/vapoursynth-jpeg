project('vapoursynth-jpeg', 'c',
    default_options: ['buildtype=release', 'b_ndebug=if-release', 'b_lto=true'],
    meson_version: '>=0.46.0',
    version: '0.1')

shared_module('vapoursynth-jpeg',
    sources: ['jpeg.c'],
    dependencies: [dependency('libturbojpeg'), dependency('vapoursynth').partial_dependency(compile_args: true, includes: true)],
    cpp_args: ['-march=native', '-Ofast'],
    install: true)
