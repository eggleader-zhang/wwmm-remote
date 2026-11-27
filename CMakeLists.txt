cmake_minimum_required(VERSION 3.24)
if(CMAKE_HOST_APPLE AND NOT CMAKE_TOOLCHAIN_FILE AND "$ENV{MACOSX_DEPLOYMENT_TARGET}" STREQUAL "")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "14.4" CACHE STRING "Minimum macOS deployment target")
endif()
project(WWMMRemote VERSION 1.5.4 LANGUAGES C CXX)
set(WWMM_BUILD_NUMBER 0 CACHE STRING "Native package build number")
configure_file(src/release_version.h.in release_version.h @ONLY)
set(wwmm_bundle_build "${PROJECT_VERSION}")
if(WWMM_BUILD_NUMBER GREATER 0)
    set(wwmm_bundle_build "${WWMM_BUILD_NUMBER}")
endif()
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets Network Concurrent)
qt_standard_project_setup()
if(MSVC)
    add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/utf-8>")
endif()
include(CTest)
include(cmake/PairCrypto.cmake)
wwmm_add_pair_crypto()
include(cmake/DesktopMedia.cmake)
wwmm_add_desktop_media()

add_library(wwmm_lan STATIC
    src/lan_address.cpp src/lan_address.h
    src/lan_probe.cpp src/lan_probe.h
    src/device_store.cpp src/device_store.h
    src/discovery.cpp src/discovery.h
    src/pair_session.cpp src/pair_session.h
    src/pairing.cpp src/pairing.h
    src/trusted_channel.cpp src/trusted_channel.h
    src/desktop_session.cpp src/desktop_session.h src/desktop_display.h src/video_viewport.h
    src/session_data.cpp src/session_data.h src/session_displays.cpp
    src/desktop_input.cpp src/desktop_input.h src/input_keys.cpp
    src/video_packets.cpp src/video_packets.h
    src/video_link.cpp src/video_link.h src/session_media.cpp src/audio_stream.cpp src/audio_stream.h
    src/probe_message.cpp src/probe_message.h)
target_include_directories(wwmm_lan PUBLIC src)
target_link_libraries(wwmm_lan PUBLIC Qt6::Network ssl PRIVATE Qt6::Concurrent wwmm_webrtc srtp2 opus)
if(WIN32)
    target_sources(wwmm_lan PRIVATE platform/windows/vault.cpp platform/windows/discovery.cpp)
    target_link_libraries(wwmm_lan PRIVATE ws2_32 crypt32 dnsapi bcrypt)
elseif(APPLE)
    target_sources(wwmm_lan PRIVATE platform/macos/vault.cpp platform/macos/discovery.cpp)
    target_link_libraries(wwmm_lan PRIVATE "-framework Security" "-framework CoreFoundation")
endif()

add_library(wwmm_widgets STATIC src/main_window.cpp src/main_window.h src/devices_panel.cpp src/devices_panel.h
    src/desktop_tray.cpp src/desktop_tray.h
    src/desktop_update.cpp src/desktop_update.h
    src/desktop_controls.cpp src/desktop_controls.h src/desktop_viewer.cpp src/desktop_viewer.h
    src/input_window.cpp src/input_window.h src/transfer_bar.cpp src/transfer_bar.h src/clipboard_images.cpp src/clipboard_images.h)
