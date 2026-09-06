set(KTX_FEATURE_STATIC_LIBRARY ON)
set(KTX_FEATURE_TOOLS OFF)
set(KTX_FEATURE_TESTS OFF)
set(KTX_FEATURE_VK_UPLOAD ON)
set(KTX_FEATURE_GL_UPLOAD OFF)
set(KTX_FEATURE_LOADTEST_APPS OFF)

set(FASTGLTF_COMPILE_AS_CPP20 ON)

set(SPIRV_REFLECT_EXECUTABLE OFF)
set(SPIRV_REFLECT_STATIC_LIB ON)
set(SPIRV_REFLECT_INSTALL OFF)

FetchContent_MakeAvailable(simdjson spdlog glm fastgltf imgui stb implot uni-algo entt spirv-reflect)
if(ANDROID AND WIVRN_USE_ANDROID_BIN_LIBKTX)
    if(NOT EXISTS ${FETCHCONTENT_BASE_DIR}/KTX-Software-${KTX_VERSION}-Android.zip)
        file(DOWNLOAD
            https://github.com/KhronosGroup/KTX-Software/releases/download/${KTX_VERSION_FULL}/KTX-Software-${KTX_VERSION}-Android.zip
            ${FETCHCONTENT_BASE_DIR}/KTX-Software-${KTX_VERSION}-Android.zip
            EXPECTED_HASH SHA256=9f84a395f37a185e2e21e77cfd3893e67271b616edf5feb39125f09ab4c06b29
            )
    endif()
    file(ARCHIVE_EXTRACT INPUT ${FETCHCONTENT_BASE_DIR}/KTX-Software-${KTX_VERSION}-Android.zip DESTINATION ${FETCHCONTENT_BASE_DIR})
    add_library(ktx INTERFACE)
    target_link_libraries(ktx INTERFACE ${FETCHCONTENT_BASE_DIR}/KTX-Software-${KTX_VERSION}-Android/${ANDROID_ABI}/lib/libktx.so)
    target_include_directories(ktx INTERFACE ${FETCHCONTENT_BASE_DIR}/KTX-Software-${KTX_VERSION}-Android/${ANDROID_ABI}/include)
elseif(WIVRN_USE_SYSTEM_LIBKTX AND NOT ANDROID)
else()
    FetchContent_MakeAvailable(libktx)
endif()

include(FreetypeHarfbuzz)
include(CompileGLSL)
include(GenerateAssets)
include(I18n)

add_library(stb INTERFACE)
target_include_directories(stb INTERFACE ${stb_SOURCE_DIR})

if(ANDROID)
    add_library(wivrn MODULE)

    find_library(ANDROID_LIBRARY NAMES android REQUIRED)
    find_library(MEDIA_LIBRARY NAMES mediandk REQUIRED)
    find_library(AAUDIO_LIBRARY NAMES aaudio REQUIRED)
    target_link_libraries(wivrn ${ANDROID_LIBRARY} ${MEDIA_LIBRARY} ${AAUDIO_LIBRARY})

    # The OpenXR loader must be dynamically loaded or the Oculus store will refuse it with "Oculus SDK not found or older than 1.0"
    set(DYNAMIC_LOADER ON)
    FetchContent_MakeAvailable(openxr_loader)
    target_link_libraries(wivrn openxr_loader)

    # libcurl
    set(BUILD_CURL_EXE OFF)
    set(BUILD_TESTING OFF)
    set(CURL_DISABLE_INSTALL ON)
    set(CURL_USE_LIBPSL OFF)
    set(CURL_USE_LIBSSH2 OFF)
    set(BUILD_EXAMPLES OFF)

    FetchContent_MakeAvailable(curl)
    target_link_libraries(wivrn libcurl_static)

    target_link_libraries(wivrn native_app_glue)
    target_compile_definitions(wivrn PRIVATE -DXR_USE_PLATFORM_ANDROID -DVK_USE_PLATFORM_ANDROID_KHR)
    target_link_options(wivrn PRIVATE -Wl,--undefined=ANativeActivity_onCreate)

    target_link_options(wivrn
        PRIVATE
        -Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/android/exported_symbols.txt
        # This causes the linker to emit an error when a version script names a
        # symbol that is not found, rather than silently ignoring that line.
        -Wl,--no-undefined-version
    )
    set_target_properties(wivrn PROPERTIES LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/android/exported_symbols.txt)

    file(GLOB PLATFORM_SOURCE CONFIGURE_DEPENDS
        "android/*.cpp"
        "audio/android/*.cpp"
        "decoder/android/*.cpp"
        )
    target_sources(wivrn PRIVATE ${PLATFORM_SOURCE})
