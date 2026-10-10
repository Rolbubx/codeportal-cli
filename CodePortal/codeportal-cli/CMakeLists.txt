cmake_minimum_required(VERSION 3.16)
project(CodePortal LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Interface : FTXUI (third_party/ftxui, version amalgamée) compilé par src/ftxui_impl.cpp.
add_executable(codeportal
    src/main.cpp
    src/ftxui_impl.cpp
    src/util.hpp
    src/tui.hpp
    src/safety.hpp
    src/json.hpp
    src/http.hpp
    src/state.hpp
    src/providers.hpp
    src/live.hpp
    src/markdown.hpp
    src/toolspec.hpp
    src/ui.hpp
    src/tools.hpp
    src/commands.hpp
    src/agent.hpp
)

if(WIN32)
    target_compile_options(codeportal PRIVATE -pthread)
    target_link_libraries(codeportal PRIVATE winhttp shell32)
    target_link_options(codeportal PRIVATE -pthread -static-libgcc -static-libstdc++)
endif()
