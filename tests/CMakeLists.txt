add_executable(tst_configmanager tst_configmanager.cpp)
target_link_libraries(tst_configmanager PRIVATE hyprfm_core Qt6::Test Qt6::Core Qt6::Gui)
add_test(NAME tst_configmanager COMMAND tst_configmanager)

add_executable(tst_themeloader tst_themeloader.cpp)
target_compile_definitions(tst_themeloader PRIVATE
    THEMES_DIR="${CMAKE_SOURCE_DIR}/themes"
)
target_link_libraries(tst_themeloader PRIVATE hyprfm_core Qt6::Test Qt6::Core Qt6::Gui)
add_test(NAME tst_themeloader COMMAND tst_themeloader)

add_executable(tst_tabmodel tst_tabmodel.cpp)
target_link_libraries(tst_tabmodel PRIVATE hyprfm_core Qt6::Test Qt6::Core)
add_test(NAME tst_tabmodel COMMAND tst_tabmodel)

add_executable(tst_clipboardmanager tst_clipboardmanager.cpp)
target_link_libraries(tst_clipboardmanager PRIVATE hyprfm_core Qt6::Test Qt6::Core Qt6::Gui)
add_test(NAME tst_clipboardmanager COMMAND tst_clipboardmanager)

add_executable(tst_bookmarkmodel tst_bookmarkmodel.cpp)
target_link_libraries(tst_bookmarkmodel PRIVATE hyprfm_core Qt6::Test Qt6::Core)
add_test(NAME tst_bookmarkmodel COMMAND tst_bookmarkmodel)

add_executable(tst_fileoperations tst_fileoperations.cpp)
target_link_libraries(tst_fileoperations PRIVATE hyprfm_core Qt6::Test Qt6::Core Qt6::Gui)
# ctest on Windows loses the QtTest output of a failed run, and the CI
# rerun can pass; keep the first run's log (uploaded with test-logs).
if(WIN32)
    add_test(NAME tst_fileoperations COMMAND tst_fileoperations
        -o ${CMAKE_BINARY_DIR}/tst_fileoperations.ctest.txt,txt -o -,txt)
else()
    add_test(NAME tst_fileoperations COMMAND tst_fileoperations)
endif()
# Shares gio trash state with tst_giotransferworker; serialize to avoid flakes.
set_tests_properties(tst_fileoperations PROPERTIES
    RUN_SERIAL TRUE
    TIMEOUT 120
)

add_executable(tst_undomanager tst_undomanager.cpp)
target_link_libraries(tst_undomanager PRIVATE hyprfm_core Qt6::Test Qt6::Core Qt6::Gui)
add_test(NAME tst_undomanager COMMAND tst_undomanager)

add_executable(tst_previewservice tst_previewservice.cpp)
target_link_libraries(tst_previewservice PRIVATE hyprfm_core Qt6::Test Qt6::Core Qt6::Gui)
add_test(NAME tst_previewservice COMMAND tst_previewservice)

add_executable(tst_rcloneservice tst_rcloneservice.cpp)
target_link_libraries(tst_rcloneservice PRIVATE hyprfm_core Qt6::Test Qt6::Core)
add_test(NAME tst_rcloneservice COMMAND tst_rcloneservice)

add_executable(tst_remoteaccessservice tst_remoteaccessservice.cpp)
target_link_libraries(tst_remoteaccessservice PRIVATE hyprfm_core Qt6::Test Qt6::Core)
add_test(NAME tst_remoteaccessservice COMMAND tst_remoteaccessservice)

add_executable(tst_diskusageservice tst_diskusageservice.cpp)
target_link_libraries(tst_diskusageservice PRIVATE hyprfm_core Qt6::Test Qt6::Core)
add_test(NAME tst_diskusageservice COMMAND tst_diskusageservice)

# --- New test targets ---

add_executable(tst_filesystemmodel tst_filesystemmodel.cpp)
target_link_libraries(tst_filesystemmodel PRIVATE hyprfm_core Qt6::Test Qt6::Core Qt6::Gui Qt6::Svg Qt6::Concurrent)
add_test(NAME tst_filesystemmodel COMMAND tst_filesystemmodel)