else()
    add_executable(wivrn)

    if (WIVRN_USE_SYSTEM_OPENXR)
        target_link_libraries(wivrn OpenXR::openxr_loader OpenXR::headers)
    else()
        FetchContent_MakeAvailable(openxr_loader)
        target_link_libraries(wivrn openxr_loader)
    endif()

    target_link_libraries(wivrn PkgConfig::LIBAV_CLIENT Fontconfig::Fontconfig CURL::libcurl)

    file(GLOB PLATFORM_SOURCE CONFIGURE_DEPENDS
        "decoder/ffmpeg/*.cpp")
    target_sources(wivrn PRIVATE ${PLATFORM_SOURCE})

    file(CREATE_LINK ${CMAKE_CURRENT_BINARY_DIR}/share ${CMAKE_BINARY_DIR}/share SYMBOLIC)
endif()



##############################################################################
# Main sources
#
target_compile_definitions(wivrn PRIVATE "XR_USE_TIMESPEC=1")

set_target_properties(wivrn PROPERTIES CXX_VISIBILITY_PRESET hidden)

##############################################################################
# NX Warp (nxvc)
#
# The codec lives in its own repository next to this one. Only two of its
# components are needed on the client: the Vulkan compute decoder (which pulls
# vk/common for device adoption) and the transport library. Everything else --
# the encoder, the CPU reference, the rate controller, the tools and the tests --
# is switched off so a client build does not pay for them.
#
# ExternalProject rather than add_subdirectory, and not by preference: nx-warp's
# own CMake reads CMAKE_SOURCE_DIR in several places (its include root, its
# install rules, its CPack resources), so as a subdirectory of WiVRn it would
# look for its headers and its licence inside this tree. Building it out of
# process also keeps its cache variables, its -Wconversion and its CPack
# configuration out of ours, which is worth having on its own.
#
# nxvc_vk_decoder adopts the client's own VkDevice, so nothing here creates a
# second Vulkan device; see nxvc_vk.h and client/decoder/nxwarp/.
set(WIVRN_NXWARP_DIR "${CMAKE_SOURCE_DIR}/../nx-warp" CACHE PATH "Path to the nx-warp source tree")
set(WIVRN_HAVE_NXWARP OFF)
if(EXISTS "${WIVRN_NXWARP_DIR}/CMakeLists.txt")
    set(WIVRN_HAVE_NXWARP ON)
    set(NXWARP_PREFIX ${CMAKE_BINARY_DIR}/nx-warp)
    set(NXWARP_INSTALL ${NXWARP_PREFIX}/install)

    set(NXWARP_CMAKE_ARGS
        -DCMAKE_BUILD_TYPE=$<IF:$<CONFIG:Debug>,Debug,Release>
        -DCMAKE_INSTALL_PREFIX=${NXWARP_INSTALL}
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        -DNXWARP_BUILD_VK=ON
        -DNXWARP_VK_SUBDIRS=common$<SEMICOLON>decoder
        -DNXWARP_BUILD_TRANSPORT=ON
        -DNXWARP_BUILD_REF=OFF
        -DNXWARP_BUILD_ENC=OFF
        -DNXWARP_BUILD_RC=OFF
        -DNXWARP_BUILD_WARP=OFF
        -DNXWARP_BUILD_STEREO=OFF
        -DNXWARP_BUILD_HYBRID=OFF
        -DNXWARP_BUILD_FOV=OFF
        -DNXWARP_BUILD_PLATFORM=OFF
        -DNXWARP_BUILD_TOOLS=OFF
        -DNXWARP_BUILD_FUZZ=OFF
        -DNXWARP_BUILD_TESTS=OFF
        -DNXWARP_BUILD_EXAMPLES=OFF
        -DVulkan_INCLUDE_DIR=${Vulkan_INCLUDE_DIR}
    )
    if(ANDROID)
        # The decoder is compiled, not linked, for the headset build: the Vulkan
        # loader comes from the platform, and the NDK's libvulkan.so is only
        # linked into libwivrn.so.
        list(APPEND NXWARP_CMAKE_ARGS
            -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
            -DANDROID_ABI=${ANDROID_ABI}
            -DANDROID_PLATFORM=${ANDROID_PLATFORM}
            -DNXVC_VK_SPIRV_TARGET_ENV=vulkan1.1
        )
    endif()

    ExternalProject_Add(nxwarp_ep
        SOURCE_DIR ${WIVRN_NXWARP_DIR}
        PREFIX ${NXWARP_PREFIX}
        BINARY_DIR ${NXWARP_PREFIX}/build
        INSTALL_DIR ${NXWARP_INSTALL}
        CMAKE_ARGS ${NXWARP_CMAKE_ARGS}
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target nxvc_vk_decoder nxvc_vk_common nxvc_transport
        # `cmake --install` rather than the install target, which would first *build*
        # everything -- including nx-warp's nxvc-vkdec CLI, which links vkWaitSemaphores
        # and so cannot link against the NDK's android-29 libvulkan.so. The libraries are
        # the only thing wanted here and BUILD_COMMAND has already made them.
        INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR>
        BUILD_ALWAYS ON
        BUILD_BYPRODUCTS
            ${NXWARP_INSTALL}/lib/libnxvc_vk_decoder.a
            ${NXWARP_INSTALL}/lib/libnxvc_vk_common.a
            ${NXWARP_INSTALL}/lib/libnxvc_transport.a
    )
    file(MAKE_DIRECTORY ${NXWARP_INSTALL}/include)

    # What the atlas ABI in THIS nx-warp checkout offers.
    #
    # The server probes its nxvc by compiling against it (cmake/NxvcAtlasFeatures.cmake),
    # which cannot work here: the client BUILDS nxvc, so at configure time
    # ${NXWARP_INSTALL}/include is an empty directory that ExternalProject has not filled
    # yet. What does exist at configure time is the source header that will be installed,
    # so that is what is read. It is the same file, one copy earlier.
    #
    # Text matching rather than a compile: a struct field and a function declaration are
    # neither of them things the preprocessor can see, and the alternative -- deferring the
    # answer to build time through a generated header -- would make every guarded site
    # depend on a generation step for a question whose answer is a grep.
    set(_nxvc_hdr ${WIVRN_NXWARP_DIR}/include/nxvc/nxvc_vk.h)
    set(WIVRN_NXVC_ATLAS_DECODE OFF)
    set(WIVRN_NXVC_ATLAS_STATS OFF)
    set(WIVRN_NXVC_TOOLS_FOR OFF)
    if(EXISTS ${_nxvc_hdr})
        file(READ ${_nxvc_hdr} _nxvc_hdr_text)
        if(_nxvc_hdr_text MATCHES "nxvc_vk_decoder_set_atlas_view")
            set(WIVRN_NXVC_ATLAS_DECODE ON)
        endif()
        if(_nxvc_hdr_text MATCHES "tiles_superseded")
            set(WIVRN_NXVC_ATLAS_STATS ON)
        endif()
        if(_nxvc_hdr_text MATCHES "nxvc_vk_decoder_tools_for")
            set(WIVRN_NXVC_TOOLS_FOR ON)
        endif()
        unset(_nxvc_hdr_text)
    endif()
    message(STATUS "NX Warp atlas (client): decoder ${WIVRN_NXVC_ATLAS_DECODE}, stats ${WIVRN_NXVC_ATLAS_STATS}, tools_for ${WIVRN_NXVC_TOOLS_FOR}")

    foreach(_lib nxvc_vk_decoder nxvc_vk_common nxvc_transport)
        add_library(${_lib} STATIC IMPORTED GLOBAL)
        set_target_properties(${_lib} PROPERTIES
            IMPORTED_LOCATION ${NXWARP_INSTALL}/lib/lib${_lib}.a
            INTERFACE_INCLUDE_DIRECTORIES ${NXWARP_INSTALL}/include)
        add_dependencies(${_lib} nxwarp_ep)
    endforeach()

    # nxvc_transport's real AEAD backends come from OpenSSL when it is there; the client
    # itself already links it, but the loopback tool below has to say so.
    find_package(OpenSSL QUIET COMPONENTS Crypto)
    if(OpenSSL_FOUND)
        set_property(TARGET nxvc_transport APPEND PROPERTY
                     INTERFACE_LINK_LIBRARIES OpenSSL::Crypto)
    endif()
