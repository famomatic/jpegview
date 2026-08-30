vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO thorvg/thorvg
    REF "v${VERSION}"
    SHA512 a3a1e3c84c2a0f6ff174adccdd1d185b4433cd781fed22a0e6b07f1ee7f7402ed220bc92caade4faa872362e7319491e532485f37e62c00e15cee5c829914c48
    HEAD_REF master
    PATCHES
        # Teach the SVG loader that font-family/font-size are style properties.
        # Upstream only reads them as direct XML attributes on <text>/<tspan>, so
        # Illustrator output (which puts fonts in CSS classes) renders every string
        # in a fallback face at the 10px default.
        0001-svg-css-font.patch
        # Expand XML character entities in text content; without it a <text>
        # node holding &amp;/&lt;/&quot; renders the escape sequence itself.
        0002-svg-text-entities.patch
)

if ("tools" IN_LIST FEATURES)
    list(APPEND BUILD_OPTIONS -Dtools=all)
endif()

vcpkg_configure_meson(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${BUILD_OPTIONS}
        # see ${SOURCE_PATH}/meson_options.txt
        -Dstatic=true # Use static modules
        -Dengines=['cpu']
        # JPEGView uses ThorVG for SVG only, and links its own libwebp.  Building
        # thorvg's bundled webp loader duplicates WebPDecodeRGBA and friends, which
        # fails the link with LNK2005, so only the loaders actually needed are built:
        # svg for the documents themselves, ttf/otf for text, png/jpg for rasters
        # embedded in an SVG.
        -Dloaders=['svg','ttf','otf','png','jpg']
        -Dsavers=['']
        -Dsimd=true
        -Dbindings=capi
        -Dtests=false
        -Dstrip=false
        -Dextra=['']
    OPTIONS_DEBUG
        -Dlog=true
        -Dbindir=${CURRENT_PACKAGES_DIR}/debug/bin
    OPTIONS_RELEASE
        -Dbindir=${CURRENT_PACKAGES_DIR}/bin
)
vcpkg_install_meson()
vcpkg_fixup_pkgconfig()

if(VCPKG_LIBRARY_LINKAGE STREQUAL "static")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/thorvg-1/thorvg.h" "#ifndef TVG_STATIC" "#if 0")
else()
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/thorvg-1/thorvg.h" "#ifndef TVG_STATIC" "#if 1")
endif()

if ("tools" IN_LIST FEATURES)
    vcpkg_copy_tools(TOOL_NAMES tvg-svg2png tvg-lottie2gif AUTO_CLEAN)
endif()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
