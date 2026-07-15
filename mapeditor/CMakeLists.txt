cmake_minimum_required(VERSION 3.16)
project(cpcw_mapeditor CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# --- GLFW (windowing / input) ------------------------------------------------
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4)
FetchContent_MakeAvailable(glfw)

# --- Dear ImGui (docking branch) --------------------------------------------
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG docking)
FetchContent_MakeAvailable(imgui)

# --- nlohmann/json (scene bridge; header-only) ------------------------------
FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
FetchContent_MakeAvailable(json)

# --- zlib (PAK type-2 decompression) ----------------------------------------
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zlib
    GIT_REPOSITORY https://github.com/madler/zlib.git
    GIT_TAG v1.3.1)
FetchContent_MakeAvailable(zlib)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC glfw)

# --- editor ------------------------------------------------------------------
add_executable(cpcw_mapeditor src/main.cpp src/mapfile.cpp src/protodb.cpp
    src/pak.cpp src/vfs.cpp src/thumb.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../viewer/src/srm_model.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../viewer/src/dds.cpp)
target_include_directories(cpcw_mapeditor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../viewer/src
    ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
target_link_libraries(cpcw_mapeditor PRIVATE zlibstatic)
target_link_libraries(cpcw_mapeditor PRIVATE imgui glfw nlohmann_json::nlohmann_json)
if (WIN32)
    target_link_libraries(cpcw_mapeditor PRIVATE opengl32 comdlg32)
elseif (APPLE)
    target_link_libraries(cpcw_mapeditor PRIVATE "-framework OpenGL")
else()
    target_link_libraries(cpcw_mapeditor PRIVATE GL)
endif()
if (MSVC)
    target_compile_definitions(cpcw_mapeditor PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