endif()

file(GLOB LOCAL_SOURCE CONFIGURE_DEPENDS
    "*.cpp"
    "decoder/*.cpp"
    "decoder/nxwarp/*.cpp"
    "scenes/*.cpp"
    "utils/*.cpp"
    "vk/*.cpp"
    "xr/*.cpp"
    "render/*.cpp"
)

file(GLOB_RECURSE VULKAN_SHADERS CONFIGURE_DEPENDS "*.glsl")
target_sources(wivrn PRIVATE ${LOCAL_SOURCE} ${VULKAN_SHADERS})
wivrn_compile_glsl(wivrn SHADERS ${VULKAN_SHADERS})

target_link_libraries(wivrn
    Boost::locale
    Boost::url
    EnTT::EnTT
    fastgltf
    FreetypeHarfbuzz
    glm::glm
    imspinner
    ktx
    simdjson
    spdlog::spdlog
    spirv-reflect-static
    stb
    uni-algo
    Vulkan::Vulkan
    wivrn-common
    wivrn-external
)

if(WIVRN_HAVE_NXWARP)
    target_link_libraries(wivrn nxvc_vk_decoder nxvc_vk_common nxvc_transport)

    # The atlas guards, from the header scan above. Absent, every atlas site compiles out
    # and the client behaves exactly as it did before the mode existed -- which is the
    # state a build against a pre-atlas nx-warp must still reach.
    if(WIVRN_NXVC_ATLAS_DECODE)
        target_compile_definitions(wivrn PRIVATE WIVRN_NXVC_ATLAS_DECODE)
    endif()
    if(WIVRN_NXVC_ATLAS_STATS)
        target_compile_definitions(wivrn PRIVATE WIVRN_NXVC_ATLAS_STATS)
    endif()
    if(WIVRN_NXVC_TOOLS_FOR)
        target_compile_definitions(wivrn PRIVATE WIVRN_NXVC_TOOLS_FOR)
    endif()

    # The depacketize-and-reassemble path, exercisable without a headset. See the file
    # header for what it proves.
    if(NOT ANDROID)
        add_executable(nxwarp-loopback
            decoder/nxwarp/tools/nxwarp_loopback.cpp
            decoder/nxwarp/nxwarp_reassemble.cpp)
        target_include_directories(nxwarp-loopback PRIVATE .)
        target_link_libraries(nxwarp-loopback nxvc_transport)
        target_compile_features(nxwarp-loopback PRIVATE cxx_std_23)
    endif()
