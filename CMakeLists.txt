cmake_minimum_required(VERSION 3.20)

project(frame-notify VERSION 0.1.4 LANGUAGES CXX)

set(FRAME_NOTIFY_VERSION "${PROJECT_VERSION}" CACHE STRING "The version the program reports (releases set it from the tag)")

include(CTest)
include(FetchContent)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(OPENVR_SDK_ROOT "" CACHE PATH "OpenVR SDK root (contains headers/openvr.h)")
set(OPENVR_INCLUDE_DIR "" CACHE PATH "Directory containing openvr.h")
set(OPENVR_LIBRARY "" CACHE FILEPATH "Path to the architecture-correct OpenVR library")
option(FRAME_NOTIFY_FETCH_OPENVR "Fetch and build the pinned OpenVR SDK when no SDK is supplied" ON)

function(frame_notify_validate_elf_arch library_path)
    if(NOT UNIX OR APPLE OR NOT EXISTS "${library_path}")
        return()
    endif()

    file(READ "${library_path}" elf_magic OFFSET 0 LIMIT 4 HEX)
    string(TOLOWER "${elf_magic}" elf_magic)
    if(NOT elf_magic STREQUAL "7f454c46")
        return()
    endif()

    file(READ "${library_path}" elf_machine OFFSET 18 LIMIT 2 HEX)
    string(TOLOWER "${elf_machine}" elf_machine)
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" target_processor)

    if(target_processor MATCHES "^(aarch64|arm64)$" AND NOT elf_machine STREQUAL "b700")
        message(FATAL_ERROR
            "OPENVR_LIBRARY is not an AArch64 ELF library (e_machine bytes: ${elf_machine}). "
            "Refusing to link a non-ARM64 OpenVR library into the Steam Frame build.")
    endif()
endfunction()

set(frame_notify_openvr_target "")
set(frame_notify_openvr_static OFF)

if(OPENVR_SDK_ROOT)
    if(NOT OPENVR_INCLUDE_DIR)
        unset(OPENVR_INCLUDE_DIR CACHE)
        find_path(OPENVR_INCLUDE_DIR
            NAMES openvr.h
            PATHS "${OPENVR_SDK_ROOT}/headers" "${OPENVR_SDK_ROOT}/include/openvr"
            NO_DEFAULT_PATH)
    endif()

    if(NOT OPENVR_LIBRARY)
        unset(OPENVR_LIBRARY CACHE)
        find_library(OPENVR_LIBRARY
            NAMES openvr_api
            PATHS "${OPENVR_SDK_ROOT}"
            PATH_SUFFIXES lib/linuxarm64 bin/linuxarm64 lib/linux64 bin/linux64 lib
            NO_DEFAULT_PATH)
    endif()
endif()

if(OPENVR_INCLUDE_DIR OR OPENVR_LIBRARY)
    if(NOT OPENVR_INCLUDE_DIR OR NOT EXISTS "${OPENVR_INCLUDE_DIR}/openvr.h")
        message(FATAL_ERROR "OPENVR_INCLUDE_DIR must point to a directory containing openvr.h")
    endif()
    if(NOT OPENVR_LIBRARY OR NOT EXISTS "${OPENVR_LIBRARY}")
        message(FATAL_ERROR "OPENVR_LIBRARY must point to an existing OpenVR library")
    endif()

    frame_notify_validate_elf_arch("${OPENVR_LIBRARY}")
    add_library(frame_notify_openvr UNKNOWN IMPORTED)
    set_target_properties(frame_notify_openvr PROPERTIES
        IMPORTED_LOCATION "${OPENVR_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${OPENVR_INCLUDE_DIR}")
    set(frame_notify_openvr_target frame_notify_openvr)
    message(STATUS "Using supplied OpenVR header: ${OPENVR_INCLUDE_DIR}/openvr.h")
    message(STATUS "Using supplied OpenVR library: ${OPENVR_LIBRARY}")