add_executable(tst_detailedview tst_detailedview.cpp viewharness.h)
target_compile_definitions(tst_detailedview PRIVATE
    TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    TEST_MODULE_DIR="${CMAKE_BINARY_DIR}/src"
)
target_link_libraries(tst_detailedview PRIVATE hyprfm_core Qt6::Test Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Concurrent)
add_dependencies(tst_detailedview hyprfm)
add_test(NAME tst_detailedview COMMAND tst_detailedview)

add_executable(tst_millerview tst_millerview.cpp viewharness.h)
target_compile_definitions(tst_millerview PRIVATE
    TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    TEST_MODULE_DIR="${CMAKE_BINARY_DIR}/src"
)
target_link_libraries(tst_millerview PRIVATE hyprfm_core Qt6::Test Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Concurrent)
add_dependencies(tst_millerview hyprfm)
add_test(NAME tst_millerview COMMAND tst_millerview)

add_executable(tst_viewcontextmenu tst_viewcontextmenu.cpp viewharness.h)
target_compile_definitions(tst_viewcontextmenu PRIVATE
    TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    TEST_MODULE_DIR="${CMAKE_BINARY_DIR}/src"
)
target_link_libraries(tst_viewcontextmenu PRIVATE hyprfm_core Qt6::Test Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Concurrent)
add_dependencies(tst_viewcontextmenu hyprfm)
add_test(NAME tst_viewcontextmenu COMMAND tst_viewcontextmenu)

add_executable(tst_toolbar tst_toolbar.cpp viewharness.h)
target_compile_definitions(tst_toolbar PRIVATE TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}" TEST_MODULE_DIR="${CMAKE_BINARY_DIR}/src")
target_link_libraries(tst_toolbar PRIVATE hyprfm_core Qt6::Test Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Concurrent)
add_dependencies(tst_toolbar hyprfm)
add_test(NAME tst_toolbar COMMAND tst_toolbar)

add_executable(tst_mainwindow tst_mainwindow.cpp)
target_compile_definitions(tst_mainwindow PRIVATE
    TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    TEST_MODULE_DIR="${CMAKE_BINARY_DIR}/src"
    HYPRFM_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    HYPRFM_DATA_DIR="${CMAKE_SOURCE_DIR}"
    HYPRFM_VERSION="test"
)
target_link_libraries(tst_mainwindow PRIVATE hyprfm_core Qt6::Test Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Svg Qt6::Network Qt6::Concurrent)
add_dependencies(tst_mainwindow hyprfm)
add_test(NAME tst_mainwindow COMMAND tst_mainwindow)

