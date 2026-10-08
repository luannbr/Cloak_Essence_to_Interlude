cmake_minimum_required(VERSION 3.16)
project(cloakhook LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "The Lineage II client is 32-bit. Configure with:  cmake -S . -B build -A Win32")
endif()

# static CRT so the proxy has no vcruntime dependency inside the game folder
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

include(FetchContent)
FetchContent_Declare(minhook
  GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
  GIT_TAG        v1.3.4)
FetchContent_MakeAvailable(minhook)

add_library(dsetup SHARED src/cloakhook.cpp)
set_target_properties(dsetup PROPERTIES PREFIX "" OUTPUT_NAME "dsetup")
target_compile_features(dsetup PRIVATE cxx_std_17)
target_compile_definitions(dsetup PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_include_directories(dsetup PRIVATE ${minhook_SOURCE_DIR}/include)
target_link_libraries(dsetup PRIVATE minhook)
target_compile_options(dsetup PRIVATE /W3 /EHsc /O2)