target_link_libraries(wwmm_widgets PUBLIC wwmm_lan Qt6::Widgets)
target_include_directories(wwmm_widgets PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")
if(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 20)
    add_library(wwmm_native STATIC platform/macos/video_encoder.mm platform/macos/video_decoder.mm
        platform/macos/capture.mm platform/macos/player.mm platform/macos/input.mm platform/macos/audio.mm)
    set_target_properties(wwmm_native PROPERTIES OBJCXX_STANDARD 20)
    target_compile_options(wwmm_native PRIVATE -fobjc-arc)
    target_link_libraries(wwmm_native PUBLIC wwmm_lan PRIVATE "-framework ScreenCaptureKit" "-framework VideoToolbox"
        "-framework CoreMedia" "-framework CoreVideo" "-framework CoreGraphics" "-framework Metal" "-framework QuartzCore" "-framework AppKit"
        "-framework ApplicationServices" "-framework Carbon" "-framework AudioToolbox" "-framework ColorSync")
elseif(WIN32)
    target_sources(wwmm_widgets PRIVATE platform/windows/keyboard_grab.cpp)
    target_link_libraries(wwmm_widgets PRIVATE user32)
    add_library(wwmm_native STATIC platform/windows/desktop.cpp platform/windows/pointer.cpp platform/windows/input.cpp platform/windows/audio.cpp)
    target_compile_definitions(wwmm_native PRIVATE NOMINMAX)
    target_link_libraries(wwmm_native PUBLIC wwmm_lan PRIVATE d3d11 d3dcompiler dxgi dxguid mfplat mf mfuuid ole32 oleaut32 strmiids wmcodecdspuuid)
endif()
target_link_libraries(wwmm_widgets PRIVATE wwmm_native Qt6::Concurrent)

qt_add_executable(wwmm_remote src/main.cpp)
qt_add_resources(wwmm_widgets "app_icons" PREFIX "/icons" BASE platform/icons
    FILES platform/icons/wwmm.png platform/icons/menubar.png platform/icons/menubar@2x.png)
if(WIN32)
    enable_language(RC)
    configure_file(platform/windows/version.rc.in "${CMAKE_CURRENT_BINARY_DIR}/wwmm-version.rc" @ONLY)
    target_sources(wwmm_remote PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/wwmm-version.rc")
    set_property(SOURCE "${CMAKE_CURRENT_BINARY_DIR}/wwmm-version.rc" APPEND PROPERTY
        OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/platform/icons/WWMM.ico")
elseif(APPLE)
    set_source_files_properties(platform/icons/WWMM.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_sources(wwmm_remote PRIVATE platform/icons/WWMM.icns)
endif()
set_target_properties(wwmm_remote PROPERTIES
    OUTPUT_NAME "WWMM Remote"
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/Info.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER "dev.wwmm.remote"
    MACOSX_BUNDLE_BUNDLE_VERSION "${wwmm_bundle_build}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
target_link_libraries(wwmm_remote PRIVATE wwmm_widgets)

if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    if(APPLE)
        qt_add_executable(wwmm_video_bench research/desktop-video-bench.mm)
        target_compile_options(wwmm_video_bench PRIVATE -fobjc-arc)
        target_link_libraries(wwmm_video_bench PRIVATE wwmm_native wwmm_widgets "-framework AppKit" "-framework CoreVideo")
    endif()
    qt_add_executable(wwmm_tests tests/lan_test.cpp)
    target_link_libraries(wwmm_tests PRIVATE wwmm_widgets Qt6::Test)
    add_test(NAME wwmm_lan_and_ui COMMAND wwmm_tests)
    qt_add_executable(wwmm_pair_tests tests/pairing_test.cpp)
    target_link_libraries(wwmm_pair_tests PRIVATE wwmm_widgets Qt6::Test)
    add_test(NAME wwmm_pairing_and_discovery COMMAND wwmm_pair_tests)
    qt_add_executable(wwmm_desktop_tests tests/desktop_test.cpp)
    target_link_libraries(wwmm_desktop_tests PRIVATE wwmm_widgets Qt6::Test)
    add_test(NAME wwmm_readonly_desktop COMMAND wwmm_desktop_tests)
    set_tests_properties(wwmm_readonly_desktop wwmm_lan_and_ui wwmm_pairing_and_discovery PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
endif()

install(TARGETS wwmm_remote BUNDLE DESTINATION . RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
if(APPLE)
    set(wwmm_notices "WWMM Remote.app/Contents/Resources/third-party")
else()
    set(wwmm_notices "${CMAKE_INSTALL_BINDIR}/third-party")
endif()
install(FILES "${WWMM_CRYPTO_LICENSE}" DESTINATION "${wwmm_notices}" RENAME BoringSSL-LICENSE)
install(FILES "${WWMM_WEBRTC_SOURCE}/LICENSE" DESTINATION "${wwmm_notices}" RENAME WebRTC-LICENSE)
install(FILES "${WWMM_WEBRTC_SOURCE}/PATENTS" DESTINATION "${wwmm_notices}" RENAME WebRTC-PATENTS)
install(FILES "${WWMM_ABSL_SOURCE}/LICENSE" DESTINATION "${wwmm_notices}" RENAME Abseil-LICENSE)
install(FILES "${WWMM_OPUS_SOURCE}/COPYING" DESTINATION "${wwmm_notices}" RENAME Opus-LICENSE)
install(FILES "${WWMM_SRTP_SOURCE}/LICENSE" DESTINATION "${wwmm_notices}" RENAME libSRTP-LICENSE)
get_filename_component(wwmm_qt_root "${Qt6_DIR}/../../.." ABSOLUTE)
install(FILES "${wwmm_qt_root}/sbom/qtbase-${Qt6_VERSION}.spdx" DESTINATION "${wwmm_notices}" OPTIONAL)
qt_generate_deploy_app_script(TARGET wwmm_remote OUTPUT_SCRIPT wwmm_deploy
    NO_UNSUPPORTED_PLATFORM_ERROR)
install(SCRIPT ${wwmm_deploy})
