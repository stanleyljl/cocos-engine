cmake_minimum_required(VERSION 3.16)
project(LandscapeVTPagingTests LANGUAGES CXX)
enable_testing()
add_executable(landscape_vt_paging_test paging_test.cpp ../../cocos/landscape/Data.cpp
    ../../cocos/landscape/Quadtree.cpp
    ../../cocos/core/geometry/Frustum.cpp
    ../../cocos/core/geometry/Plane.cpp
    ../../cocos/core/geometry/AABB.cpp
    ../../cocos/math/Quaternion.cpp
    ../../cocos/math/Mat3.cpp
    ../../cocos/math/Math.cpp
    ../../cocos/math/Mat4.cpp
    ../../cocos/landscape/VTRenderer.cpp
    ../../cocos/landscape/Residency.cpp ../../cocos/base/RefCounted.cpp
    ../../cocos/math/Vec3.cpp ../../cocos/math/Vec4.cpp ../../cocos/math/MathUtil.cpp)
target_compile_features(landscape_vt_paging_test PRIVATE cxx_std_17)
target_compile_definitions(landscape_vt_paging_test PRIVATE LANDSCAPE_PARITY_TESTS="${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/landscape")
target_compile_definitions(landscape_vt_paging_test PRIVATE BOOST_ALL_NO_LIB BOOST_CONTAINER_NO_LIB CC_DEBUG=0 CC_STATIC _USE_MATH_DEFINES)
if(WIN32)
    target_compile_definitions(landscape_vt_paging_test PRIVATE CC_PLATFORM=2 CC_PLATFORM_WINDOWS=2 CC_PLATFORM_ANDROID=3)
    target_include_directories(landscape_vt_paging_test PRIVATE ../../external/win64/include ../../external/win64/include/v8)
else()
    target_compile_definitions(landscape_vt_paging_test PRIVATE CC_PLATFORM=100 CC_PLATFORM_WINDOWS=2 CC_PLATFORM_ANDROID=3)
    if(APPLE)
        target_include_directories(landscape_vt_paging_test PRIVATE ../../external/mac/include ../../external/mac/include/v8)
    else()
        target_include_directories(landscape_vt_paging_test PRIVATE ../../external/linux/include ../../external/linux/include/v8)
    endif()
endif()
if(MSVC)
    target_compile_options(landscape_vt_paging_test PRIVATE /utf-8)
endif()
target_include_directories(landscape_vt_paging_test PRIVATE ../.. ../../cocos ../../cocos/renderer ../../external/sources)
add_test(NAME landscape_vt_paging COMMAND landscape_vt_paging_test)

add_executable(landscape_query_test query_test.cpp
    ../../cocos/landscape/Query.cpp ../../cocos/landscape/Data.cpp
    ../../cocos/math/Vec3.cpp ../../cocos/math/MathUtil.cpp)
target_compile_features(landscape_query_test PRIVATE cxx_std_17)
target_compile_definitions(landscape_query_test PRIVATE BOOST_ALL_NO_LIB BOOST_CONTAINER_NO_LIB CC_DEBUG=0 CC_STATIC)
if(WIN32)
    target_compile_definitions(landscape_query_test PRIVATE CC_PLATFORM=2 CC_PLATFORM_WINDOWS=2 CC_PLATFORM_ANDROID=3)
else()
    target_compile_definitions(landscape_query_test PRIVATE CC_PLATFORM=100 CC_PLATFORM_WINDOWS=2 CC_PLATFORM_ANDROID=3)
endif()
if(MSVC)
    target_compile_options(landscape_query_test PRIVATE /utf-8)
endif()
target_include_directories(landscape_query_test PRIVATE ../../cocos ../../external/sources)
add_test(NAME landscape_query COMMAND landscape_query_test)

# Link the real scheduler/tree/layout with deterministic IO and GPU boundary doubles.
add_executable(landscape_schedule_test schedule_test.cpp
    ../../cocos/landscape/Schedule.cpp ../../cocos/landscape/Quadtree.cpp ../../cocos/landscape/Data.cpp
    ../../cocos/core/geometry/Frustum.cpp ../../cocos/core/geometry/Plane.cpp ../../cocos/core/geometry/AABB.cpp
    ../../cocos/math/Quaternion.cpp ../../cocos/math/Mat3.cpp ../../cocos/math/Math.cpp ../../cocos/math/Mat4.cpp
    ../../cocos/math/Vec3.cpp ../../cocos/math/Vec4.cpp ../../cocos/math/MathUtil.cpp ../../cocos/base/RefCounted.cpp)
target_compile_features(landscape_schedule_test PRIVATE cxx_std_17)
target_compile_definitions(landscape_schedule_test PRIVATE $<TARGET_PROPERTY:landscape_vt_paging_test,COMPILE_DEFINITIONS>)
target_include_directories(landscape_schedule_test PRIVATE $<TARGET_PROPERTY:landscape_vt_paging_test,INCLUDE_DIRECTORIES>)
if(MSVC)
    target_compile_options(landscape_schedule_test PRIVATE /utf-8)
endif()
add_test(NAME landscape_schedule COMMAND landscape_schedule_test)