endif()

target_compile_definitions(wivrn PRIVATE -DXR_USE_GRAPHICS_API_VULKAN)
target_compile_definitions(wivrn PRIVATE -DVMA_STATS_STRING_ENABLED=0)
target_compile_definitions(wivrn PRIVATE -DGLM_ENABLE_EXPERIMENTAL)

target_include_directories(wivrn PRIVATE .)

if(NOT ANDROID)
    install(TARGETS wivrn)
endif()

##############################################################################
# Dear ImGui
#
add_library(wivrn-imgui STATIC)
target_sources(wivrn-imgui PRIVATE
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/misc/freetype/imgui_freetype.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
    ${implot_SOURCE_DIR}/implot.cpp
    ${implot_SOURCE_DIR}/implot_items.cpp
)

target_include_directories(wivrn-imgui PUBLIC ${imgui_SOURCE_DIR})
target_include_directories(wivrn-imgui PUBLIC ${implot_SOURCE_DIR})
# target_compile_definitions(wivrn-imgui PUBLIC "IMGUI_USER_CONFIG=\"render/imgui_config.h\"")
target_compile_definitions(wivrn-imgui PUBLIC IMGUI_USE_WCHAR32)
target_compile_definitions(wivrn-imgui PUBLIC IMGUI_ENABLE_FREETYPE)
# target_compile_definitions(wivrn-imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS) # incompatible with implot
# target_compile_definitions(wivrn-imgui PUBLIC IMGUI_ENABLE_FREETYPE_LUNASVG)
target_compile_features(wivrn-imgui PRIVATE cxx_std_23)
target_link_libraries(wivrn-imgui FreetypeHarfbuzz Vulkan::Vulkan)

