cmake_minimum_required(VERSION 3.21)
project(PRBot VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Исходники и строки интерфейса хранятся в UTF-8 независимо от языка Windows.
if(MSVC)
    add_compile_options(/utf-8)
endif()

find_package(Qt6 6.2 REQUIRED COMPONENTS Core Gui Widgets Network)

add_library(prbot_core STATIC
    src/config.cpp
    src/config.h
    src/pr_bot.cpp
    src/pr_bot.h
    src/run_log.cpp
    src/run_log.h
    src/webdriver_client.cpp
    src/webdriver_client.h
    src/forum_page.cpp
    src/forum_page.h
    src/forum_links.cpp
    src/forum_links.h
)
target_link_libraries(prbot_core PUBLIC Qt6::Core Qt6::Network)

add_library(prbot_ui STATIC
    src/bot_window.cpp
    src/bot_window.h
    src/driver_updater.cpp
    src/driver_updater.h
    src/window_theme.cpp
    src/window_theme.h
    src/run_history_dialog.cpp
    src/run_history_dialog.h
)
target_link_libraries(prbot_ui PUBLIC prbot_core Qt6::Widgets)

qt_add_executable(prbot src/main.cpp)
target_link_libraries(prbot PRIVATE prbot_ui)

include(CTest)
if(BUILD_TESTING)
    add_executable(prbot_core_tests
        tests/core_smoke.cpp
    )
    target_link_libraries(prbot_core_tests PRIVATE prbot_core)
    add_test(NAME prbot_core_tests COMMAND prbot_core_tests)

    add_executable(prbot_log_tests
        tests/run_log_test.cpp
    )
    target_link_libraries(prbot_log_tests PRIVATE prbot_core)
    add_test(NAME prbot_log_tests COMMAND prbot_log_tests)

    add_executable(prbot_browser_tests
        tests/browser_workflow_test.cpp
    )
    target_link_libraries(prbot_browser_tests PRIVATE prbot_ui)
    add_executable(prbot_ui_tests tests/ui_layout_test.cpp)
    target_link_libraries(prbot_ui_tests PRIVATE prbot_ui)
    add_test(NAME prbot_ui_tests COMMAND prbot_ui_tests)
    set_tests_properties(prbot_ui_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    option(PRBOT_BROWSER_TESTS "Run browser scenarios against a local fixture server" OFF)
    set(PRBOT_TEST_CHROMEDRIVER "${CMAKE_CURRENT_SOURCE_DIR}/.env/bin/chromedriver"
        CACHE FILEPATH "ChromeDriver for local browser tests")
    if(PRBOT_BROWSER_TESTS)
        add_test(NAME prbot_browser_tests COMMAND prbot_browser_tests "${PRBOT_TEST_CHROMEDRIVER}")
        set_tests_properties(prbot_browser_tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120)
    endif()
endif()

if(WIN32)
    set_target_properties(prbot PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
