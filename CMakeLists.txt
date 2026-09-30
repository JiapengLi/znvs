cmake_minimum_required(VERSION 3.16)
project(znvs VERSION 1.0.0 LANGUAGES C)

set(ZNVS_DATA_CRC 1 CACHE STRING "Payload CRC32: 0 or 1 (persistent format choice)")
set(ZNVS_CACHE_SIZE 0 CACHE STRING "Lookup slots: 0 or a power of two")
set(ZNVS_IO_SIZE 32 CACHE STRING "Scratch bytes: power of two, 8..256")
add_library(znvs STATIC znvs/znvs.c)
add_library(znvs::znvs ALIAS znvs)
target_include_directories(znvs PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/znvs>)
target_compile_features(znvs PUBLIC c_std_99)
# PUBLIC is intentional: these options affect both implementation and header ABI.
target_compile_definitions(znvs PUBLIC
    ZNVS_DATA_CRC=${ZNVS_DATA_CRC}
    ZNVS_CACHE_SIZE=${ZNVS_CACHE_SIZE}
    ZNVS_IO_SIZE=${ZNVS_IO_SIZE})
set_target_properties(znvs PROPERTIES C_EXTENSIONS OFF)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(ZNVS_TOP_LEVEL ON)
else()
    set(ZNVS_TOP_LEVEL OFF)
endif()
option(ZNVS_BUILD_TESTS "Build host NOR simulator tests" ${ZNVS_TOP_LEVEL})
option(ZNVS_BUILD_EXAMPLE "Build the RAM-flash host example" ${ZNVS_TOP_LEVEL})
if(ZNVS_BUILD_TESTS)
    enable_testing()
    add_executable(test_znvs tests/test_znvs.c)
    target_link_libraries(test_znvs PRIVATE znvs::znvs)
    add_test(NAME znvs_nor COMMAND test_znvs)
endif()
if(ZNVS_BUILD_EXAMPLE)
    add_executable(znvs_demo examples/basic.c)
    target_link_libraries(znvs_demo PRIVATE znvs::znvs)
endif()