target_link_libraries(wivrn wivrn-imgui)



##############################################################################
# Vulkan Validation Layer
#
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/vk_layer_settings.txt
"khronos_validation.debug_action = VK_DBG_LAYER_ACTION_LOG_MSG,VK_DBG_LAYER_ACTION_BREAK
khronos_validation.report_flags = error,warn
khronos_validation.log_filename = stdout
khronos_validation.disables =
khronos_validation.enables = VK_VALIDATION_FEATURE_ENABLE_THREAD_SAFETY_EXT
")
endif()

if(WIVRN_USE_ANDROID_VALIDATION_LAYER)
    set(VVL_VERSION "1.4.321.0")

    if(NOT EXISTS ${FETCHCONTENT_BASE_DIR}/Vulkan-ValidationLayers-${VVL_VERSION}.zip)
        if (EXISTS ${CMAKE_SOURCE_DIR}/Vulkan-ValidationLayers-${VVL_VERSION}.zip)
            file(CREATE_LINK ${CMAKE_SOURCE_DIR}/Vulkan-ValidationLayers-${VVL_VERSION}.zip ${FETCHCONTENT_BASE_DIR}/Vulkan-ValidationLayers-${VVL_VERSION}.zip SYMBOLIC)
        else()
            file(DOWNLOAD https://github.com/KhronosGroup/Vulkan-ValidationLayers/releases/download/vulkan-sdk-${VVL_VERSION}/android-binaries-${VVL_VERSION}.zip
                 ${FETCHCONTENT_BASE_DIR}/Vulkan-ValidationLayers-${VVL_VERSION}.zip
                 EXPECTED_HASH SHA256=75f2881ba7af0507c8cec13b4bf6c95556c6333485cdb8be41f608fccf762128)
        endif()
    endif()

    set(VVL_ZIPFILE ${CMAKE_CURRENT_SOURCE_DIR}/Vulkan-ValidationLayers-${VVL_VERSION}.zip)

    file(ARCHIVE_EXTRACT INPUT ${FETCHCONTENT_BASE_DIR}/Vulkan-ValidationLayers-${VVL_VERSION}.zip DESTINATION ${FETCHCONTENT_BASE_DIR}/Vulkan-ValidationLayer)

    file(COPY
        ${FETCHCONTENT_BASE_DIR}/Vulkan-ValidationLayer/android-binaries-${VVL_VERSION}/${CMAKE_ANDROID_ARCH_ABI}
        DESTINATION ${ANDROID_LIB_DIRECTORIES})
