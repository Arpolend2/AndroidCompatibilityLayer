cmake_minimum_required(VERSION 3.12)
project(AndroidCompatibilityLayer LANGUAGES C CXX)

option(ANDROID_COMPATIBILITY_LAYER_BUILD_TEST_TARGETS "Generate optional test targets" ON)

set(APP_ID ru.androidcompatibilitylayer.appmanager)
set(MANAGER_SPEC "${CMAKE_CURRENT_SOURCE_DIR}/rpm/ru.androidcompatibilitylayer.appmanager.spec")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${MANAGER_SPEC}")
foreach(field Version Release)
    file(STRINGS "${MANAGER_SPEC}" manager_field REGEX "^${field}:[ \t]*")
    if(NOT manager_field MATCHES "^${field}:[ \t]*([0-9][A-Za-z0-9._+~^]*)[ \t]*$")
        message(FATAL_ERROR "The manager RPM spec must contain a literal ${field}")
    endif()
    set(MANAGER_${field} "${CMAKE_MATCH_1}")
endforeach()
set_source_files_properties(aurora/main.cpp PROPERTIES COMPILE_DEFINITIONS
    "ACL_MANAGER_BUILD_VERSION=\"${MANAGER_Version}-${MANAGER_Release}\"")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/aurora/standalone-runtime-abi")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/aurora/standalone-runtime-abi" STANDALONE_RUNTIME_ABI)
string(STRIP "${STANDALONE_RUNTIME_ABI}" STANDALONE_RUNTIME_ABI)
if(NOT STANDALONE_RUNTIME_ABI MATCHES "^[1-9][0-9]*$" OR
   STANDALONE_RUNTIME_ABI GREATER 999999999)
    message(FATAL_ERROR "Invalid standalone runtime ABI marker")
endif()

