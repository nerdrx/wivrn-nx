# What the nxvc this build found actually offers, for the atlas coding mode.
#
# The atlas arrived in two halves on two branches -- the GPU decoder (tool bits 31 and 34,
# the R8 display view) and the encoder that emits it (the per-frame ATLAS/PICTURE mode, the
# `D` trigger) -- and they land upstream at different times. WiVRn is built against both
# during that rollout, so every use of the atlas ABI here is guarded, and the guards are
# decided ONCE, here, rather than rediscovered at each site.
#
# Neither half ships a feature macro of its own the way NXVC_VK_DECODER_PASSB_SEGMENTS
# does, and neither a struct field nor a function declaration is something the preprocessor
# can see, so these are compile probes rather than #ifdefs. Each one compiles the smallest
# program that uses exactly the thing being asked about.
#
# Defines, on the caller's scope:
#
#   WIVRN_NXVC_ATLAS_DECODE   the decoder can produce and expose a sampled atlas view
#   WIVRN_NXVC_ATLAS_ENCODE   the encoder create info carries the atlas mode fields
#   WIVRN_NXVC_ATLAS_STATS    nxvc_vkd_stats carries tiles_superseded
#   WIVRN_NXVC_TOOLS_FOR      nxvc_vk_decoder_tools_for() exists
#
# The last one has an upstream macro (NXVC_VK_DECODER_TOOLS_FOR) and is probed anyway, so
# that a prefix which grew the function without the macro is still detected, and so that
# every atlas guard in this tree is spelled the same way.

include(CheckCXXSourceCompiles)
include(CMakePushCheckState)

function(wivrn_probe_nxvc_atlas_features)
    if (NOT WIVRN_USE_NXWARP)
        return()
    endif()

    cmake_push_check_state(RESET)

    # The probes need nxvc's headers and Vulkan's. Link is not attempted: a declaration is
    # all that is being asked about, and requiring a successful link here would make the
    # answer depend on static library ordering that the real targets already solve.
    set(CMAKE_REQUIRED_QUIET ON)
    set(CMAKE_REQUIRED_LINK_OPTIONS -fsyntax-only)

    foreach(_t nxvc::vk_decoder nxvc::vk_encoder nxvc::vk_common)
        if (TARGET ${_t})
            get_target_property(_inc ${_t} INTERFACE_INCLUDE_DIRECTORIES)
            if (_inc)
                list(APPEND CMAKE_REQUIRED_INCLUDES ${_inc})
            endif()
        endif()
    endforeach()
    if (Vulkan_INCLUDE_DIRS)
        list(APPEND CMAKE_REQUIRED_INCLUDES ${Vulkan_INCLUDE_DIRS})
    endif()
    if (NXVC_VK_HEADERS_DIR)
        list(APPEND CMAKE_REQUIRED_INCLUDES ${NXVC_VK_HEADERS_DIR})
    endif()

    # -fsyntax-only means the "executable" is never produced, which check_cxx_source_compiles
    # would then call a failure. Compile to an object instead.
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    unset(CMAKE_REQUIRED_LINK_OPTIONS)

    check_cxx_source_compiles("
        #include <nxvc/nxvc_vk.h>
        int probe(nxvc_vk_decoder * d)
        {
            nxvc_vk_decoder_set_atlas_view(d, NXVC_VKD_ATLAS_VIEW_R8);
            nxvc_vkd_atlas_images imgs{};
            nxvc_vk_decoder_atlas_images(d, &imgs);
            return int(imgs.width[0]);
        }
        " WIVRN_NXVC_ATLAS_DECODE)

    check_cxx_source_compiles("
        #include <nxvc/nxvc_vk_enc.h>
        int probe(nxvc_vke_create_info * ci)
        {
            ci->atlas = 1;
            ci->atlas_mode = 1;
            ci->atlas_picture_d = 8;
            return 0;
        }
        " WIVRN_NXVC_ATLAS_ENCODE)

    check_cxx_source_compiles("
        #include <nxvc/nxvc_vk.h>
        unsigned probe(const nxvc_vkd_stats * s) { return s->tiles_superseded; }
        " WIVRN_NXVC_ATLAS_STATS)

    check_cxx_source_compiles("
        #include <nxvc/nxvc_vk.h>
        unsigned long long probe() { return nxvc_vk_decoder_tools_for(0x1002u, \"probe\"); }
        " WIVRN_NXVC_TOOLS_FOR)

    cmake_pop_check_state()

    foreach(_v WIVRN_NXVC_ATLAS_DECODE WIVRN_NXVC_ATLAS_ENCODE WIVRN_NXVC_ATLAS_STATS WIVRN_NXVC_TOOLS_FOR)
        if (${_v})
            set(${_v} ON PARENT_SCOPE)
        else()
            set(${_v} OFF PARENT_SCOPE)
        endif()
    endforeach()
endfunction()