else()
    # silence "Manually-specified variables were not used by the project" warning
    set(_unused ${ANDROID_LIB_DIRECTORIES})
endif()


##############################################################################
# Assets
#
if(ANDROID)
    set(ASSETS_DIR ${CMAKE_ANDROID_ASSETS_DIRECTORIES})
else()
    set(ASSETS_DIR ${CMAKE_CURRENT_BINARY_DIR}/share/wivrn/assets)
    file(MAKE_DIRECTORY ${ASSETS_DIR})
    install(DIRECTORY "${ASSETS_DIR}" DESTINATION "${CMAKE_INSTALL_DATADIR}/wivrn")
endif()


##############################################################################
# Internationalization
#
create_mo_files(wivrn wivrn "${CMAKE_CURRENT_SOURCE_DIR}/../locale")
create_glyphset(wivrn wivrn "${CMAKE_CURRENT_SOURCE_DIR}/../locale")


##############################################################################
# License files
#
set(LICENSES_DIR "${ASSETS_DIR}/licenses")

file(MAKE_DIRECTORY "${LICENSES_DIR}")
file(COPY_FILE "${CMAKE_SOURCE_DIR}/COPYING" "${LICENSES_DIR}/WiVRn")
file(COPY_FILE "${simdjson_SOURCE_DIR}/LICENSE" "${LICENSES_DIR}/simdjson")
file(COPY_FILE "${CMAKE_SOURCE_DIR}/LICENSE-OFL-1.1" "${LICENSES_DIR}/FontAwesome")
if (OPENXR_SOURCE_DIR)
    file(COPY_FILE "${OPENXR_SOURCE_DIR}/LICENSE" "${LICENSES_DIR}/openxr-loader")
endif()


##############################################################################
# Generate assets
#
file(REAL_PATH ${WIVRN_CSS} WIVRN_CSS_ABS BASE_DIRECTORY "${CMAKE_SOURCE_DIR}")

set(IMAGES_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../images)
set(ASSETS_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../assets-source)

wivrn_rasterize_svg(SOURCE ${IMAGES_SRC_DIR}/wivrn.svg               DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/wivrn.png               WIDTH 540 HEIGHT 540 CSS ${WIVRN_CSS_ABS})
wivrn_rasterize_svg(SOURCE ${IMAGES_SRC_DIR}/wivrn-pride.svg         DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/wivrn-pride.png         WIDTH 540 HEIGHT 540 CSS ${WIVRN_CSS_ABS})
wivrn_rasterize_svg(SOURCE ${IMAGES_SRC_DIR}/wivrn-christmas.svg     DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/wivrn-christmas.png     WIDTH 540 HEIGHT 540 CSS ${WIVRN_CSS_ABS})
wivrn_rasterize_svg(SOURCE ${ASSETS_SRC_DIR}/passthrough.svg         DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/passthrough.png         WIDTH 256 HEIGHT 256)
wivrn_rasterize_svg(SOURCE ${ASSETS_SRC_DIR}/default-environment.svg DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/default-environment.png WIDTH 256 HEIGHT 256)

wivrn_generate_ktx(        SOURCE ${CMAKE_CURRENT_BINARY_DIR}/wivrn.png                 DESTINATION ${ASSETS_DIR}/wivrn.ktx2                          TARGET wivrn)
wivrn_generate_ktx(        SOURCE ${CMAKE_CURRENT_BINARY_DIR}/wivrn-pride.png           DESTINATION ${ASSETS_DIR}/wivrn-pride.ktx2                    TARGET wivrn)
wivrn_generate_ktx(        SOURCE ${CMAKE_CURRENT_BINARY_DIR}/wivrn-christmas.png       DESTINATION ${ASSETS_DIR}/wivrn-christmas.ktx2                TARGET wivrn)
wivrn_generate_ktx(        SOURCE ${ASSETS_SRC_DIR}/default_icon.png                    DESTINATION ${ASSETS_DIR}/default_icon.ktx2                   TARGET wivrn)
wivrn_generate_ktx(        SOURCE ${CMAKE_CURRENT_BINARY_DIR}/passthrough.png           DESTINATION ${ASSETS_DIR}/passthrough.ktx2                    TARGET wivrn)
wivrn_generate_ktx(        SOURCE ${CMAKE_CURRENT_BINARY_DIR}/default-environment.png   DESTINATION ${ASSETS_DIR}/default-environment.ktx2            TARGET wivrn)
if(WIVRN_COMPRESS_GLB)
    wivrn_gltf_transform_uastc(SOURCE ${ASSETS_SRC_DIR}/ground.glb                      DESTINATION ${ASSETS_DIR}/ground.glb                          TARGET wivrn ZSTD 20)