find_package(Qt5 COMPONENTS Core Gui Qml Quick Multimedia Sensors Network DBus Positioning LinguistTools REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(AURORA REQUIRED auroraapp)
pkg_check_modules(RUNTIME_MANAGER REQUIRED runtime-manager-qt5)
pkg_check_modules(AURORA_WEBVIEW REQUIRED aurorawebview)

set(CMAKE_AUTOMOC ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
qt5_add_translation(ANDROID_COMPATIBILITY_LAYER_TRANSLATIONS translations/androidcompatibilitylayer_ru.ts)
add_custom_target(android_compatibility_layer_translations ALL DEPENDS ${ANDROID_COMPATIBILITY_LAYER_TRANSLATIONS})

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

set(ANDROID_COMPATIBILITY_LAYER_SQLITE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/aurora/sqlite")
foreach(file sqlite3.c sqlite3.h)
    if(NOT EXISTS "${ANDROID_COMPATIBILITY_LAYER_SQLITE_DIR}/${file}")
        message(FATAL_ERROR "SQLite source is missing: ${ANDROID_COMPATIBILITY_LAYER_SQLITE_DIR}/${file}. See aurora/README.md.")
    endif()
endforeach()

add_library(acl_runtime_objects OBJECT
    aurora/main.cpp
    aurora/microg_watchdog.cpp
    aurora/microg_watchdog.h
    aurora/microg_watchdog_schedule.h
    aurora/probe.h
    aurora/runtime_settings.h
    aurora/log_channel.cpp
    aurora/log_channel.h
    aurora/diagnostic_log.cpp
    aurora/diagnostic_log.h
    aurora/error_log.cpp
    aurora/error_log.h
    aurora/memory_log_bus.cpp
    aurora/memory_log_bus.h
    aurora/side_mouse_buttons.cpp
    aurora/side_mouse_buttons.h
    aurora/preview_activation_channel.cpp
    aurora/preview_activation_channel.h
    aurora/apk_store.cpp
    aurora/apk_store.h
    aurora/library_app_index.cpp
    aurora/library_app_index.h
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
    aurora/manager_update.cpp
    aurora/manager_update.h
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
    aurora/standalone_start.cpp
    aurora/standalone_start.h
    aurora/standalone_app_mapping.cpp
    aurora/standalone_app_mapping.h
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
    aurora/manager_lifecycle.cpp
    aurora/manager_lifecycle.h
    aurora/single_instance_lock.cpp
    aurora/single_instance_lock.h
    aurora/shared_microg_ipc.cpp
    aurora/shared_microg_ipc.h
    aurora/shared_microg_supervisor.cpp
    aurora/shared_microg_supervisor.h
    aurora/shared_microg_worker.c
    aurora/shared_microg_worker.h
    aurora/preview.cpp
    aurora/preview.h
    aurora/preview_frame_export.cpp
    aurora/preview_frame_export.h
    aurora/notification_host.cpp aurora/notification_host.h
    aurora/embedded_preview.cpp
    aurora/embedded_preview.h
    aurora/store_preview_supervisor.cpp
    aurora/store_preview_supervisor.h
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
if(ANDROID_COMPATIBILITY_LAYER_FFMPEG7_INCLUDE_DIR)
    foreach(header
            libavcodec/avcodec.h
            libavcodec/version_major.h
            libavutil/avconfig.h
            libavutil/version.h)
        if(NOT EXISTS "${ANDROID_COMPATIBILITY_LAYER_FFMPEG7_INCLUDE_DIR}/${header}")
            message(FATAL_ERROR
                "FFmpeg 7 include directory is incomplete: missing ${ANDROID_COMPATIBILITY_LAYER_FFMPEG7_INCLUDE_DIR}/${header}. "
                "Run tools/prepare-ffmpeg7-headers.py --arch aarch64.")
        endif()
    endforeach()
    file(STRINGS "${ANDROID_COMPATIBILITY_LAYER_FFMPEG7_INCLUDE_DIR}/libavcodec/version_major.h"
        FFMPEG7_CODEC_VERSION_LINE REGEX "^#define LIBAVCODEC_VERSION_MAJOR")
    file(STRINGS "${ANDROID_COMPATIBILITY_LAYER_FFMPEG7_INCLUDE_DIR}/libavutil/version.h"
        FFMPEG7_UTIL_VERSION_LINE REGEX "^#define LIBAVUTIL_VERSION_MAJOR")
    string(REGEX MATCH "[0-9]+" FFMPEG7_CODEC_MAJOR "${FFMPEG7_CODEC_VERSION_LINE}")
    string(REGEX MATCH "[0-9]+" FFMPEG7_UTIL_MAJOR "${FFMPEG7_UTIL_VERSION_LINE}")
    if(NOT FFMPEG7_CODEC_MAJOR STREQUAL "61" OR NOT FFMPEG7_UTIL_MAJOR STREQUAL "59")
        message(FATAL_ERROR "FFmpeg bridge requires libavcodec 61 and libavutil 59 headers.")
    endif()
    set(FFMPEG7_EXPECTED_ARCH "")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
        set(FFMPEG7_EXPECTED_ARCH "aarch64")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64)$")
        set(FFMPEG7_EXPECTED_ARCH "x86_64")
    endif()
    if(FFMPEG7_EXPECTED_ARCH AND EXISTS
            "${ANDROID_COMPATIBILITY_LAYER_FFMPEG7_INCLUDE_DIR}/FFmpeg-HEADERS.txt")
        file(STRINGS "${ANDROID_COMPATIBILITY_LAYER_FFMPEG7_INCLUDE_DIR}/FFmpeg-HEADERS.txt"
            FFMPEG7_ARCH_LINE REGEX "^architecture=")
        if(NOT FFMPEG7_ARCH_LINE STREQUAL "architecture=${FFMPEG7_EXPECTED_ARCH}")
            message(FATAL_ERROR
                "FFmpeg headers were prepared for '${FFMPEG7_ARCH_LINE}', but this build targets '${FFMPEG7_EXPECTED_ARCH}'.")
        endif()
    endif()
    target_sources(acl_runtime_objects PRIVATE
        aurora/codec_audio.cpp aurora/codec_audio.h aurora/codec_media_host.cpp)
    target_include_directories(acl_runtime_objects PRIVATE
        "${ANDROID_COMPATIBILITY_LAYER_FFMPEG7_INCLUDE_DIR}")
    target_compile_definitions(acl_runtime_objects PRIVATE ACL_WITH_FFMPEG7_MEDIA)
endif()
add_executable(${APP_ID} aurora/application_entry.cpp aurora/report_webview.cpp)
add_executable(acl-runtime aurora/application_entry.cpp)
set_target_properties(acl-runtime PROPERTIES OUTPUT_NAME "${APP_ID}.runtime")
target_link_libraries(${APP_ID} PRIVATE acl_runtime_objects)
target_link_libraries(acl-runtime PRIVATE acl_runtime_objects)
add_dependencies(${APP_ID} acl-runtime android_compatibility_layer_translations)
set_target_properties(${APP_ID} PROPERTIES CXX_STANDARD 17)
target_compile_definitions(${APP_ID} PRIVATE ACL_WITH_REPORT_WEBVIEW)
target_include_directories(${APP_ID} PRIVATE ${AURORA_WEBVIEW_INCLUDE_DIRS})
# Keep the SDK's ordered CEF link flags, but do not inherit SDK build paths.
set(REPORT_WEBVIEW_LINK_FLAGS)
set(REPORT_WEBVIEW_RPATH)
foreach(flag IN LISTS AURORA_WEBVIEW_LDFLAGS)
    if(flag MATCHES "^-Wl,-rpath,(/usr/lib(64)?/cef)$")
        list(APPEND REPORT_WEBVIEW_RPATH "${CMAKE_MATCH_1}")
    elseif(NOT flag MATCHES "^-Wl,-rpath," AND NOT flag MATCHES "^(-pie|-rdynamic)$")
        list(APPEND REPORT_WEBVIEW_LINK_FLAGS "${flag}")
    endif()
endforeach()
if(NOT REPORT_WEBVIEW_RPATH)
    message(FATAL_ERROR "Aurora WebView pkg-config metadata did not specify a system CEF library path")
endif()
# Aurora's RPM build sets CMAKE_SKIP_RPATH, so pass only these SDK system
# paths explicitly to the linker for the manager only.
foreach(path IN LISTS REPORT_WEBVIEW_RPATH)
    target_link_options(${APP_ID} PRIVATE "LINKER:-rpath,${path}")
endforeach()
target_link_libraries(${APP_ID} PRIVATE ${REPORT_WEBVIEW_LINK_FLAGS})
target_include_directories(acl_runtime_objects PRIVATE
    ${AURORA_INCLUDE_DIRS}
    ${RUNTIME_MANAGER_INCLUDE_DIRS}
    core/third_party/zlib
)
target_compile_definitions(acl_runtime_objects PRIVATE _GNU_SOURCE MEGLER_NO_EMBEDDED_TESTS)
target_compile_definitions(acl_runtime_objects PRIVATE STANDALONE_RUNTIME_ABI="${STANDALONE_RUNTIME_ABI}")
pkg_check_modules(EGL REQUIRED egl)
pkg_check_modules(GLES2 REQUIRED glesv2)
target_sources(acl_runtime_objects PRIVATE
    aurora/egl_frame_transport.c aurora/preview_gpu.cpp aurora/preview_gpu.h
    aurora/embedded_gpu_transport.cpp aurora/embedded_gpu_transport.h
    aurora/preview_gpu_canvas.cpp aurora/preview_gpu_canvas.h)
target_compile_definitions(acl_runtime_objects PRIVATE
    MG_EXPERIMENTAL_NATIVE_EGL MG_EXPERIMENTAL_GPU_COMPOSITION MG_EXPERIMENTAL_GPU_CANVAS)
target_include_directories(acl_runtime_objects PRIVATE ${EGL_INCLUDE_DIRS} ${GLES2_INCLUDE_DIRS})
target_link_libraries(acl_runtime_objects PUBLIC ${EGL_LDFLAGS} ${GLES2_LDFLAGS})
target_link_libraries(acl_runtime_objects PUBLIC
    Qt5::Quick Qt5::Multimedia Qt5::Sensors Qt5::Network Qt5::DBus Qt5::Positioning
    ${AURORA_LDFLAGS} ${RUNTIME_MANAGER_LDFLAGS} m pthread dl
)

if(ANDROID_COMPATIBILITY_LAYER_BUILD_TEST_TARGETS)
find_package(Qt5 COMPONENTS Test REQUIRED)
add_executable(android_compatibility_layer_report_webview_startup_test EXCLUDE_FROM_ALL
    aurora/tests/report_webview_startup_test.cpp aurora/report_webview.cpp)
set_target_properties(android_compatibility_layer_report_webview_startup_test PROPERTIES CXX_STANDARD 17)
target_compile_definitions(android_compatibility_layer_report_webview_startup_test PRIVATE ACL_WITH_REPORT_WEBVIEW)
target_include_directories(android_compatibility_layer_report_webview_startup_test PRIVATE ${AURORA_WEBVIEW_INCLUDE_DIRS})
target_link_libraries(android_compatibility_layer_report_webview_startup_test PRIVATE Qt5::Core Qt5::Gui)
add_executable(android_compatibility_layer_shared_microg_graphics_test EXCLUDE_FROM_ALL
    aurora/tests/shared_microg_graphics_test.cpp aurora/preview_text.cpp aurora/preview_image.cpp
    core/tests/microg-binder-boundary/worker_graphics_fixture.c core/tests/mg_gfx_standin.c
    ${ZLIB_SOURCES})
target_compile_options(android_compatibility_layer_shared_microg_graphics_test PRIVATE
    -ffunction-sections -fdata-sections)
target_link_libraries(android_compatibility_layer_shared_microg_graphics_test PRIVATE
    Qt5::Gui -Wl,--gc-sections m pthread)
add_subdirectory(aurora/tests/shared_microg_headless EXCLUDE_FROM_ALL)
    add_executable(android_compatibility_layer_webview_native_probe EXCLUDE_FROM_ALL aurora/tests/webview_native_probe.cpp)
    set_property(TARGET android_compatibility_layer_webview_native_probe PROPERTY CXX_STANDARD 17)
    target_include_directories(android_compatibility_layer_webview_native_probe PRIVATE ${AURORA_WEBVIEW_INCLUDE_DIRS})
    target_link_libraries(android_compatibility_layer_webview_native_probe PRIVATE Qt5::Quick ${AURORA_WEBVIEW_LDFLAGS})
add_executable(android_compatibility_layer_rpm_icon_test EXCLUDE_FROM_ALL aurora/tests/rpm_icon_test.cpp aurora/rpm_icon.cpp)
target_link_libraries(android_compatibility_layer_rpm_icon_test PRIVATE Qt5::Gui)
add_executable(android_compatibility_layer_log_actions_qml_test EXCLUDE_FROM_ALL
    aurora/tests/log_actions_qml_test.cpp)
target_link_libraries(android_compatibility_layer_log_actions_qml_test PRIVATE Qt5::Quick Qt5::Test)
add_executable(android_compatibility_layer_yandex_form_qml_test EXCLUDE_FROM_ALL
    aurora/tests/yandex_form_qml_test.cpp)
target_link_libraries(android_compatibility_layer_yandex_form_qml_test PRIVATE Qt5::Quick Qt5::Qml)
add_executable(android_compatibility_layer_rpm_stream_test EXCLUDE_FROM_ALL aurora/tests/rpm_stream_test.cpp aurora/rpm_writer.cpp)
target_link_libraries(android_compatibility_layer_rpm_stream_test PRIVATE Qt5::Core)
add_executable(android_compatibility_layer_rpm_sign_test EXCLUDE_FROM_ALL aurora/tests/rpm_sign_test.cpp aurora/rpm_signers.cpp)
target_link_libraries(android_compatibility_layer_rpm_sign_test PRIVATE Qt5::Core ${CMAKE_DL_LIBS})
add_executable(android_compatibility_layer_rpm_standalone_test EXCLUDE_FROM_ALL aurora/tests/rpm_standalone_test.cpp
    aurora/rpm_standalone.cpp aurora/rpm_icon.cpp aurora/rpm_writer.cpp aurora/apk_store.cpp
    aurora/standalone_app_mapping.cpp)
target_compile_definitions(android_compatibility_layer_rpm_standalone_test PRIVATE
    STANDALONE_RUNTIME_ABI="${STANDALONE_RUNTIME_ABI}")
target_link_libraries(android_compatibility_layer_rpm_standalone_test PRIVATE Qt5::Gui)
add_executable(android_compatibility_layer_standalone_paths_test EXCLUDE_FROM_ALL
    aurora/tests/standalone_paths_test.cpp aurora/standalone_paths.cpp aurora/apk_store.cpp)
target_link_libraries(android_compatibility_layer_standalone_paths_test PRIVATE Qt5::Core)
add_executable(android_compatibility_layer_network_snapshot_test EXCLUDE_FROM_ALL aurora/tests/network_snapshot_test.cpp)
add_executable(android_compatibility_layer_native_egl_system_test EXCLUDE_FROM_ALL
    tools/native-egl-system-test.c
    ${CORE_SOURCES}
    ${ZLIB_SOURCES}
)
target_include_directories(android_compatibility_layer_native_egl_system_test PRIVATE core/third_party/zlib)
target_compile_definitions(android_compatibility_layer_native_egl_system_test PRIVATE _GNU_SOURCE MEGLER_NO_EMBEDDED_TESTS MG_EXPERIMENTAL_NATIVE_EGL)
target_link_libraries(android_compatibility_layer_native_egl_system_test PRIVATE m pthread dl)

add_executable(android_compatibility_layer_native_egl_image_test EXCLUDE_FROM_ALL
    tools/native-egl-image-test.c aurora/egl_frame_transport.c)
target_link_libraries(android_compatibility_layer_native_egl_image_test PRIVATE EGL GLESv2)
add_executable(android_compatibility_layer_preview_gpu_order_test EXCLUDE_FROM_ALL aurora/tests/preview_gpu_order_test.cpp)
    add_executable(android_compatibility_layer_preview_gpu_state_test EXCLUDE_FROM_ALL
        aurora/tests/preview_gpu_state_test.cpp aurora/preview_gpu.cpp aurora/preview_gpu.h
        aurora/preview_frame_export.cpp aurora/embedded_gpu_transport.cpp aurora/embedded_gpu_transport.h
        aurora/egl_frame_transport.c)
    target_include_directories(android_compatibility_layer_preview_gpu_state_test PRIVATE ${EGL_INCLUDE_DIRS} ${GLES2_INCLUDE_DIRS})
    target_link_libraries(android_compatibility_layer_preview_gpu_state_test PRIVATE Qt5::Quick ${EGL_LDFLAGS} ${GLES2_LDFLAGS} pthread)
    add_executable(android_compatibility_layer_embedded_gpu_transport_test EXCLUDE_FROM_ALL
        aurora/tests/embedded_gpu_transport_test.cpp aurora/embedded_gpu_transport.cpp aurora/embedded_gpu_transport.h)
    target_link_libraries(android_compatibility_layer_embedded_gpu_transport_test PRIVATE
        Qt5::Core ${EGL_LDFLAGS} ${GLES2_LDFLAGS} pthread)
    add_executable(android_compatibility_layer_embedded_gpu_import_test EXCLUDE_FROM_ALL
        aurora/tests/embedded_gpu_import_test.cpp aurora/embedded_gpu_transport.cpp aurora/embedded_gpu_transport.h)
    target_link_libraries(android_compatibility_layer_embedded_gpu_import_test PRIVATE
        Qt5::Core ${EGL_LDFLAGS} ${GLES2_LDFLAGS} pthread)
add_executable(android_compatibility_layer_gl_native_test EXCLUDE_FROM_ALL tools/gl-native-test-main.c ${CORE_SOURCES} ${TEST_SOURCES} ${ZLIB_SOURCES})
target_include_directories(android_compatibility_layer_gl_native_test PRIVATE core/third_party/zlib)
target_compile_definitions(android_compatibility_layer_gl_native_test PRIVATE _GNU_SOURCE)
target_link_libraries(android_compatibility_layer_gl_native_test PRIVATE m pthread dl)

add_executable(android_compatibility_layer_gfx_native_test EXCLUDE_FROM_ALL tools/gfx-native-test-main.c ${CORE_SOURCES} ${TEST_SOURCES} ${ZLIB_SOURCES})
target_include_directories(android_compatibility_layer_gfx_native_test PRIVATE core/third_party/zlib)
target_compile_definitions(android_compatibility_layer_gfx_native_test PRIVATE _GNU_SOURCE)
target_link_libraries(android_compatibility_layer_gfx_native_test PRIVATE m pthread dl)
    pkg_check_modules(GPU_CANVAS_TEST_GLES2 REQUIRED glesv2)
    add_executable(android_compatibility_layer_gpu_canvas_test EXCLUDE_FROM_ALL
        aurora/tests/preview_gpu_canvas_test.cpp
        aurora/preview_gpu_canvas.cpp
        core/tests/mg_gfx_standin.c
        ${CORE_SOURCES}
        ${ZLIB_SOURCES}
    )
    target_include_directories(android_compatibility_layer_gpu_canvas_test PRIVATE core/third_party/zlib)
    target_compile_definitions(android_compatibility_layer_gpu_canvas_test PRIVATE _GNU_SOURCE MEGLER_NO_EMBEDDED_TESTS)
    target_link_libraries(android_compatibility_layer_gpu_canvas_test PRIVATE Qt5::Gui ${GPU_CANVAS_TEST_GLES2_LDFLAGS} m pthread dl)

add_library(jni_load_fixture SHARED EXCLUDE_FROM_ALL aurora/tests/jni_load_fixture.c)
add_library(jni_load_failure SHARED EXCLUDE_FROM_ALL aurora/tests/jni_load_fixture.c)
add_library(jni_unity_load_fixture SHARED EXCLUDE_FROM_ALL aurora/tests/jni_load_fixture.c)
target_compile_definitions(jni_load_failure PRIVATE JNI_LOAD_FAILURE)
target_compile_definitions(jni_unity_load_fixture PRIVATE JNI_LOAD_NO_WAIT)
set_target_properties(jni_unity_load_fixture PROPERTIES OUTPUT_NAME "FirebaseCppApp-13_6_0")
set_target_properties(jni_load_fixture jni_load_failure jni_unity_load_fixture PROPERTIES LINK_FLAGS "-nostdlib")
add_executable(android_compatibility_layer_jni_load_test EXCLUDE_FROM_ALL aurora/tests/jni_load_test.c ${CORE_SOURCES} ${ZLIB_SOURCES})
target_include_directories(android_compatibility_layer_jni_load_test PRIVATE core/third_party/zlib)
target_compile_definitions(android_compatibility_layer_jni_load_test PRIVATE _GNU_SOURCE MEGLER_NO_EMBEDDED_TESTS)
target_link_libraries(android_compatibility_layer_jni_load_test PRIVATE m pthread dl)
add_dependencies(android_compatibility_layer_jni_load_test jni_load_fixture jni_load_failure jni_unity_load_fixture)

add_executable(android_compatibility_layer_network_host_test EXCLUDE_FROM_ALL
    aurora/tests/network_host_test.cpp aurora/network_host.cpp)
target_link_libraries(android_compatibility_layer_network_host_test PRIVATE Qt5::Core Qt5::Network Qt5::DBus)
add_executable(android_compatibility_layer_network_connman_dbus_test EXCLUDE_FROM_ALL
    aurora/tests/network_connman_dbus_test.cpp)
target_link_libraries(android_compatibility_layer_network_connman_dbus_test PRIVATE Qt5::Core Qt5::DBus)
add_executable(android_compatibility_layer_location_host_test EXCLUDE_FROM_ALL
    aurora/tests/location_host_test.cpp aurora/location_host.cpp aurora/location_host_test_support.h)
target_compile_definitions(android_compatibility_layer_location_host_test PRIVATE ANDROID_COMPATIBILITY_LAYER_LOCATION_HOST_TESTING)
target_link_libraries(android_compatibility_layer_location_host_test PRIVATE Qt5::Core Qt5::Positioning)

add_executable(android_compatibility_layer_bluetooth_advertisement_test EXCLUDE_FROM_ALL
    aurora/tests/bluetooth_advertisement_test.cpp aurora/bluetooth_advertisement.cpp)
target_link_libraries(android_compatibility_layer_bluetooth_advertisement_test PRIVATE Qt5::Core)

pkg_check_modules(DBUS_TEST QUIET dbus-1)
if(DBUS_TEST_FOUND)
    add_executable(android_compatibility_layer_bluetooth_dbus_test EXCLUDE_FROM_ALL
        aurora/tests/bluetooth_dbus_test.cpp)
    target_include_directories(android_compatibility_layer_bluetooth_dbus_test PRIVATE ${DBUS_TEST_INCLUDE_DIRS})
    target_compile_options(android_compatibility_layer_bluetooth_dbus_test PRIVATE ${DBUS_TEST_CFLAGS_OTHER})
    target_link_libraries(android_compatibility_layer_bluetooth_dbus_test PRIVATE
        Qt5::Core Qt5::DBus ${DBUS_TEST_LDFLAGS})
endif()

add_executable(android_compatibility_layer_preview_text_test EXCLUDE_FROM_ALL
    aurora/tests/preview_text_test.cpp
    aurora/preview_text.cpp
    aurora/preview_image.cpp
    ${CORE_SOURCES}
    ${TEST_SOURCES}
    ${ZLIB_SOURCES}
)
target_include_directories(android_compatibility_layer_preview_text_test PRIVATE core/third_party/zlib)
target_compile_definitions(android_compatibility_layer_preview_text_test PRIVATE _GNU_SOURCE)
target_link_libraries(android_compatibility_layer_preview_text_test PRIVATE Qt5::Gui m pthread dl)

add_executable(android_compatibility_layer_preview_image_test EXCLUDE_FROM_ALL
    aurora/tests/preview_image_test.cpp
    aurora/preview_image.cpp
)
target_link_libraries(android_compatibility_layer_preview_image_test PRIVATE Qt5::Gui)

add_executable(android_compatibility_layer_preview_frame_test EXCLUDE_FROM_ALL aurora/tests/preview_frame_test.cpp)
target_link_libraries(android_compatibility_layer_preview_frame_test PRIVATE Qt5::Gui)

add_executable(android_compatibility_layer_preview_frame_export_test EXCLUDE_FROM_ALL
    aurora/tests/preview_frame_export_test.cpp
    aurora/preview_frame_export.cpp
)
target_link_libraries(android_compatibility_layer_preview_frame_export_test PRIVATE Qt5::Gui)

add_executable(android_compatibility_layer_preview_keyboard_test EXCLUDE_FROM_ALL
    aurora/tests/preview_keyboard_test.cpp
    aurora/preview_keyboard.cpp
    aurora/preview_keyboard.h
)
target_link_libraries(android_compatibility_layer_preview_keyboard_test PRIVATE Qt5::Quick pthread)

add_executable(android_compatibility_layer_audio_queue_test EXCLUDE_FROM_ALL aurora/tests/audio_queue_test.cpp)
target_link_libraries(android_compatibility_layer_audio_queue_test PRIVATE pthread)

add_executable(android_compatibility_layer_camera_frame_test EXCLUDE_FROM_ALL aurora/tests/camera_frame_test.cpp)
add_executable(android_compatibility_layer_camera_qt_frame_test EXCLUDE_FROM_ALL aurora/tests/camera_qt_frame_test.cpp)
target_link_libraries(android_compatibility_layer_camera_qt_frame_test PRIVATE Qt5::Multimedia)

add_executable(android_compatibility_layer_preview_viewport_test EXCLUDE_FROM_ALL aurora/tests/preview_viewport_test.cpp)
target_link_libraries(android_compatibility_layer_preview_viewport_test PRIVATE Qt5::Core)

add_executable(android_compatibility_layer_runtime_settings_test EXCLUDE_FROM_ALL aurora/tests/runtime_settings_test.cpp)
add_executable(android_compatibility_layer_translation_test EXCLUDE_FROM_ALL aurora/tests/translation_test.cpp)
target_link_libraries(android_compatibility_layer_translation_test PRIVATE Qt5::Core)
add_dependencies(android_compatibility_layer_translation_test android_compatibility_layer_translations)
target_link_libraries(android_compatibility_layer_runtime_settings_test PRIVATE Qt5::Core)

add_executable(android_compatibility_layer_shared_folders_test EXCLUDE_FROM_ALL
    aurora/tests/shared_folders_test.cpp aurora/shared_folders.cpp)
target_link_libraries(android_compatibility_layer_shared_folders_test PRIVATE Qt5::Core)

add_executable(android_compatibility_layer_app_cache_clear_test EXCLUDE_FROM_ALL
    aurora/tests/app_cache_clear_test.cpp aurora/app_cache_clear.cpp)
target_link_libraries(android_compatibility_layer_app_cache_clear_test PRIVATE Qt5::Core)

add_executable(android_compatibility_layer_google_services_runtime_test EXCLUDE_FROM_ALL aurora/tests/google_services_runtime_test.cpp)
add_executable(android_compatibility_layer_manager_lifecycle_test EXCLUDE_FROM_ALL
    aurora/tests/manager_lifecycle_test.cpp
    aurora/manager_lifecycle.cpp aurora/manager_lifecycle.h
    aurora/shared_microg_supervisor.cpp aurora/shared_microg_supervisor.h
    aurora/single_instance_lock.cpp aurora/single_instance_lock.h
    aurora/external_apk_receiver.cpp aurora/external_apk_receiver.h
    aurora/external_apk_path.cpp aurora/shared_microg_ipc.cpp)
target_include_directories(android_compatibility_layer_manager_lifecycle_test PRIVATE ${RUNTIME_MANAGER_INCLUDE_DIRS})
target_link_libraries(android_compatibility_layer_manager_lifecycle_test PRIVATE Qt5::Core Qt5::DBus ${RUNTIME_MANAGER_LDFLAGS})
add_executable(android_compatibility_layer_shared_microg_supervisor_test EXCLUDE_FROM_ALL
    aurora/tests/shared_microg_supervisor_test.cpp
    aurora/shared_microg_supervisor.cpp aurora/shared_microg_supervisor.h)
target_link_libraries(android_compatibility_layer_shared_microg_supervisor_test PRIVATE Qt5::Core)
add_executable(android_compatibility_layer_microg_watchdog_test EXCLUDE_FROM_ALL
    aurora/tests/microg_watchdog_test.cpp aurora/microg_watchdog.cpp aurora/shared_microg_ipc.cpp)
target_link_libraries(android_compatibility_layer_microg_watchdog_test PRIVATE Qt5::Core Qt5::DBus)
add_executable(android_compatibility_layer_microg_watchdog_schedule_test EXCLUDE_FROM_ALL
    aurora/tests/microg_watchdog_schedule_test.cpp)
target_link_libraries(android_compatibility_layer_microg_watchdog_schedule_test PRIVATE Qt5::Core)

add_executable(android_compatibility_layer_shared_microg_startup_client EXCLUDE_FROM_ALL
    tools/shared-microg-startup-client.cpp aurora/shared_microg_ipc.cpp
    ${CORE_SOURCES} ${TEST_SOURCES} ${ZLIB_SOURCES})
target_include_directories(android_compatibility_layer_shared_microg_startup_client PRIVATE
    core/third_party/zlib aurora/sqlite)
target_compile_definitions(android_compatibility_layer_shared_microg_startup_client PRIVATE
    _GNU_SOURCE MG_HAVE_SQLITE=1)
target_link_libraries(android_compatibility_layer_shared_microg_startup_client PRIVATE Qt5::Core Qt5::DBus m pthread dl)
    target_compile_definitions(android_compatibility_layer_manager_lifecycle_test PRIVATE ACL_STANDALONE_MICROG_DIAGNOSTIC)
    add_executable(android_compatibility_layer_standalone_microg_diagnostic_page_test EXCLUDE_FROM_ALL
        aurora/tests/standalone_microg_diagnostic_page_test.cpp)
    target_link_libraries(android_compatibility_layer_standalone_microg_diagnostic_page_test
        PRIVATE Qt5::Quick Qt5::DBus)
target_link_libraries(android_compatibility_layer_google_services_runtime_test PRIVATE Qt5::Core)

add_executable(android_compatibility_layer_official_store_release_test EXCLUDE_FROM_ALL
    aurora/tests/official_store_release_test.cpp aurora/official_store_release.cpp)
target_link_libraries(android_compatibility_layer_official_store_release_test PRIVATE Qt5::Core)

add_executable(android_compatibility_layer_official_store_download_test EXCLUDE_FROM_ALL
    aurora/tests/official_store_download_test.cpp aurora/official_store_download.cpp
    aurora/official_store_download.h aurora/official_store_release.cpp)
target_link_libraries(android_compatibility_layer_official_store_download_test PRIVATE Qt5::Network)

add_executable(android_compatibility_layer_manager_update_test EXCLUDE_FROM_ALL
    aurora/tests/manager_update_test.cpp aurora/manager_update.cpp aurora/manager_update.h)
target_link_libraries(android_compatibility_layer_manager_update_test PRIVATE Qt5::Network)

add_executable(android_compatibility_layer_download_notification_test EXCLUDE_FROM_ALL
    aurora/tests/download_notification_test.cpp)
target_link_libraries(android_compatibility_layer_download_notification_test PRIVATE Qt5::Quick Qt5::DBus)

add_executable(android_compatibility_layer_microg_install_test EXCLUDE_FROM_ALL
    aurora/tests/microg_install_test.cpp aurora/microg_install.cpp aurora/microg_install.h
    aurora/external_apk_path.cpp)
target_link_libraries(android_compatibility_layer_microg_install_test PRIVATE Qt5::Network)

add_executable(android_compatibility_layer_microg_preview_paths_test EXCLUDE_FROM_ALL
    aurora/tests/microg_preview_paths_test.cpp aurora/microg_preview_paths.h)
target_link_libraries(android_compatibility_layer_microg_preview_paths_test PRIVATE Qt5::Core)

add_executable(android_compatibility_layer_log_channel_test EXCLUDE_FROM_ALL
    aurora/tests/log_channel_test.cpp aurora/log_channel.cpp aurora/log_channel.h)
target_link_libraries(android_compatibility_layer_log_channel_test PRIVATE Qt5::Core)

add_executable(android_compatibility_layer_error_log_test EXCLUDE_FROM_ALL
    aurora/tests/error_log_test.cpp aurora/error_log.cpp aurora/error_log.h
    aurora/memory_log_bus.cpp aurora/memory_log_bus.h
    aurora/diagnostic_log.cpp aurora/diagnostic_log.h)
target_compile_definitions(android_compatibility_layer_error_log_test PRIVATE _GNU_SOURCE)
target_link_libraries(android_compatibility_layer_error_log_test PRIVATE Qt5::Core Qt5::DBus pthread)

add_executable(android_compatibility_layer_memory_log_bus_test EXCLUDE_FROM_ALL
    aurora/tests/memory_log_bus_test.cpp aurora/memory_log_bus.cpp aurora/memory_log_bus.h)
target_link_libraries(android_compatibility_layer_memory_log_bus_test PRIVATE Qt5::Core Qt5::DBus)

add_executable(android_compatibility_layer_preview_activation_channel_test EXCLUDE_FROM_ALL
    aurora/tests/preview_activation_channel_test.cpp
    aurora/preview_activation_channel.cpp aurora/preview_activation_channel.h)
target_link_libraries(android_compatibility_layer_preview_activation_channel_test PRIVATE Qt5::Gui Qt5::Network)

add_executable(android_compatibility_layer_audio_output_test EXCLUDE_FROM_ALL
    aurora/tests/audio_output_test.cpp
    aurora/audio_output.cpp
)
target_link_libraries(android_compatibility_layer_audio_output_test PRIVATE Qt5::Multimedia pthread)

set(ANDROID_COMPATIBILITY_LAYER_FFMPEG7_INCLUDE_DIR "" CACHE PATH
    "Directory containing matching FFmpeg 7 libavcodec and libavutil headers")
add_executable(android_compatibility_layer_codec_media_test EXCLUDE_FROM_ALL
    aurora/tests/codec_audio_test.cpp
    aurora/codec_audio.cpp
    aurora/codec_media_host.cpp
    aurora/audio_mp3_codec.cpp
)
if(ANDROID_COMPATIBILITY_LAYER_FFMPEG7_INCLUDE_DIR)
    target_include_directories(android_compatibility_layer_codec_media_test PRIVATE
        "${ANDROID_COMPATIBILITY_LAYER_FFMPEG7_INCLUDE_DIR}")
endif()
target_link_libraries(android_compatibility_layer_codec_media_test PRIVATE Qt5::Core)

add_executable(android_compatibility_layer_audio_resampling_test EXCLUDE_FROM_ALL aurora/tests/audio_resampling_test.c)
target_compile_options(android_compatibility_layer_audio_resampling_test PRIVATE -ffunction-sections -fdata-sections)
target_link_libraries(android_compatibility_layer_audio_resampling_test PRIVATE m -Wl,--gc-sections)

add_executable(android_compatibility_layer_audio_mp3_codec_test EXCLUDE_FROM_ALL
    aurora/tests/audio_mp3_codec_test.cpp
    aurora/audio_mp3_codec.cpp
)
target_link_libraries(android_compatibility_layer_audio_mp3_codec_test PRIVATE pthread)

add_executable(android_compatibility_layer_media_mp3_host_test EXCLUDE_FROM_ALL
    aurora/tests/media_mp3_host_test.cpp
    aurora/media_mp3_host.cpp
)
target_link_libraries(android_compatibility_layer_media_mp3_host_test PRIVATE Qt5::Multimedia)

add_executable(android_compatibility_layer_audio_preview_codec_test EXCLUDE_FROM_ALL
    aurora/tests/audio_preview_codec_test.cpp
    aurora/preview.cpp
    aurora/preview.h
    aurora/preview_frame_export.cpp aurora/preview_frame_export.h
    aurora/notification_host.cpp aurora/notification_host.h
    aurora/error_log.cpp aurora/error_log.h
    aurora/memory_log_bus.cpp aurora/memory_log_bus.h
    aurora/shared_folders.cpp
    aurora/shared_folders.h
    aurora/audio_mp3_codec.cpp
    aurora/media_mp3_host.cpp
    aurora/log_channel.cpp
    aurora/log_channel.h
)
target_link_libraries(android_compatibility_layer_audio_preview_codec_test PRIVATE Qt5::Quick Qt5::Multimedia Qt5::Network Qt5::DBus pthread)

add_executable(android_compatibility_layer_notification_host_test EXCLUDE_FROM_ALL
    aurora/tests/notification_host_test.cpp
    aurora/notification_host.cpp aurora/notification_host.h)
target_link_libraries(android_compatibility_layer_notification_host_test PRIVATE Qt5::Core Qt5::DBus)

set(EMBEDDED_PREVIEW_TEST_SOURCES
    aurora/tests/embedded_preview_test.cpp
    aurora/embedded_preview.cpp aurora/embedded_preview.h
    aurora/preview.cpp aurora/preview.h
    aurora/notification_host.cpp aurora/notification_host.h
    aurora/preview_keyboard.cpp aurora/preview_keyboard.h
    aurora/picker_host.cpp aurora/picker_host.h
    aurora/error_log.cpp aurora/error_log.h
    aurora/memory_log_bus.cpp aurora/memory_log_bus.h
    aurora/shared_folders.cpp aurora/shared_folders.h
    aurora/preview_image.cpp aurora/preview_frame_export.cpp aurora/preview_frame_export.h
    aurora/audio_mp3_codec.cpp
    aurora/media_mp3_host.cpp aurora/log_channel.cpp aurora/log_channel.h
    aurora/store_preview_supervisor.cpp aurora/store_preview_supervisor.h
    core/src/drivers/mg_store_restart.c)
add_executable(android_compatibility_layer_embedded_preview_test EXCLUDE_FROM_ALL
    ${EMBEDDED_PREVIEW_TEST_SOURCES})
target_compile_definitions(android_compatibility_layer_embedded_preview_test PRIVATE _GNU_SOURCE)
target_link_libraries(android_compatibility_layer_embedded_preview_test PRIVATE Qt5::Quick Qt5::Multimedia Qt5::Network Qt5::DBus pthread)

    add_executable(android_compatibility_layer_embedded_preview_gpu_test EXCLUDE_FROM_ALL
        ${EMBEDDED_PREVIEW_TEST_SOURCES}
        aurora/embedded_gpu_transport.cpp aurora/embedded_gpu_transport.h
        aurora/preview_gpu.cpp aurora/preview_gpu.h
        aurora/preview_gpu_canvas.cpp aurora/preview_gpu_canvas.h
        aurora/egl_frame_transport.c)
    target_compile_definitions(android_compatibility_layer_embedded_preview_gpu_test PRIVATE
        _GNU_SOURCE MG_EXPERIMENTAL_GPU_COMPOSITION MG_EXPERIMENTAL_GPU_CANVAS)
    target_include_directories(android_compatibility_layer_embedded_preview_gpu_test PRIVATE ${EGL_INCLUDE_DIRS} ${GLES2_INCLUDE_DIRS})
    target_link_libraries(android_compatibility_layer_embedded_preview_gpu_test PRIVATE
        Qt5::Quick Qt5::Multimedia Qt5::Network Qt5::DBus ${EGL_LDFLAGS} ${GLES2_LDFLAGS} pthread)

add_executable(android_compatibility_layer_embedded_preview_silica_test EXCLUDE_FROM_ALL
    aurora/tests/embedded_preview_silica_test.cpp
    aurora/tests/embedded_preview_silica_test.qml)
target_link_libraries(android_compatibility_layer_embedded_preview_silica_test PRIVATE Qt5::Quick)

add_executable(android_compatibility_layer_preview_navigation_silica_test EXCLUDE_FROM_ALL
    aurora/tests/preview_navigation_silica_test.cpp)
target_link_libraries(android_compatibility_layer_preview_navigation_silica_test PRIVATE Qt5::Quick)

add_executable(android_compatibility_layer_audio_mixer_path_test EXCLUDE_FROM_ALL
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
target_compile_definitions(android_compatibility_layer_audio_mixer_path_test PRIVATE _GNU_SOURCE)
target_compile_options(android_compatibility_layer_audio_mixer_path_test PRIVATE -ffunction-sections -fdata-sections)
target_link_libraries(android_compatibility_layer_audio_mixer_path_test PRIVATE Qt5::Multimedia pthread dl m -Wl,--gc-sections)

add_executable(android_compatibility_layer_apk_store_test EXCLUDE_FROM_ALL
    aurora/tests/apk_store_test.cpp
    aurora/apk_store.cpp
    ${CORE_SOURCES}
    ${TEST_SOURCES}
    ${ZLIB_SOURCES}
)
target_include_directories(android_compatibility_layer_apk_store_test PRIVATE core/third_party/zlib)
target_compile_definitions(android_compatibility_layer_apk_store_test PRIVATE _GNU_SOURCE)
target_link_libraries(android_compatibility_layer_apk_store_test PRIVATE Qt5::Core m pthread dl)

add_executable(android_compatibility_layer_apk_store_library_test EXCLUDE_FROM_ALL
    aurora/tests/apk_store_library_test.cpp
    aurora/apk_store.cpp
)
target_link_libraries(android_compatibility_layer_apk_store_library_test PRIVATE Qt5::Core)

add_executable(android_compatibility_layer_install_request_candidate_test EXCLUDE_FROM_ALL
    aurora/tests/install_request_candidate_test.cpp
    aurora/install_request_candidate.cpp aurora/install_request_prepare.cpp aurora/install_request_prepare.h
    aurora/install_request_snapshot.cpp)
target_link_libraries(android_compatibility_layer_install_request_candidate_test PRIVATE Qt5::Core)

add_executable(android_compatibility_layer_install_request_channel_test EXCLUDE_FROM_ALL
    aurora/tests/install_request_channel_test.cpp aurora/install_request_channel.cpp aurora/install_request_channel.h)
target_link_libraries(android_compatibility_layer_install_request_channel_test PRIVATE Qt5::Network)

add_executable(android_compatibility_layer_install_request_host_test EXCLUDE_FROM_ALL
    aurora/tests/install_request_host_test.cpp aurora/install_request_channel.cpp aurora/install_request_channel.h
    aurora/install_request_host.cpp aurora/install_request_host.h)
target_link_libraries(android_compatibility_layer_install_request_host_test PRIVATE Qt5::Network)

add_executable(android_compatibility_layer_standalone_apk_export_test EXCLUDE_FROM_ALL
    aurora/tests/standalone_apk_export_test.cpp
    aurora/library_app_index.cpp aurora/library_app_index.h
    aurora/standalone_app_mapping.cpp aurora/standalone_app_mapping.h
    aurora/standalone_apk_export.cpp aurora/standalone_apk_export.h
    aurora/external_apk_install.cpp aurora/external_apk_install.h aurora/external_apk_path.cpp
    aurora/install_request_candidate.cpp aurora/install_request_prepare.cpp aurora/install_request_prepare.h
    aurora/install_request_snapshot.cpp aurora/install_request_commit.cpp)
target_link_libraries(android_compatibility_layer_standalone_apk_export_test PRIVATE Qt5::Gui Qt5::DBus)

add_executable(android_compatibility_layer_preview_sessions_test EXCLUDE_FROM_ALL
    aurora/tests/preview_sessions_test.cpp
    aurora/preview_sessions.cpp aurora/preview_sessions.h
    aurora/diagnostic_log.cpp aurora/diagnostic_log.h
    aurora/error_log.cpp aurora/error_log.h
    aurora/memory_log_bus.cpp aurora/memory_log_bus.h
    aurora/preview_activation_channel.cpp aurora/preview_activation_channel.h
    aurora/log_channel.cpp aurora/log_channel.h
    aurora/install_request_channel.cpp aurora/install_request_channel.h
    aurora/install_request_host.cpp aurora/install_request_host.h
    aurora/install_request_candidate.cpp aurora/install_request_prepare.cpp aurora/install_request_prepare.h
    aurora/install_request_snapshot.cpp aurora/install_request_commit.cpp
    aurora/install_request_coordinator.cpp aurora/install_request_coordinator.h)
target_compile_definitions(android_compatibility_layer_preview_sessions_test PRIVATE _GNU_SOURCE)
target_link_libraries(android_compatibility_layer_preview_sessions_test PRIVATE Qt5::Network Qt5::Gui Qt5::DBus)

if(NOT CMAKE_CROSSCOMPILING)
    add_executable(android_compatibility_layer_probe_multitask_test EXCLUDE_FROM_ALL
        aurora/tests/probe_multitask_test.cpp aurora/probe.h
        aurora/manager_update.cpp aurora/manager_update.h
        aurora/official_store_download.cpp aurora/official_store_download.h aurora/official_store_release.cpp
        aurora/apk_store.cpp aurora/external_apk_install.cpp aurora/external_apk_install.h
        aurora/apk_bundle_install.cpp aurora/apk_bundle_install.h
        aurora/microg_install.cpp aurora/microg_install.h
        aurora/shared_folders.cpp aurora/shared_folders.h
        aurora/external_apk_path.cpp aurora/preview_sessions.cpp aurora/preview_sessions.h
        aurora/diagnostic_log.cpp aurora/diagnostic_log.h
        aurora/error_log.cpp aurora/error_log.h
        aurora/memory_log_bus.cpp aurora/memory_log_bus.h
        aurora/preview_activation_channel.cpp aurora/preview_activation_channel.h
        aurora/log_channel.cpp aurora/log_channel.h
        aurora/install_request_channel.cpp aurora/install_request_channel.h
        aurora/install_request_host.cpp aurora/install_request_host.h
        aurora/install_request_candidate.cpp aurora/install_request_prepare.cpp aurora/install_request_prepare.h
        aurora/install_request_snapshot.cpp aurora/install_request_commit.cpp
        aurora/install_request_coordinator.cpp aurora/install_request_coordinator.h
        aurora/rpm_writer.cpp aurora/rpm_signers.cpp aurora/rpm_icon.cpp)
    target_compile_definitions(android_compatibility_layer_probe_multitask_test PRIVATE _GNU_SOURCE)
    target_link_libraries(android_compatibility_layer_probe_multitask_test PRIVATE Qt5::Quick Qt5::Network Qt5::DBus ${CMAKE_DL_LIBS})
endif()

add_executable(android_compatibility_layer_install_request_commit_test EXCLUDE_FROM_ALL
    aurora/tests/install_request_commit_test.cpp aurora/install_request_commit.cpp)
target_compile_definitions(android_compatibility_layer_install_request_commit_test PRIVATE _GNU_SOURCE)
target_link_libraries(android_compatibility_layer_install_request_commit_test PRIVATE Qt5::Core)
add_executable(android_compatibility_layer_external_apk_install_test EXCLUDE_FROM_ALL
    aurora/tests/external_apk_install_test.cpp aurora/external_apk_install.cpp aurora/external_apk_install.h
    aurora/external_apk_path.cpp aurora/install_request_candidate.cpp
    aurora/install_request_prepare.cpp aurora/install_request_prepare.h
    aurora/install_request_snapshot.cpp aurora/install_request_commit.cpp)
target_compile_definitions(android_compatibility_layer_external_apk_install_test PRIVATE _GNU_SOURCE)
target_link_libraries(android_compatibility_layer_external_apk_install_test PRIVATE Qt5::Core)

add_executable(android_compatibility_layer_install_request_real_test EXCLUDE_FROM_ALL
    aurora/tests/install_request_real_test.cpp aurora/install_request_candidate.cpp
    aurora/install_request_snapshot.cpp aurora/apk_store.cpp
    ${CORE_SOURCES} ${TEST_SOURCES} ${ZLIB_SOURCES})
target_include_directories(android_compatibility_layer_install_request_real_test PRIVATE core/third_party/zlib)
target_compile_definitions(android_compatibility_layer_install_request_real_test PRIVATE _GNU_SOURCE)
target_link_libraries(android_compatibility_layer_install_request_real_test PRIVATE Qt5::Core m pthread dl)

add_executable(android_compatibility_layer_install_request_integration_test EXCLUDE_FROM_ALL
    aurora/tests/install_request_integration_test.cpp
    aurora/install_request_channel.cpp aurora/install_request_channel.h
    aurora/install_request_host.cpp aurora/install_request_host.h
    aurora/install_request_candidate.cpp aurora/install_request_prepare.cpp aurora/install_request_prepare.h
    aurora/install_request_snapshot.cpp aurora/install_request_commit.cpp
    aurora/install_request_coordinator.cpp aurora/install_request_coordinator.h
    ${CORE_SOURCES} ${TEST_SOURCES} ${ZLIB_SOURCES})
target_include_directories(android_compatibility_layer_install_request_integration_test PRIVATE core/third_party/zlib)
target_compile_definitions(android_compatibility_layer_install_request_integration_test PRIVATE _GNU_SOURCE)
target_link_libraries(android_compatibility_layer_install_request_integration_test PRIVATE Qt5::Network m pthread dl)

foreach(target android_compatibility_layer_preview_text_test android_compatibility_layer_apk_store_test android_compatibility_layer_install_request_real_test android_compatibility_layer_install_request_integration_test)
    target_include_directories(${target} PRIVATE "${ANDROID_COMPATIBILITY_LAYER_SQLITE_DIR}")
    target_compile_definitions(${target} PRIVATE MG_HAVE_SQLITE=1)
endforeach()
endif()

target_include_directories(acl_runtime_objects PRIVATE "${ANDROID_COMPATIBILITY_LAYER_SQLITE_DIR}")
target_compile_definitions(acl_runtime_objects PRIVATE MG_HAVE_SQLITE=1)

install(TARGETS ${APP_ID} RUNTIME DESTINATION bin)
# This ELF is read only to build standalone RPMs; workers self-spawn within
# Sailjail's private-bin instead of executing another binary.
install(FILES "$<TARGET_FILE:acl-runtime>" DESTINATION share/${APP_ID}
    PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
set(ANDROID_ROOT "" CACHE PATH "Override the ABI-matched Android runtime to install")
if(ANDROID_ROOT STREQUAL "")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(armv7.*|arm)$")
        set(ANDROID_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/build/android9/root-armeabi-v7a")
    else()
        set(ANDROID_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/aurora/android-runtime")
    endif()
endif()
message(STATUS "Android runtime: ${ANDROID_ROOT}")
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
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|armv7.*|arm)$")
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
        set(ANDROID_RUNTIME_ARCH aarch64)
    else()
        set(ANDROID_RUNTIME_ARCH armv7hl)
    endif()
    configure_file(tools/aurora/check_runtime.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/check_android_runtime.cmake" @ONLY)
    include("${CMAKE_CURRENT_BINARY_DIR}/check_android_runtime.cmake")
    install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/check_android_runtime.cmake")
endif()
set(SHARED_ANDROID_RUNTIME share/common/ru.androidcompatibilitylayer/appmanager/android-runtime)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|armv7.*|arm)$")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
        set(WEBVIEW_PROVIDER "${CMAKE_CURRENT_SOURCE_DIR}/build/webview/arm64-v8a/webview.apk")
        set(WEBVIEW_PROVIDER_SHA256 "492c3d054e4123ae0c1e6f3541e5ab515633e1fb4006e33bf343749ef1b66ae2")
        set(WEBVIEW_ABI "arm64-v8a")
    else()
        set(WEBVIEW_PROVIDER "${CMAKE_CURRENT_SOURCE_DIR}/build/webview/webview.apk")
        set(WEBVIEW_PROVIDER_SHA256 "19a2cdf16d451ee1a3dea8e5d5b369dc85e7bbc616701d6a41a51e851cfd37c4")
        set(WEBVIEW_ABI "armeabi-v7a")
    endif()
    if(NOT EXISTS "${WEBVIEW_PROVIDER}")
        message(FATAL_ERROR
            "The required ${WEBVIEW_ABI} WebView provider is missing: ${WEBVIEW_PROVIDER}. "
            "Fetch it with tools/webview-fetch.sh --aosp-chromium-128 ${WEBVIEW_ABI}.")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${WEBVIEW_PROVIDER}")
    file(SHA256 "${WEBVIEW_PROVIDER}" local_webview_sha256)
    if(NOT local_webview_sha256 STREQUAL WEBVIEW_PROVIDER_SHA256)
        message(FATAL_ERROR "The ${WEBVIEW_ABI} WebView provider does not match the pinned AOSP Chromium 128 APK.")
    endif()
    set(WEBVIEW_SUPPORT_STUB
        "${CMAKE_CURRENT_BINARY_DIR}/webview/libwebviewchromium_plat_support.so")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/webview")
    file(WRITE "${WEBVIEW_SUPPORT_STUB}"
        "The platform support of the WebView: its natives are answered by the core (core/src/drivers/mg_java_webview.c).\n")
    install(CODE "
        file(SHA256 \"${WEBVIEW_PROVIDER}\" staged_webview_sha256)
        if(NOT staged_webview_sha256 STREQUAL \"${WEBVIEW_PROVIDER_SHA256}\")
            message(FATAL_ERROR \"The ${WEBVIEW_ABI} WebView provider changed after CMake configuration.\")
        endif()
    ")
    install(FILES "${WEBVIEW_PROVIDER}"
        DESTINATION "${SHARED_ANDROID_RUNTIME}/app/webview"
        PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
    install(FILES "${WEBVIEW_SUPPORT_STUB}"
        DESTINATION "${SHARED_ANDROID_RUNTIME}/lib"
        PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
endif()
install(DIRECTORY "${ANDROID_ROOT}/" DESTINATION "${SHARED_ANDROID_RUNTIME}"
    FILE_PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ
    PATTERN "fixtures" EXCLUDE
    PATTERN "platform-framework*.dex" EXCLUDE
    PATTERN "webview.apk" EXCLUDE
    PATTERN "libwebviewchromium_plat_support.so" EXCLUDE
    PATTERN ".standalone-abi" EXCLUDE)
install(FILES aurora/standalone-runtime-abi DESTINATION "${SHARED_ANDROID_RUNTIME}"
    RENAME .standalone-abi)
install(DIRECTORY qml DESTINATION share/${APP_ID})
install(FILES ${ANDROID_COMPATIBILITY_LAYER_TRANSLATIONS} DESTINATION share/${APP_ID}/qml)
install(FILES ${APP_ID}.desktop DESTINATION share/applications)
foreach(size 86 108 128 172)
    install(FILES aurora/icons/${size}.png DESTINATION share/icons/hicolor/${size}x${size}/apps
        RENAME ${APP_ID}.png)
endforeach()

add_custom_target(project_files SOURCES
    ${APP_ID}.desktop
    rpm/${APP_ID}.spec
    qml/AndroidCompatibilityLayer.qml
    qml/Preview.qml
    aurora/icons/86.png
    aurora/icons/108.png
    aurora/icons/128.png
    aurora/icons/172.png
)
file(WRITE "${CMAKE_BINARY_DIR}/QtCreatorDeployment.txt"
    "${CMAKE_INSTALL_PREFIX}\n${CMAKE_BINARY_DIR}/${APP_ID}:bin\n${CMAKE_BINARY_DIR}/${APP_ID}.runtime:share/${APP_ID}\n")
