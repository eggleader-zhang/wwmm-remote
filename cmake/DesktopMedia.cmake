include(FetchContent)

function(wwmm_add_desktop_media)
    FetchContent_Declare(webrtc_src
        URL https://codeload.github.com/webrtc-mirror/webrtc/tar.gz/3710345e71ad3a7c1f4ed4efef152fd4fc24869a
        URL_HASH SHA256=3361a8dc1b27481db00ad253d6587b15a5590b0ba94281f1213f7f666f680f17
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR wwmm-populate-only)
    FetchContent_Declare(media_absl
        URL https://codeload.github.com/abseil/abseil-cpp/tar.gz/refs/tags/20260817.0
        URL_HASH SHA256=f7e05179df39c45434cad433f5783840bb3788ef322976f9138bc6b72b3a107d
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR wwmm-populate-only)
    FetchContent_Declare(media_srtp
        URL https://codeload.github.com/cisco/libsrtp/tar.gz/refs/tags/v2.8.1
        URL_HASH SHA256=ef5569220749529d778013aae1178391d972570a2b4f7288dda22effa875b07c
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR wwmm-populate-only)
    FetchContent_MakeAvailable(webrtc_src media_absl media_srtp)
    set(LIBSRTP_TEST_APPS OFF)
    set(ENABLE_WARNINGS_AS_ERRORS OFF)
    set(BUILD_SHARED_LIBS OFF)
    set(CMAKE_AUTOMOC OFF)
    set(CMAKE_AUTOUIC OFF)
    add_subdirectory("${media_srtp_SOURCE_DIR}" "${media_srtp_BINARY_DIR}" EXCLUDE_FROM_ALL)
    set(ABSL_ENABLE_INSTALL OFF)
    add_subdirectory("${media_absl_SOURCE_DIR}" "${media_absl_BINARY_DIR}" EXCLUDE_FROM_ALL)

    set(rtc_sources
        common_video/h264/h264_common.cc common_video/h264/sps_parser.cc
        modules/pacing/interval_budget.cc modules/remote_bitrate_estimator/aimd_rate_control.cc
        modules/remote_bitrate_estimator/bwe_defines.cc modules/congestion_controller/goog_cc/link_capacity_estimator.cc
        rtc_base/experiments/field_trial_parser.cc
        modules/rtp_rtcp/source/rtp_format.cc modules/rtp_rtcp/source/rtp_format_h264.cc
        modules/rtp_rtcp/source/rtp_format_video_generic.cc modules/rtp_rtcp/source/rtp_format_vp8.cc
        modules/rtp_rtcp/source/rtp_format_vp9.cc modules/rtp_rtcp/source/rtp_packetizer_av1.cc
        modules/rtp_rtcp/source/leb128.cc rtc_base/bit_buffer.cc rtc_base/byte_buffer.cc
        modules/rtp_rtcp/source/rtp_header_extension_map.cc
        modules/rtp_rtcp/source/rtp_packet.cc modules/rtp_rtcp/source/rtp_packet_to_send.cc
        rtc_base/bitstream_reader.cc rtc_base/checks.cc rtc_base/copy_on_write_buffer.cc)
    list(TRANSFORM rtc_sources PREPEND "${webrtc_src_SOURCE_DIR}/")
    add_library(wwmm_webrtc STATIC ${rtc_sources})
    target_link_libraries(wwmm_webrtc PRIVATE absl::strings)
    target_include_directories(wwmm_webrtc SYSTEM PUBLIC "${webrtc_src_SOURCE_DIR}" "${media_absl_SOURCE_DIR}")
    target_compile_definitions(wwmm_webrtc PUBLIC RTC_DISABLE_LOGGING)
    if(WIN32)
        target_compile_definitions(wwmm_webrtc PUBLIC WEBRTC_WIN NOMINMAX)
    else()
        target_compile_definitions(wwmm_webrtc PUBLIC WEBRTC_POSIX WEBRTC_MAC)
    endif()
    if(MSVC)
        target_compile_options(wwmm_webrtc PRIVATE /Gy /Gw)
        target_link_options(wwmm_webrtc INTERFACE /OPT:REF)
    else()
        target_compile_options(wwmm_webrtc PRIVATE -ffunction-sections -fdata-sections)
        if(APPLE)
            target_link_options(wwmm_webrtc INTERFACE -Wl,-dead_strip)
        else()
            target_link_options(wwmm_webrtc INTERFACE -Wl,--gc-sections)
        endif()
    endif()
    set(WWMM_WEBRTC_SOURCE "${webrtc_src_SOURCE_DIR}" PARENT_SCOPE)
    set(WWMM_SRTP_SOURCE "${media_srtp_SOURCE_DIR}" PARENT_SCOPE)
    set(WWMM_ABSL_SOURCE "${media_absl_SOURCE_DIR}" PARENT_SCOPE)
endfunction()