elseif(FRAME_NOTIFY_FETCH_OPENVR)
    # Pinned to Valve's current public release reviewed for this milestone.
    set(BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(BUILD_FRAMEWORK OFF CACHE BOOL "" FORCE)
    set(USE_LIBCXX OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(openvr
        GIT_REPOSITORY https://github.com/ValveSoftware/openvr.git
        GIT_TAG v2.15.6
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(openvr)

    if(TARGET openvr_api)
        set(frame_notify_openvr_target openvr_api)
    elseif(TARGET openvr_api64)
        set(frame_notify_openvr_target openvr_api64)
    else()
        message(FATAL_ERROR "The fetched OpenVR SDK did not define an expected library target")
    endif()
    set(frame_notify_openvr_static ON)
    message(STATUS "Using fetched OpenVR SDK v2.15.6 (${CMAKE_SYSTEM_PROCESSOR})")
else()
    message(FATAL_ERROR
        "OpenVR was not supplied and FRAME_NOTIFY_FETCH_OPENVR is OFF. Set OPENVR_SDK_ROOT, "
        "or set both OPENVR_INCLUDE_DIR and OPENVR_LIBRARY.")
endif()

add_library(frame_notify_ui STATIC
    src/ui/app_style.cpp
    src/ui/font.cpp
    src/ui/history_view.cpp
    src/ui/icon.cpp
    src/ui/phone_info.cpp
    src/ui/phone_view.cpp
    src/ui/raster.cpp
    src/ui/renderer.cpp
    src/ui/scroll_controller.cpp
    src/ui/scroll_texture.cpp
    src/ui/time_format.cpp
    src/ui/typography.cpp)
target_include_directories(frame_notify_ui PUBLIC src)
# The UI loads the bundled Inter fonts at runtime. They are copied next to the executables below;
# the source-tree location is a fallback for binaries run from another directory.
target_compile_definitions(frame_notify_ui PRIVATE
    "FRAME_NOTIFY_FONT_DIR=\"${CMAKE_SOURCE_DIR}/src/ui/fonts\"")
foreach(frame_notify_font_file Inter-Regular.ttf Inter-SemiBold.ttf OFL.txt)
    configure_file("${CMAKE_SOURCE_DIR}/src/ui/fonts/${frame_notify_font_file}"
                   "${CMAKE_BINARY_DIR}/fonts/${frame_notify_font_file}" COPYONLY)
endforeach()

add_library(frame_notify_core STATIC
    src/bluetooth/phone_link.cpp
    src/history/store.cpp
    src/ipc/json_line.cpp
    src/ipc/socket_server.cpp
    src/system/autostart.cpp
    src/system/process.cpp
    src/system/single_instance.cpp
    src/system/update_check.cpp
    src/system/update_install.cpp)
target_include_directories(frame_notify_core PUBLIC src)
# The Bluetooth helper is a Python script; a binary started from another directory still finds it
# here, in the source tree.
target_compile_definitions(frame_notify_core PRIVATE
    "FRAME_NOTIFY_SCRIPTS_DIR=\"${CMAKE_SOURCE_DIR}/scripts\"")

add_executable(frame-notify
    src/main.cpp
    src/openvr/attach_plan.cpp
    src/openvr/dashboard.cpp
    src/openvr/notifications.cpp
    src/openvr/runtime.cpp
    src/openvr/vr_session.cpp)
target_include_directories(frame-notify PRIVATE src)
target_compile_definitions(frame-notify PRIVATE "FRAME_NOTIFY_VERSION=\"${FRAME_NOTIFY_VERSION}\"")
target_link_libraries(frame-notify PRIVATE
    frame_notify_core
    frame_notify_ui
    ${frame_notify_openvr_target})
if(frame_notify_openvr_static)
    target_compile_definitions(frame-notify PRIVATE OPENVR_BUILD_STATIC)
endif()

if(MSVC)
    target_compile_definitions(frame-notify PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(frame_notify_core PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(frame-notify PRIVATE /W4 /permissive-)
    target_compile_options(frame_notify_core PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(frame_notify_ui PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(frame-notify PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(frame_notify_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(frame_notify_ui PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(frame-notify PROPERTIES
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN")

if(UNIX AND OPENVR_LIBRARY MATCHES "\\.so(\\..*)?$")
    add_custom_command(TARGET frame-notify POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${OPENVR_LIBRARY}" "$<TARGET_FILE_DIR:frame-notify>"
        COMMENT "Copying the supplied OpenVR shared library beside frame-notify")
endif()

if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME ancs-python
            COMMAND "${Python3_EXECUTABLE}" -m unittest discover
                -s "${CMAKE_SOURCE_DIR}/tests" -p "ancs_*_test.py")
    endif()

    add_executable(frame-notify-icon-test tests/icon_test.cpp)
    target_link_libraries(frame-notify-icon-test PRIVATE frame_notify_ui)
    add_test(NAME icon-generation COMMAND frame-notify-icon-test)

    add_executable(frame-notify-history-view-test tests/history_view_test.cpp)
    target_link_libraries(frame-notify-history-view-test PRIVATE frame_notify_ui)
    add_test(NAME history-view-rendering COMMAND frame-notify-history-view-test)

    add_executable(frame-notify-ui-test tests/ui_test.cpp)
    target_link_libraries(frame-notify-ui-test PRIVATE frame_notify_ui)
    add_test(NAME ui-primitives COMMAND frame-notify-ui-test)

    find_program(FRAME_NOTIFY_BASH bash)
    if(FRAME_NOTIFY_BASH)
        add_test(NAME install-scripts
            COMMAND "${FRAME_NOTIFY_BASH}" "${CMAKE_SOURCE_DIR}/tests/install_test.sh")
    endif()

    add_executable(frame-notify-scroll-texture-test tests/scroll_texture_test.cpp)
    target_link_libraries(frame-notify-scroll-texture-test PRIVATE frame_notify_ui)
    add_test(NAME scroll-texture COMMAND frame-notify-scroll-texture-test)

    add_executable(frame-notify-phone-view-test tests/phone_view_test.cpp)
    target_link_libraries(frame-notify-phone-view-test PRIVATE frame_notify_ui)
    add_test(NAME phone-screens COMMAND frame-notify-phone-view-test)

    add_executable(frame-notify-message-test tests/notification_message_test.cpp)
    target_link_libraries(frame-notify-message-test PRIVATE frame_notify_core)
    add_test(NAME notification-message-parsing COMMAND frame-notify-message-test)

    add_executable(frame-notify-attach-plan-test tests/attach_plan_test.cpp src/openvr/attach_plan.cpp)
    target_include_directories(frame-notify-attach-plan-test PRIVATE src)
    add_test(NAME steamvr-attach-plan COMMAND frame-notify-attach-plan-test)

    add_executable(frame-notify-update-check-test tests/update_check_test.cpp)
    target_link_libraries(frame-notify-update-check-test PRIVATE frame_notify_core)
    add_test(NAME update-check COMMAND frame-notify-update-check-test)

    add_executable(frame-notify-update-install-test tests/update_install_test.cpp)
    target_link_libraries(frame-notify-update-install-test PRIVATE frame_notify_core)
    add_test(NAME update-install COMMAND frame-notify-update-install-test)

    add_executable(frame-notify-system-test tests/system_test.cpp)
    target_link_libraries(frame-notify-system-test PRIVATE frame_notify_core)
    add_test(NAME system-autostart-and-lock COMMAND frame-notify-system-test)

    add_executable(frame-notify-phone-link-test tests/phone_link_test.cpp)
    target_link_libraries(frame-notify-phone-link-test PRIVATE frame_notify_core)
    add_test(NAME phone-link-supervision COMMAND frame-notify-phone-link-test)
endif()
