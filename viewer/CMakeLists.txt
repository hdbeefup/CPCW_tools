cmake_minimum_required(VERSION 3.15)
project(cpcw_viewer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(cpcw_viewer
    src/viewer.cpp
    src/srm_model.cpp
    src/dds.cpp
)

# d3d9.h / d3d9.lib ship in the Windows SDK — no legacy DirectX SDK needed.
target_link_libraries(cpcw_viewer PRIVATE d3d9 comdlg32)

if (MSVC)
    target_compile_options(cpcw_viewer PRIVATE /W3 /EHsc)
    target_compile_definitions(cpcw_viewer PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()
