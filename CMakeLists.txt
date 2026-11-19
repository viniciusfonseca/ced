cmake_minimum_required(VERSION 3.20)
project(ced VERSION 0.2.0 LANGUAGES C)
if(WIN32)
    set(CED_EXTENSIONS_DEFAULT OFF)
else()
    set(CED_EXTENSIONS_DEFAULT ON)
endif()
option(CED_ENABLE_EXTENSIONS "Enable experimental local extensions" ${CED_EXTENSIONS_DEFAULT})
if(WIN32 AND CED_ENABLE_EXTENSIONS)
    message(FATAL_ERROR "The experimental extension host requires POSIX. Configure Windows with -DCED_ENABLE_EXTENSIONS=OFF.")
endif()

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
foreach(dependency_file IN ITEMS
        third_party/CUI/CMakeLists.txt
        third_party/cjson/cJSON.c
        third_party/libvterm/src/vterm.c)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${dependency_file}")
        message(FATAL_ERROR "Missing dependency submodules. Run:\n  git submodule update --init --recursive")
    endif()
endforeach()
set(CUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CED_CUI_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/CUI")
add_subdirectory(third_party/CUI EXCLUDE_FROM_ALL)
find_package(Threads REQUIRED)
find_package(Perl REQUIRED)

add_library(ced_json STATIC third_party/cjson/cJSON.c)
target_include_directories(ced_json PUBLIC third_party/cjson)
add_library(ced_vterm STATIC
    third_party/libvterm/src/encoding.c third_party/libvterm/src/keyboard.c
    third_party/libvterm/src/mouse.c third_party/libvterm/src/parser.c
    third_party/libvterm/src/pen.c third_party/libvterm/src/screen.c
    third_party/libvterm/src/state.c third_party/libvterm/src/unicode.c
    third_party/libvterm/src/vterm.c)
target_include_directories(ced_vterm PUBLIC third_party/libvterm/include)
# The upstream Git checkout contains encoding tables, not their generated C
# headers. Generate those in the build tree so the submodule stays clean.
set(CED_VTERM_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated/libvterm")
set(CED_VTERM_ENCODINGS)
foreach(encoding IN ITEMS DECdrawing uk)
    set(output "${CED_VTERM_GENERATED}/encoding/${encoding}.inc")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${CMAKE_COMMAND}"
            "-DPERL=${PERL_EXECUTABLE}"
            "-DGENERATOR=${CMAKE_CURRENT_SOURCE_DIR}/third_party/libvterm/tbl2inc_c.pl"
            "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/third_party/libvterm/src/encoding/${encoding}.tbl"
            "-DOUTPUT=${output}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateVTermEncoding.cmake"
        DEPENDS
            "third_party/libvterm/src/encoding/${encoding}.tbl"
            "third_party/libvterm/tbl2inc_c.pl"
            "cmake/GenerateVTermEncoding.cmake"
        VERBATIM)
    list(APPEND CED_VTERM_ENCODINGS "${output}")
endforeach()
add_custom_target(ced_vterm_encodings DEPENDS ${CED_VTERM_ENCODINGS})
add_dependencies(ced_vterm ced_vterm_encodings)
target_include_directories(ced_vterm PRIVATE "${CED_VTERM_GENERATED}")

add_library(ced_core src/workspace.c src/preferences.c src/lsp.c src/terminal.c src/process_memory.c src/documents.c src/commands.c)
if(WIN32)
    target_sources(ced_core PRIVATE src/windows_compat.c src/windows_process.c src/windows_files.c)
    target_compile_definitions(ced_core PUBLIC _WIN32_WINNT=0x0A00 WINVER=0x0A00)
    target_link_libraries(ced_core PUBLIC bcrypt psapi advapi32 winpthread)
else()
    target_sources(ced_core PRIVATE src/file_operations.c)
endif()
target_include_directories(ced_core PUBLIC src)
target_compile_definitions(ced_core PUBLIC _DARWIN_C_SOURCE _POSIX_C_SOURCE=200809L _XOPEN_SOURCE=700)
target_compile_options(ced_core PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(ced_core PUBLIC Threads::Threads ced_json ced_vterm)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(ced_core PUBLIC util m)
endif()

set(CED_LOGO_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/ced_logo_data.h")
add_custom_command(OUTPUT "${CED_LOGO_HEADER}"
    COMMAND "${CMAKE_COMMAND}"
        "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/assets/ced-256.png"
        "-DOUTPUT=${CED_LOGO_HEADER}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedLogo.cmake"
    DEPENDS assets/ced-256.png cmake/EmbedLogo.cmake
    VERBATIM)
add_executable(ced src/main.c src/terminal_view.c src/editor_assist.c src/editor_logo.c "${CED_LOGO_HEADER}")
target_sources(ced PRIVATE src/editor_bridge.c)
if(CED_ENABLE_EXTENSIONS)
    add_library(ced_extensions STATIC src/rpc_transport.c src/extensions/json.c
        src/extensions/manifest.c src/extensions/sha256.c src/extensions/catalog.c src/extensions/process.c
        src/extensions/manager.c src/extensions/broker.c)
    target_link_libraries(ced_extensions PUBLIC ced_core)
    target_compile_options(ced_extensions PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(ced PRIVATE ced_extensions)
    target_sources(ced PRIVATE src/extensions/extension_ui.c)
    target_compile_definitions(ced PRIVATE CED_ENABLE_EXTENSIONS=1)
    string(TOLOWER "${CMAKE_SYSTEM_NAME}" CED_EXTENSION_OS)
    set(CED_EXTENSION_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
    if(CED_EXTENSION_ARCH STREQUAL "aarch64")
        set(CED_EXTENSION_ARCH arm64)
    endif()
    set(CED_EXTENSION_PLATFORM "${CED_EXTENSION_OS}-${CED_EXTENSION_ARCH}")
    set(CED_EXAMPLE_PACKAGE "${CMAKE_BINARY_DIR}/examples/extensions/uppercase-selection")
    configure_file(examples/extensions/uppercase-selection/ced-extension.json
        "${CED_EXAMPLE_PACKAGE}/ced-extension.json" @ONLY)
    configure_file(third_party/cjson/LICENSE "${CED_EXAMPLE_PACKAGE}/licenses/cJSON.txt" COPYONLY)
    add_executable(ced_uppercase_extension examples/extensions/uppercase-selection/main.c)
    set_target_properties(ced_uppercase_extension PROPERTIES OUTPUT_NAME uppercase-selection
        RUNTIME_OUTPUT_DIRECTORY "${CED_EXAMPLE_PACKAGE}/bin")
    target_link_libraries(ced_uppercase_extension PRIVATE ced_extensions)
endif()
target_include_directories(ced PRIVATE "${CED_CUI_SOURCE_DIR}/src" "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_options(ced PRIVATE -Wall -Wextra -Wpedantic)
cui_app_target(ced)
if(APPLE)
    enable_language(OBJC)
    find_program(CED_CODESIGN_EXECUTABLE codesign REQUIRED)
    target_sources(ced PRIVATE src/macos.m)
    target_compile_options(ced PRIVATE $<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>)
    # The standalone CLI also needs a Dock icon, without external resources.
    target_link_options(ced PRIVATE
        "LINKER:-sectcreate,__DATA,__ced_icon,${CMAKE_CURRENT_SOURCE_DIR}/assets/ced-256.png")
    set_property(TARGET ced APPEND PROPERTY LINK_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/ced-256.png"
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/ced.icns"
        "${CMAKE_BINARY_DIR}/Info.plist")
    # Keep the CLI executable and also provide a Finder-launchable application.
    configure_file(packaging/Info.plist.in ${CMAKE_BINARY_DIR}/Info.plist @ONLY)
    add_custom_command(TARGET ced POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/CED.app/Contents/MacOS"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/CED.app/Contents/Resources"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:ced>" "${CMAKE_BINARY_DIR}/CED.app/Contents/MacOS/ced"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_BINARY_DIR}/Info.plist" "${CMAKE_BINARY_DIR}/CED.app/Contents/Info.plist"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/packaging/ced.icns" "${CMAKE_BINARY_DIR}/CED.app/Contents/Resources/ced.icns"
        COMMAND "${CED_CODESIGN_EXECUTABLE}" --force --sign - "${CMAKE_BINARY_DIR}/CED.app"
        VERBATIM)
endif()

add_library(ced_fonts STATIC src/editor_font.c)
target_include_directories(ced_fonts PRIVATE "${CED_CUI_SOURCE_DIR}/src")
target_link_libraries(ced_fonts PUBLIC ced_core cui::cui)
target_compile_options(ced_fonts PRIVATE -Wall -Wextra -Wpedantic)
if(WIN32)
    target_sources(ced_fonts PRIVATE src/font_locator_windows.c)
elseif(APPLE)
    target_sources(ced_fonts PRIVATE src/font_locator.m)
    target_compile_options(ced_fonts PRIVATE $<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>)
    target_link_libraries(ced_fonts PUBLIC "-framework CoreText")
else()
    target_sources(ced_fonts PRIVATE src/font_locator.c)
endif()
target_link_libraries(ced PRIVATE ced_fonts)

include(CTest)
if(BUILD_TESTING AND WIN32)
    add_executable(ced_windows_tests tests/windows_test.c)
    target_link_libraries(ced_windows_tests PRIVATE ced_core)
    target_compile_options(ced_windows_tests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    add_test(NAME windows_platform COMMAND ced_windows_tests)
    set_tests_properties(windows_platform PROPERTIES TIMEOUT 45)
endif()
if(BUILD_TESTING AND NOT WIN32)
    add_executable(ced_editor_font_tests tests/editor_font_test.c src/editor_assist.c)
    target_include_directories(ced_editor_font_tests PRIVATE "${CED_CUI_SOURCE_DIR}/src")
    target_link_libraries(ced_editor_font_tests PRIVATE ced_fonts)
    target_compile_options(ced_editor_font_tests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    add_test(NAME editor_font COMMAND ced_editor_font_tests
        "${CED_CUI_SOURCE_DIR}/assets/fonts/GeistMono-Regular.ttf"
        "${CED_CUI_SOURCE_DIR}/assets/fonts/Inter-Regular.ttf" "${CMAKE_BINARY_DIR}/editor-font.png")
    set_tests_properties(editor_font PROPERTIES TIMEOUT 20)
    add_executable(ced_tests tests/workspace_test.c)
    target_link_libraries(ced_tests PRIVATE ced_core)
    target_compile_options(ced_tests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    add_test(NAME workspace COMMAND ced_tests)
    add_executable(ced_lsp_tests tests/lsp_test.c)
    target_link_libraries(ced_lsp_tests PRIVATE ced_core)
    target_compile_options(ced_lsp_tests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    find_program(CED_CLANGD_EXECUTABLE clangd)
    if(CED_CLANGD_EXECUTABLE)
        add_test(NAME lsp COMMAND ced_lsp_tests ${CED_CLANGD_EXECUTABLE})
    else()
        add_test(NAME lsp COMMAND ced_lsp_tests)
    endif()
    set_tests_properties(lsp PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 45)
    add_executable(ced_terminal_tests tests/terminal_test.c)
    target_link_libraries(ced_terminal_tests PRIVATE ced_core)
    target_compile_options(ced_terminal_tests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    add_test(NAME terminal COMMAND ced_terminal_tests)
    set_tests_properties(terminal PROPERTIES TIMEOUT 20)
    add_executable(ced_editor_assist_tests tests/editor_assist_test.c src/editor_assist.c src/editor_bridge.c)
    target_include_directories(ced_editor_assist_tests PRIVATE "${CED_CUI_SOURCE_DIR}/src")
    target_link_libraries(ced_editor_assist_tests PRIVATE ced_core cui::cui)
    target_compile_options(ced_editor_assist_tests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    add_test(NAME editor_assist COMMAND ced_editor_assist_tests)
    if(CED_ENABLE_EXTENSIONS)
        foreach(suite IN ITEMS manifest protocol edit integration lifecycle broker example)
            add_executable(ced_extension_${suite}_tests tests/extension_${suite}_test.c)
            target_link_libraries(ced_extension_${suite}_tests PRIVATE ced_extensions)
            target_compile_options(ced_extension_${suite}_tests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
        endforeach()
        foreach(suite IN ITEMS edit integration)
            target_sources(ced_extension_${suite}_tests PRIVATE src/editor_bridge.c)
            target_include_directories(ced_extension_${suite}_tests PRIVATE "${CED_CUI_SOURCE_DIR}/src")
            target_link_libraries(ced_extension_${suite}_tests PRIVATE cui::cui)
        endforeach()
        add_test(NAME extension_manifest COMMAND ced_extension_manifest_tests "${CED_EXAMPLE_PACKAGE}")
        add_test(NAME extension_protocol COMMAND ced_extension_protocol_tests)
        add_test(NAME extension_broker COMMAND ced_extension_broker_tests)
        add_test(NAME extension_example COMMAND ced_extension_example_tests "${CED_EXAMPLE_PACKAGE}")
        add_test(NAME extension_edit COMMAND ced_extension_edit_tests)
        add_test(NAME extension_integration COMMAND ced_extension_integration_tests "${CED_EXAMPLE_PACKAGE}")
        add_executable(ced_extension_fixture tests/extension_fixture.c)
        target_link_libraries(ced_extension_fixture PRIVATE ced_extensions)
        add_test(NAME extension_lifecycle COMMAND ced_extension_lifecycle_tests "$<TARGET_FILE:ced_extension_fixture>")
        add_executable(ced_extension_ui_tests tests/extension_ui_test.c src/extensions/extension_ui.c)
        target_link_libraries(ced_extension_ui_tests PRIVATE ced_extensions cui::cui)
        target_compile_options(ced_extension_ui_tests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
        add_test(NAME extension_ui COMMAND ced_extension_ui_tests "${CED_EXAMPLE_PACKAGE}" "${CMAKE_BINARY_DIR}")
        set_tests_properties(extension_manifest extension_protocol extension_broker extension_example extension_edit extension_integration extension_lifecycle PROPERTIES TIMEOUT 25)
    endif()
endif()
