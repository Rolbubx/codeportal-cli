cmake_minimum_required(VERSION 3.16)
project(CodePortal LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(codeportal
    src/main.cpp
    src/util.hpp
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
    src/agenttools.hpp
    src/api.hpp
    src/update.hpp
    src/version.hpp
    src/web.hpp
    src/qr.hpp
    res/codeportal.rc
)

if(WIN32)
    target_compile_options(codeportal PRIVATE -pthread)
    target_link_libraries(codeportal PRIVATE winhttp shell32 ws2_32 bcrypt)
    target_link_options(codeportal PRIVATE -pthread -static)
endif()
