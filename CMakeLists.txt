cmake_minimum_required(VERSION 3.12)
project(AndroidCompatibilityLayer LANGUAGES C CXX)

option(MEGLER_EXPERIMENTAL_NATIVE_EGL "Enable experimental native EGL/GLES rendering" OFF)
option(MEGLER_EXPERIMENTAL_GPU_COMPOSITION "Enable experimental native-window GPU presentation" OFF)
option(MEGLER_BUILD_TEST_TARGETS "Generate optional test targets" ON)
if(MEGLER_EXPERIMENTAL_GPU_COMPOSITION AND NOT MEGLER_EXPERIMENTAL_NATIVE_EGL)
    message(FATAL_ERROR "GPU composition requires MEGLER_EXPERIMENTAL_NATIVE_EGL")
endif()

# Keep the installed identity until an upgrade-safe data migration is available.
set(APP_ID ru.megler.MeglerProbe)

find_package(Qt5 COMPONENTS Core Gui Qml Quick Multimedia Sensors Network DBus Positioning LinguistTools REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(AURORA REQUIRED auroraapp)
pkg_check_modules(RUNTIME_MANAGER REQUIRED runtime-manager-qt5)

set(CMAKE_AUTOMOC ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
qt5_add_translation(MEGLER_TRANSLATIONS translations/megler_ru.ts)
add_custom_target(megler_translations ALL DEPENDS ${MEGLER_TRANSLATIONS})

file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/core/src/*.c")
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/core/tests/*.c")
list(FILTER TEST_SOURCES EXCLUDE REGEX "/mg_tests_main\\.c$")
file(GLOB ZLIB_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/core/third_party/zlib/*.c")
list(FILTER ZLIB_SOURCES EXCLUDE REGEX "/gz[^/]*\\.c$")

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(core/src/sqlite/mg_sqlite3.c PROPERTIES
        COMPILE_OPTIONS "$<$<CONFIG:Debug>:-g1>")
endif()

set(MEGLER_SQLITE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/aurora/sqlite")
foreach(file sqlite3.c sqlite3.h)
    if(NOT EXISTS "${MEGLER_SQLITE_DIR}/${file}")
        message(FATAL_ERROR "SQLite source is missing: ${MEGLER_SQLITE_DIR}/${file}. See aurora/README.md.")
    endif()
endforeach()

add_executable(${APP_ID}
    aurora/main.cpp
    aurora/probe.h
    aurora/runtime_settings.h
    aurora/log_channel.cpp
    aurora/log_channel.h
    aurora/diagnostic_log.cpp
    aurora/diagnostic_log.h
    aurora/error_log.cpp
    aurora/error_log.h
    aurora/side_mouse_buttons.cpp
    aurora/side_mouse_buttons.h
    aurora/preview_activation_channel.cpp
    aurora/preview_activation_channel.h
    aurora/apk_store.cpp
    aurora/apk_store.h
    aurora/apk_bundle_install.cpp
    aurora/apk_bundle_install.h
    aurora/audio_output.cpp
    aurora/audio_output.h
    aurora/audio_queue.h
    aurora/audio_format.h
    aurora/audio_mp3_codec.cpp
    aurora/audio_mp3_codec.h
    aurora/audio_minimp3.h
    aurora/media_mp3_host.cpp
    aurora/media_mp3_host.h
    aurora/bluetooth_dbus.h
    aurora/camera_host.cpp
    aurora/camera_host.h
    aurora/camera_frame.h
    aurora/camera_qt_frame.h
    aurora/sensor_host.cpp
    aurora/sensor_host.h
    aurora/location_host.cpp
    aurora/location_host.h
    aurora/bluetooth_host.cpp
    aurora/bluetooth_host.h
    aurora/bluetooth_advertisement.cpp
    aurora/bluetooth_advertisement.h
    aurora/network_host.cpp
    aurora/network_host.h
    aurora/network_connman.h
    aurora/network_snapshot.h
    aurora/official_store_release.cpp
    aurora/official_store_release.h
    aurora/official_store_download.cpp
    aurora/official_store_download.h
    aurora/picker_host.cpp
    aurora/picker_host.h
    aurora/shared_folders.cpp
    aurora/shared_folders.h
    aurora/app_cache_clear.cpp
    aurora/app_cache_clear.h
    aurora/install_request_channel.cpp
    aurora/install_request_channel.h
    aurora/install_request_host.cpp
    aurora/install_request_host.h
    aurora/standalone_apk_export.cpp
    aurora/standalone_apk_export.h
    aurora/install_request_candidate.cpp
    aurora/install_request_candidate.h
    aurora/install_request_prepare.cpp
    aurora/install_request_prepare.h
    aurora/install_request_snapshot.cpp
    aurora/install_request_snapshot.h
    aurora/install_request_commit.cpp
    aurora/install_request_commit.h
    aurora/install_request_coordinator.cpp
    aurora/install_request_coordinator.h
    aurora/preview_sessions.cpp
    aurora/preview_sessions.h
    aurora/external_apk_path.cpp
    aurora/external_apk_path.h
    aurora/external_apk_install.cpp
    aurora/external_apk_install.h
    aurora/microg_install.cpp
    aurora/microg_install.h
    aurora/external_apk_receiver.cpp
    aurora/external_apk_receiver.h
    aurora/preview.cpp
    aurora/preview.h
    aurora/preview_viewport.h
    aurora/preview_text.cpp
    aurora/preview_text.h
    aurora/preview_image.cpp
    aurora/preview_image.h
    aurora/preview_keyboard.cpp
    aurora/preview_keyboard.h
    aurora/rpm_writer.cpp
    aurora/rpm_writer.h
    aurora/rpm_signers.cpp
    aurora/rpm_signers.h
    aurora/rpm_icon.cpp
    aurora/rpm_icon.h
    aurora/rpm_standalone.cpp
    aurora/rpm_standalone.h
    aurora/standalone_paths.cpp
    aurora/standalone_paths.h
    aurora/rpm_test_keys.h
    ${CORE_SOURCES}
    core/tests/mg_gfx_standin.c
    ${ZLIB_SOURCES}
)
add_dependencies(${APP_ID} megler_translations)
target_include_directories(${APP_ID} PRIVATE
    ${AURORA_INCLUDE_DIRS}
    ${RUNTIME_MANAGER_INCLUDE_DIRS}
    core/third_party/zlib
)
target_compile_definitions(${APP_ID} PRIVATE _GNU_SOURCE MEGLER_NO_EMBEDDED_TESTS)
if(MEGLER_EXPERIMENTAL_NATIVE_EGL)
    target_compile_definitions(${APP_ID} PRIVATE MG_EXPERIMENTAL_NATIVE_EGL)
endif()
if(MEGLER_EXPERIMENTAL_GPU_COMPOSITION)
    pkg_check_modules(EGL REQUIRED egl)
    pkg_check_modules(GLES2 REQUIRED glesv2)
    target_sources(${APP_ID} PRIVATE aurora/egl_frame_transport.c aurora/preview_gpu.cpp aurora/preview_gpu.h)
    target_compile_definitions(${APP_ID} PRIVATE MG_EXPERIMENTAL_GPU_COMPOSITION)
    target_include_directories(${APP_ID} PRIVATE ${EGL_INCLUDE_DIRS} ${GLES2_INCLUDE_DIRS})
    target_link_libraries(${APP_ID} PRIVATE ${EGL_LDFLAGS} ${GLES2_LDFLAGS})
endif()
target_link_libraries(${APP_ID} PRIVATE
    Qt5::Quick Qt5::Multimedia Qt5::Sensors Qt5::Network Qt5::DBus Qt5::Positioning
    ${AURORA_LDFLAGS} ${RUNTIME_MANAGER_LDFLAGS} m pthread dl
)

if(MEGLER_BUILD_TEST_TARGETS)
add_executable(megler_rpm_icon_test EXCLUDE_FROM_ALL aurora/tests/rpm_icon_test.cpp aurora/rpm_icon.cpp)
target_link_libraries(megler_rpm_icon_test PRIVATE Qt5::Gui)
add_executable(megler_rpm_stream_test EXCLUDE_FROM_ALL aurora/tests/rpm_stream_test.cpp aurora/rpm_writer.cpp)
target_link_libraries(megler_rpm_stream_test PRIVATE Qt5::Core)
add_executable(megler_rpm_sign_test EXCLUDE_FROM_ALL aurora/tests/rpm_sign_test.cpp aurora/rpm_signers.cpp)
target_link_libraries(megler_rpm_sign_test PRIVATE Qt5::Core ${CMAKE_DL_LIBS})
add_executable(megler_rpm_standalone_test EXCLUDE_FROM_ALL aurora/tests/rpm_standalone_test.cpp
    aurora/rpm_standalone.cpp aurora/rpm_icon.cpp aurora/rpm_writer.cpp aurora/apk_store.cpp)
target_link_libraries(megler_rpm_standalone_test PRIVATE Qt5::Gui)
add_executable(megler_standalone_paths_test EXCLUDE_FROM_ALL
    aurora/tests/standalone_paths_test.cpp aurora/standalone_paths.cpp aurora/apk_store.cpp)
target_link_libraries(megler_standalone_paths_test PRIVATE Qt5::Core)
add_executable(megler_network_snapshot_test EXCLUDE_FROM_ALL aurora/tests/network_snapshot_test.cpp)
add_executable(megler_native_egl_system_test EXCLUDE_FROM_ALL
    tools/native-egl-system-test.c
    ${CORE_SOURCES}
    ${ZLIB_SOURCES}
)
target_include_directories(megler_native_egl_system_test PRIVATE core/third_party/zlib)
target_compile_definitions(megler_native_egl_system_test PRIVATE _GNU_SOURCE MEGLER_NO_EMBEDDED_TESTS MG_EXPERIMENTAL_NATIVE_EGL)
target_link_libraries(megler_native_egl_system_test PRIVATE m pthread dl)

add_executable(megler_native_egl_image_test EXCLUDE_FROM_ALL
    tools/native-egl-image-test.c aurora/egl_frame_transport.c)
target_link_libraries(megler_native_egl_image_test PRIVATE EGL GLESv2)
add_executable(megler_preview_gpu_order_test EXCLUDE_FROM_ALL aurora/tests/preview_gpu_order_test.cpp)
add_executable(megler_gl_native_test EXCLUDE_FROM_ALL tools/gl-native-test-main.c ${CORE_SOURCES} ${TEST_SOURCES} ${ZLIB_SOURCES})
target_include_directories(megler_gl_native_test PRIVATE core/third_party/zlib)
target_compile_definitions(megler_gl_native_test PRIVATE _GNU_SOURCE)
target_link_libraries(megler_gl_native_test PRIVATE m pthread dl)

add_library(jni_load_fixture SHARED EXCLUDE_FROM_ALL aurora/tests/jni_load_fixture.c)
add_library(jni_load_failure SHARED EXCLUDE_FROM_ALL aurora/tests/jni_load_fixture.c)
target_compile_definitions(jni_load_failure PRIVATE JNI_LOAD_FAILURE)
set_target_properties(jni_load_fixture jni_load_failure PROPERTIES LINK_FLAGS "-nostdlib")
add_executable(megler_jni_load_test EXCLUDE_FROM_ALL aurora/tests/jni_load_test.c ${CORE_SOURCES} ${ZLIB_SOURCES})
target_include_directories(megler_jni_load_test PRIVATE core/third_party/zlib)
target_compile_definitions(megler_jni_load_test PRIVATE _GNU_SOURCE MEGLER_NO_EMBEDDED_TESTS)
target_link_libraries(megler_jni_load_test PRIVATE m pthread dl)
add_dependencies(megler_jni_load_test jni_load_fixture jni_load_failure)

add_executable(megler_network_host_test EXCLUDE_FROM_ALL
    aurora/tests/network_host_test.cpp aurora/network_host.cpp)
target_link_libraries(megler_network_host_test PRIVATE Qt5::Core Qt5::Network Qt5::DBus)
add_executable(megler_network_connman_dbus_test EXCLUDE_FROM_ALL
    aurora/tests/network_connman_dbus_test.cpp)
target_link_libraries(megler_network_connman_dbus_test PRIVATE Qt5::Core Qt5::DBus)
add_executable(megler_location_host_test EXCLUDE_FROM_ALL
    aurora/tests/location_host_test.cpp aurora/location_host.cpp aurora/location_host_test_support.h)
target_compile_definitions(megler_location_host_test PRIVATE MEGLER_LOCATION_HOST_TESTING)
target_link_libraries(megler_location_host_test PRIVATE Qt5::Core Qt5::Positioning)

add_executable(megler_bluetooth_advertisement_test EXCLUDE_FROM_ALL
    aurora/tests/bluetooth_advertisement_test.cpp aurora/bluetooth_advertisement.cpp)
target_link_libraries(megler_bluetooth_advertisement_test PRIVATE Qt5::Core)

pkg_check_modules(DBUS_TEST QUIET dbus-1)
if(DBUS_TEST_FOUND)
    add_executable(megler_bluetooth_dbus_test EXCLUDE_FROM_ALL
        aurora/tests/bluetooth_dbus_test.cpp)
    target_include_directories(megler_bluetooth_dbus_test PRIVATE ${DBUS_TEST_INCLUDE_DIRS})
    target_compile_options(megler_bluetooth_dbus_test PRIVATE ${DBUS_TEST_CFLAGS_OTHER})
    target_link_libraries(megler_bluetooth_dbus_test PRIVATE
        Qt5::Core Qt5::DBus ${DBUS_TEST_LDFLAGS})
endif()

add_executable(megler_preview_text_test EXCLUDE_FROM_ALL
    aurora/tests/preview_text_test.cpp
    aurora/preview_text.cpp
    aurora/preview_image.cpp
    ${CORE_SOURCES}
    ${TEST_SOURCES}
    ${ZLIB_SOURCES}
)
target_include_directories(megler_preview_text_test PRIVATE core/third_party/zlib)
target_compile_definitions(megler_preview_text_test PRIVATE _GNU_SOURCE)
target_link_libraries(megler_preview_text_test PRIVATE Qt5::Gui m pthread dl)

add_executable(megler_preview_image_test EXCLUDE_FROM_ALL
    aurora/tests/preview_image_test.cpp
    aurora/preview_image.cpp
)
target_link_libraries(megler_preview_image_test PRIVATE Qt5::Gui)

add_executable(megler_preview_frame_test EXCLUDE_FROM_ALL aurora/tests/preview_frame_test.cpp)
target_link_libraries(megler_preview_frame_test PRIVATE Qt5::Gui)

add_executable(megler_preview_keyboard_test EXCLUDE_FROM_ALL
    aurora/tests/preview_keyboard_test.cpp
    aurora/preview_keyboard.cpp
    aurora/preview_keyboard.h
)
target_link_libraries(megler_preview_keyboard_test PRIVATE Qt5::Quick pthread)

add_executable(megler_audio_queue_test EXCLUDE_FROM_ALL aurora/tests/audio_queue_test.cpp)
target_link_libraries(megler_audio_queue_test PRIVATE pthread)

add_executable(megler_camera_frame_test EXCLUDE_FROM_ALL aurora/tests/camera_frame_test.cpp)
add_executable(megler_camera_qt_frame_test EXCLUDE_FROM_ALL aurora/tests/camera_qt_frame_test.cpp)
target_link_libraries(megler_camera_qt_frame_test PRIVATE Qt5::Multimedia)

add_executable(megler_preview_viewport_test EXCLUDE_FROM_ALL aurora/tests/preview_viewport_test.cpp)
target_link_libraries(megler_preview_viewport_test PRIVATE Qt5::Core)

add_executable(megler_runtime_settings_test EXCLUDE_FROM_ALL aurora/tests/runtime_settings_test.cpp)
add_executable(megler_translation_test EXCLUDE_FROM_ALL aurora/tests/translation_test.cpp)
target_link_libraries(megler_translation_test PRIVATE Qt5::Core)
add_dependencies(megler_translation_test megler_translations)
target_link_libraries(megler_runtime_settings_test PRIVATE Qt5::Core)

add_executable(megler_shared_folders_test EXCLUDE_FROM_ALL
    aurora/tests/shared_folders_test.cpp aurora/shared_folders.cpp)
target_link_libraries(megler_shared_folders_test PRIVATE Qt5::Core)

add_executable(megler_app_cache_clear_test EXCLUDE_FROM_ALL
    aurora/tests/app_cache_clear_test.cpp aurora/app_cache_clear.cpp)
target_link_libraries(megler_app_cache_clear_test PRIVATE Qt5::Core)

add_executable(megler_google_services_runtime_test EXCLUDE_FROM_ALL aurora/tests/google_services_runtime_test.cpp)
target_link_libraries(megler_google_services_runtime_test PRIVATE Qt5::Core)

add_executable(megler_official_store_release_test EXCLUDE_FROM_ALL
    aurora/tests/official_store_release_test.cpp aurora/official_store_release.cpp)
target_link_libraries(megler_official_store_release_test PRIVATE Qt5::Core)

add_executable(megler_official_store_download_test EXCLUDE_FROM_ALL
    aurora/tests/official_store_download_test.cpp aurora/official_store_download.cpp
    aurora/official_store_download.h aurora/official_store_release.cpp)
target_link_libraries(megler_official_store_download_test PRIVATE Qt5::Network)

add_executable(megler_download_notification_test EXCLUDE_FROM_ALL
    aurora/tests/download_notification_test.cpp)
target_link_libraries(megler_download_notification_test PRIVATE Qt5::Quick Qt5::DBus)

add_executable(megler_microg_install_test EXCLUDE_FROM_ALL
    aurora/tests/microg_install_test.cpp aurora/microg_install.cpp aurora/microg_install.h
    aurora/external_apk_path.cpp)
target_link_libraries(megler_microg_install_test PRIVATE Qt5::Network)

add_executable(megler_microg_preview_paths_test EXCLUDE_FROM_ALL
    aurora/tests/microg_preview_paths_test.cpp aurora/microg_preview_paths.h)
target_link_libraries(megler_microg_preview_paths_test PRIVATE Qt5::Core)

add_executable(megler_log_channel_test EXCLUDE_FROM_ALL
    aurora/tests/log_channel_test.cpp aurora/log_channel.cpp aurora/log_channel.h)
target_link_libraries(megler_log_channel_test PRIVATE Qt5::Core)

add_executable(megler_error_log_test EXCLUDE_FROM_ALL
    aurora/tests/error_log_test.cpp aurora/error_log.cpp aurora/error_log.h
    aurora/diagnostic_log.cpp aurora/diagnostic_log.h)
target_compile_definitions(megler_error_log_test PRIVATE _GNU_SOURCE)
target_link_libraries(megler_error_log_test PRIVATE Qt5::Core pthread)

add_executable(megler_preview_activation_channel_test EXCLUDE_FROM_ALL
    aurora/tests/preview_activation_channel_test.cpp
    aurora/preview_activation_channel.cpp aurora/preview_activation_channel.h)
target_link_libraries(megler_preview_activation_channel_test PRIVATE Qt5::Gui Qt5::Network)

add_executable(megler_audio_output_test EXCLUDE_FROM_ALL
    aurora/tests/audio_output_test.cpp
    aurora/audio_output.cpp
)
target_link_libraries(megler_audio_output_test PRIVATE Qt5::Multimedia pthread)

add_executable(megler_audio_resampling_test EXCLUDE_FROM_ALL aurora/tests/audio_resampling_test.c)
target_compile_options(megler_audio_resampling_test PRIVATE -ffunction-sections -fdata-sections)
target_link_libraries(megler_audio_resampling_test PRIVATE m -Wl,--gc-sections)

add_executable(megler_audio_mp3_codec_test EXCLUDE_FROM_ALL
    aurora/tests/audio_mp3_codec_test.cpp
    aurora/audio_mp3_codec.cpp
)
target_link_libraries(megler_audio_mp3_codec_test PRIVATE pthread)

add_executable(megler_media_mp3_host_test EXCLUDE_FROM_ALL
    aurora/tests/media_mp3_host_test.cpp
    aurora/media_mp3_host.cpp
)
target_link_libraries(megler_media_mp3_host_test PRIVATE Qt5::Multimedia)

add_executable(megler_audio_preview_codec_test EXCLUDE_FROM_ALL
    aurora/tests/audio_preview_codec_test.cpp
    aurora/preview.cpp
    aurora/preview.h
    aurora/error_log.cpp aurora/error_log.h
    aurora/shared_folders.cpp
    aurora/shared_folders.h
    aurora/audio_mp3_codec.cpp
    aurora/media_mp3_host.cpp
    aurora/log_channel.cpp
    aurora/log_channel.h
)
target_link_libraries(megler_audio_preview_codec_test PRIVATE Qt5::Quick Qt5::Multimedia Qt5::Network Qt5::DBus pthread)

add_executable(megler_audio_mixer_path_test EXCLUDE_FROM_ALL
    aurora/tests/audio_mixer_path_test.cpp
    aurora/audio_output.cpp
    aurora/audio_mp3_codec.cpp
    core/src/ndk/mg_audio.c
    core/src/ndk/mg_aaudio.c
    core/src/drivers/mg_java_media.c
    core/src/mg_base.c
    core/src/host/mg_host_common.c
    core/src/host/mg_host_posix.c
)
target_compile_definitions(megler_audio_mixer_path_test PRIVATE _GNU_SOURCE)
target_compile_options(megler_audio_mixer_path_test PRIVATE -ffunction-sections -fdata-sections)
target_link_libraries(megler_audio_mixer_path_test PRIVATE Qt5::Multimedia pthread dl m -Wl,--gc-sections)

add_executable(megler_apk_store_test EXCLUDE_FROM_ALL
    aurora/tests/apk_store_test.cpp
    aurora/apk_store.cpp
    ${CORE_SOURCES}
    ${TEST_SOURCES}
    ${ZLIB_SOURCES}
)
target_include_directories(megler_apk_store_test PRIVATE core/third_party/zlib)
target_compile_definitions(megler_apk_store_test PRIVATE _GNU_SOURCE)
target_link_libraries(megler_apk_store_test PRIVATE Qt5::Core m pthread dl)

add_executable(megler_apk_store_library_test EXCLUDE_FROM_ALL
    aurora/tests/apk_store_library_test.cpp
    aurora/apk_store.cpp
)
target_link_libraries(megler_apk_store_library_test PRIVATE Qt5::Core)

add_executable(megler_install_request_candidate_test EXCLUDE_FROM_ALL
    aurora/tests/install_request_candidate_test.cpp
    aurora/install_request_candidate.cpp aurora/install_request_prepare.cpp aurora/install_request_prepare.h
    aurora/install_request_snapshot.cpp)
target_link_libraries(megler_install_request_candidate_test PRIVATE Qt5::Core)

add_executable(megler_install_request_channel_test EXCLUDE_FROM_ALL
    aurora/tests/install_request_channel_test.cpp aurora/install_request_channel.cpp aurora/install_request_channel.h)
target_link_libraries(megler_install_request_channel_test PRIVATE Qt5::Network)

add_executable(megler_install_request_host_test EXCLUDE_FROM_ALL
    aurora/tests/install_request_host_test.cpp aurora/install_request_channel.cpp aurora/install_request_channel.h
    aurora/install_request_host.cpp aurora/install_request_host.h)
target_link_libraries(megler_install_request_host_test PRIVATE Qt5::Network)

add_executable(megler_standalone_apk_export_test EXCLUDE_FROM_ALL
    aurora/tests/standalone_apk_export_test.cpp
    aurora/standalone_apk_export.cpp aurora/standalone_apk_export.h
    aurora/external_apk_install.cpp aurora/external_apk_install.h aurora/external_apk_path.cpp
    aurora/install_request_candidate.cpp aurora/install_request_prepare.cpp aurora/install_request_prepare.h
    aurora/install_request_snapshot.cpp aurora/install_request_commit.cpp)
target_link_libraries(megler_standalone_apk_export_test PRIVATE Qt5::Gui)

add_executable(megler_preview_sessions_test EXCLUDE_FROM_ALL
    aurora/tests/preview_sessions_test.cpp
    aurora/preview_sessions.cpp aurora/preview_sessions.h
    aurora/diagnostic_log.cpp aurora/diagnostic_log.h
    aurora/error_log.cpp aurora/error_log.h
    aurora/preview_activation_channel.cpp aurora/preview_activation_channel.h
    aurora/log_channel.cpp aurora/log_channel.h
    aurora/install_request_channel.cpp aurora/install_request_channel.h
    aurora/install_request_host.cpp aurora/install_request_host.h
    aurora/install_request_candidate.cpp aurora/install_request_prepare.cpp aurora/install_request_prepare.h
    aurora/install_request_snapshot.cpp aurora/install_request_commit.cpp
    aurora/install_request_coordinator.cpp aurora/install_request_coordinator.h)
target_compile_definitions(megler_preview_sessions_test PRIVATE _GNU_SOURCE)
target_link_libraries(megler_preview_sessions_test PRIVATE Qt5::Network Qt5::Gui)

if(NOT CMAKE_CROSSCOMPILING)
    add_executable(megler_probe_multitask_test EXCLUDE_FROM_ALL
        aurora/tests/probe_multitask_test.cpp aurora/probe.h
        aurora/official_store_download.cpp aurora/official_store_download.h aurora/official_store_release.cpp
        aurora/apk_store.cpp aurora/external_apk_install.cpp aurora/external_apk_install.h
        aurora/apk_bundle_install.cpp aurora/apk_bundle_install.h
        aurora/microg_install.cpp aurora/microg_install.h
        aurora/shared_folders.cpp aurora/shared_folders.h
        aurora/external_apk_path.cpp aurora/preview_sessions.cpp aurora/preview_sessions.h
        aurora/diagnostic_log.cpp aurora/diagnostic_log.h
        aurora/error_log.cpp aurora/error_log.h
        aurora/preview_activation_channel.cpp aurora/preview_activation_channel.h
        aurora/log_channel.cpp aurora/log_channel.h
        aurora/install_request_channel.cpp aurora/install_request_channel.h
        aurora/install_request_host.cpp aurora/install_request_host.h
        aurora/install_request_candidate.cpp aurora/install_request_prepare.cpp aurora/install_request_prepare.h
        aurora/install_request_snapshot.cpp aurora/install_request_commit.cpp
        aurora/install_request_coordinator.cpp aurora/install_request_coordinator.h
        aurora/rpm_writer.cpp aurora/rpm_signers.cpp aurora/rpm_icon.cpp)
    target_compile_definitions(megler_probe_multitask_test PRIVATE _GNU_SOURCE)
    target_link_libraries(megler_probe_multitask_test PRIVATE Qt5::Quick Qt5::Network ${CMAKE_DL_LIBS})
endif()

add_executable(megler_install_request_commit_test EXCLUDE_FROM_ALL
    aurora/tests/install_request_commit_test.cpp aurora/install_request_commit.cpp)
target_compile_definitions(megler_install_request_commit_test PRIVATE _GNU_SOURCE)
target_link_libraries(megler_install_request_commit_test PRIVATE Qt5::Core)

add_executable(megler_install_request_real_test EXCLUDE_FROM_ALL
    aurora/tests/install_request_real_test.cpp aurora/install_request_candidate.cpp
    aurora/install_request_snapshot.cpp aurora/apk_store.cpp
    ${CORE_SOURCES} ${TEST_SOURCES} ${ZLIB_SOURCES})
target_include_directories(megler_install_request_real_test PRIVATE core/third_party/zlib)
target_compile_definitions(megler_install_request_real_test PRIVATE _GNU_SOURCE)
target_link_libraries(megler_install_request_real_test PRIVATE Qt5::Core m pthread dl)

add_executable(megler_install_request_integration_test EXCLUDE_FROM_ALL
    aurora/tests/install_request_integration_test.cpp
    aurora/install_request_channel.cpp aurora/install_request_channel.h
    aurora/install_request_host.cpp aurora/install_request_host.h
    aurora/install_request_candidate.cpp aurora/install_request_prepare.cpp aurora/install_request_prepare.h
    aurora/install_request_snapshot.cpp aurora/install_request_commit.cpp
    aurora/install_request_coordinator.cpp aurora/install_request_coordinator.h
    ${CORE_SOURCES} ${TEST_SOURCES} ${ZLIB_SOURCES})
target_include_directories(megler_install_request_integration_test PRIVATE core/third_party/zlib)
target_compile_definitions(megler_install_request_integration_test PRIVATE _GNU_SOURCE)
target_link_libraries(megler_install_request_integration_test PRIVATE Qt5::Network m pthread dl)

foreach(target megler_preview_text_test megler_apk_store_test megler_install_request_real_test megler_install_request_integration_test)
    target_include_directories(${target} PRIVATE "${MEGLER_SQLITE_DIR}")
    target_compile_definitions(${target} PRIVATE MG_HAVE_SQLITE=1)
endforeach()
endif()

target_include_directories(${APP_ID} PRIVATE "${MEGLER_SQLITE_DIR}")
target_compile_definitions(${APP_ID} PRIVATE MG_HAVE_SQLITE=1)

install(TARGETS ${APP_ID} RUNTIME DESTINATION bin)
set(ANDROID_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/aurora/android-runtime")
foreach(file framework/megler-libcore.dex framework/core-oj.dex framework/core-libart.dex
             framework/conscrypt.dex framework/okhttp.dex framework/bouncycastle.dex framework/apache-xml.dex
             framework/megler-framework.dex framework/megler-framework-classes2.dex
             lib/libcrypto.so lib/libssl.so lib/libexpat.so lib/libicuuc.so lib/libicui18n.so
             lib/libjavacore.so lib/libjavacrypto.so lib/libopenjdk.so lib/libnativehelper.so
             usr/icu/icudt60l.dat framework/framework-res.apk)
    if(NOT EXISTS "${ANDROID_ROOT}/${file}")
        message(FATAL_ERROR "Android runtime is incomplete: ${ANDROID_ROOT}/${file}. See aurora/README.md.")
    endif()
endforeach()
install(DIRECTORY "${ANDROID_ROOT}/" DESTINATION share/${APP_ID}/android-runtime
    FILE_PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ
    PATTERN "fixtures" EXCLUDE
    PATTERN "platform-framework*.dex" EXCLUDE)
install(DIRECTORY qml DESTINATION share/${APP_ID})
install(FILES ${MEGLER_TRANSLATIONS} DESTINATION share/${APP_ID}/qml)
install(FILES ${APP_ID}.desktop DESTINATION share/applications)
foreach(size 86 108 128 172)
    install(FILES aurora/icons/${size}.png DESTINATION share/icons/hicolor/${size}x${size}/apps
        RENAME ${APP_ID}.png)
endforeach()

add_custom_target(project_files SOURCES
    ${APP_ID}.desktop
    rpm/${APP_ID}.spec
    qml/MeglerProbe.qml
    qml/Preview.qml
    aurora/icons/86.png
    aurora/icons/108.png
    aurora/icons/128.png
    aurora/icons/172.png
)
file(WRITE "${CMAKE_BINARY_DIR}/QtCreatorDeployment.txt"
    "${CMAKE_INSTALL_PREFIX}\n${CMAKE_BINARY_DIR}/${APP_ID}:bin\n")
