# Packaging metadata. Actual package building is done in CI / maintainer
# tooling; this meson file just carries metadata and sanity checks.

pkg_meta = custom_target(
    'packaging-metadata',
    output: 'packaging.marker',
    command: ['touch', '@OUTPUT@'],
    build_by_default: false,
)