# The view harnesses assert pixel geometry of a 900x600 window; a tiling
# compositor would resize a real window, so keep them on the offscreen QPA.
set_tests_properties(tst_detailedview tst_millerview tst_viewcontextmenu tst_toolbar tst_mainwindow PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(tst_devicemodel tst_devicemodel.cpp)
target_link_libraries(tst_devicemodel PRIVATE hyprfm_core Qt6::Test Qt6::Core)
add_test(NAME tst_devicemodel COMMAND tst_devicemodel)

add_executable(tst_iconprovider tst_iconprovider.cpp)
target_link_libraries(tst_iconprovider PRIVATE hyprfm_core Qt6::Test Qt6::Core Qt6::Gui Qt6::Quick Qt6::Svg)
add_test(NAME tst_iconprovider COMMAND tst_iconprovider)

add_executable(tst_thumbnailprovider tst_thumbnailprovider.cpp)
target_link_libraries(tst_thumbnailprovider PRIVATE hyprfm_core Qt6::Test Qt6::Core Qt6::Gui Qt6::Quick Qt6::Svg)
add_test(NAME tst_thumbnailprovider COMMAND tst_thumbnailprovider)

add_executable(tst_pdfpreviewprovider tst_pdfpreviewprovider.cpp)
target_link_libraries(tst_pdfpreviewprovider PRIVATE hyprfm_core Qt6::Test Qt6::Core Qt6::Gui Qt6::Quick)
add_test(NAME tst_pdfpreviewprovider COMMAND tst_pdfpreviewprovider)

add_executable(tst_searchresultsmodel tst_searchresultsmodel.cpp)
target_link_libraries(tst_searchresultsmodel PRIVATE hyprfm_core Qt6::Test Qt6::Core Qt6::Gui)
add_test(NAME tst_searchresultsmodel COMMAND tst_searchresultsmodel)

add_executable(tst_searchproxymodel tst_searchproxymodel.cpp)
target_link_libraries(tst_searchproxymodel PRIVATE hyprfm_core Qt6::Test Qt6::Core Qt6::Gui Qt6::Svg Qt6::Concurrent)
add_test(NAME tst_searchproxymodel COMMAND tst_searchproxymodel)

add_executable(tst_searchservice tst_searchservice.cpp)
target_link_libraries(tst_searchservice PRIVATE hyprfm_core Qt6::Test Qt6::Core Qt6::Gui)
add_test(NAME tst_searchservice COMMAND tst_searchservice)

add_executable(tst_giotransferworker tst_giotransferworker.cpp)
target_link_libraries(tst_giotransferworker PRIVATE hyprfm_core Qt6::Test Qt6::Core)
add_test(NAME tst_giotransferworker COMMAND tst_giotransferworker)
# gio spawns child processes and shares trash/mount state with other tests;
# running serially prevents flaky timeouts under `ctest -j`.
set_tests_properties(tst_giotransferworker PROPERTIES
    RUN_SERIAL TRUE
    TIMEOUT 120
)

if(NOT WIN32)
# Multi-window / single-instance arbitration lives in main(), so it is covered
# by driving the real binary. Skips itself (exit 77) without a Wayland session.
add_test(NAME tst_instance_launch
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tst_instance_launch.sh $<TARGET_FILE:hyprfm>
)
set_tests_properties(tst_instance_launch PROPERTIES
    SKIP_RETURN_CODE 77
    RUN_SERIAL TRUE
    TIMEOUT 120
)
endif()

# --help / --version answer before any Qt GUI setup, so unlike the launch test
# these run anywhere, including headless CI.
add_test(NAME tst_cli_help COMMAND hyprfm --help)
set_tests_properties(tst_cli_help PROPERTIES
    PASS_REGULAR_EXPRESSION "-n, --new-window"
)
add_test(NAME tst_cli_version COMMAND hyprfm --version)
set_tests_properties(tst_cli_version PROPERTIES
    PASS_REGULAR_EXPRESSION "^hyprfm [0-9]+\\.[0-9]+\\.[0-9]+"
)

# XDG trash spec on disk; Windows has the Recycle Bin instead.
if(NOT WIN32)
add_executable(tst_xdgtrash tst_xdgtrash.cpp)
target_link_libraries(tst_xdgtrash PRIVATE hyprfm_core Qt6::Test Qt6::Core)
add_test(NAME tst_xdgtrash COMMAND tst_xdgtrash)
endif()

add_executable(tst_recentfilesmodel tst_recentfilesmodel.cpp)
target_link_libraries(tst_recentfilesmodel PRIVATE hyprfm_core Qt6::Test Qt6::Core Qt6::Concurrent)
add_test(NAME tst_recentfilesmodel COMMAND tst_recentfilesmodel)

# SFTP over libssh2 against a real OpenSSH server (Windows CI starts one and
# sets HYPRFM_SFTP_TEST_HOST); skips without it. Linux uses GVFS instead.
if(WIN32)
add_executable(tst_sftpsession tst_sftpsession.cpp)
target_link_libraries(tst_sftpsession PRIVATE hyprfm_core Qt6::Test Qt6::Core)
# The rerun meets what the first run left on the server; keep the first log.
add_test(NAME tst_sftpsession COMMAND tst_sftpsession
    -o ${CMAKE_BINARY_DIR}/tst_sftpsession.ctest.txt,txt -o -,txt)
set_tests_properties(tst_sftpsession PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