else()
    wivrn_copy_file(       SOURCE ${ASSETS_SRC_DIR}/ground.glb                          DESTINATION ${ASSETS_DIR}/ground.glb                          TARGET wivrn)
endif()

wivrn_webxr_download()
wivrn_webxr_controller(PROFILE generic-trigger-squeeze DESTINATION ${ASSETS_DIR}/controllers TARGET wivrn)
wivrn_webxr_controller(PROFILE htc-vive-focus-3        DESTINATION ${ASSETS_DIR}/controllers TARGET wivrn)
wivrn_webxr_controller(PROFILE meta-quest-touch-plus   DESTINATION ${ASSETS_DIR}/controllers TARGET wivrn)
wivrn_webxr_controller(PROFILE meta-quest-touch-pro    DESTINATION ${ASSETS_DIR}/controllers TARGET wivrn)
wivrn_webxr_controller(PROFILE oculus-touch-v2         DESTINATION ${ASSETS_DIR}/controllers TARGET wivrn)
wivrn_webxr_controller(PROFILE oculus-touch-v3         DESTINATION ${ASSETS_DIR}/controllers TARGET wivrn)
wivrn_webxr_controller(PROFILE pico-4                  DESTINATION ${ASSETS_DIR}/controllers TARGET wivrn)
wivrn_webxr_controller(PROFILE pico-4u                 DESTINATION ${ASSETS_DIR}/controllers TARGET wivrn)
wivrn_webxr_controller(PROFILE pico-neo3               DESTINATION ${ASSETS_DIR}/controllers TARGET wivrn)
wivrn_webxr_controller(PROFILE samsung-galaxyxr        DESTINATION ${ASSETS_DIR}/controllers TARGET wivrn)
wivrn_webxr_controller(PROFILE yvr-touch-v2            DESTINATION ${ASSETS_DIR}/controllers TARGET wivrn)

# Add the VR splash screen (only used on Quest)
if(ANDROID)
    wivrn_copy_file(SOURCE ${CMAKE_CURRENT_BINARY_DIR}/wivrn.png DESTINATION ${ASSETS_DIR}/vr_splash.png TARGET wivrn)
endif()

##############################################################################
# Copy the assets to be able to find them when started from the build
# directory
if(NOT ANDROID)
    file(GLOB_RECURSE ASSETS CONFIGURE_DEPENDS LIST_DIRECTORIES FALSE RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/../assets" "${CMAKE_CURRENT_SOURCE_DIR}/../assets/*" )

    set(COPIED_ASSETS)

    foreach(ASSET IN LISTS ASSETS)
        add_custom_command(OUTPUT ${ASSETS_DIR}/${ASSET}
            COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/../assets/${ASSET} ${ASSETS_DIR}/${ASSET}
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../assets/${ASSET})

        set(COPIED_ASSETS ${COPIED_ASSETS} ${ASSETS_DIR}/${ASSET})
    endforeach()
    add_custom_target(assets-builddir ALL DEPENDS ${COPIED_ASSETS})
    add_dependencies(wivrn assets-builddir)

    set_target_properties(wivrn PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

    install(DIRECTORY "${ASSETS_DIR}" DESTINATION ${CMAKE_INSTALL_DATADIR}/wivrn)
endif()
