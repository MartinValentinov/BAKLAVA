cmake_minimum_required(VERSION 3.22)

# Orin AGX is SM 8.7. Must be set before project() enables CUDA -- by the
# time project() returns, CMake has already cached its own auto-detected
# default and this guard would be a no-op.
if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
  set(CMAKE_CUDA_ARCHITECTURES 87)
endif()

project(sar_ship_detect LANGUAGES CXX CUDA)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 17)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O3")
set(CMAKE_CUDA_FLAGS_RELEASE "-O3 --use_fast_math")

find_package(CUDAToolkit REQUIRED)

# ---- GDAL ------------------------------------------------------------------
# WARNING: if you are inside a conda env, pkg-config will find conda's GDAL
# rather than the system one. Conda GDAL is built against a different GEOS and
# a different libstdc++ ABI, and landmask.cpp leans on GEOS through Buffer()
# and Intersection(). Build with `conda deactivate` unless you have a reason
# not to. Configure output prints the version it picked -- check it.
find_package(PkgConfig REQUIRED)
pkg_check_modules(GDAL gdal)
if(NOT GDAL_FOUND)
  find_path(GDAL_INCLUDE_DIRS gdal_priv.h PATH_SUFFIXES gdal)
  find_library(GDAL_LIBRARIES NAMES gdal)
endif()
if(NOT GDAL_INCLUDE_DIRS OR NOT GDAL_LIBRARIES)
  message(FATAL_ERROR "GDAL not found. apt install libgdal-dev")
endif()
message(STATUS "GDAL version : ${GDAL_VERSION}")
message(STATUS "GDAL includes: ${GDAL_INCLUDE_DIRS}")
message(STATUS "GDAL libs    : ${GDAL_LIBRARIES}")
if(GDAL_LIBRARY_DIRS)
  link_directories(${GDAL_LIBRARY_DIRS})
endif()

# ---- TensorRT --------------------------------------------------------------
set(TRT_ROOT "/usr" CACHE PATH "TensorRT install prefix")
find_path(TRT_INCLUDE_DIR NvInfer.h
          HINTS ${TRT_ROOT}/include ${TRT_ROOT}/include/aarch64-linux-gnu)
find_library(TRT_NVINFER    nvinfer      HINTS ${TRT_ROOT}/lib ${TRT_ROOT}/lib/aarch64-linux-gnu)
find_library(TRT_ONNXPARSER nvonnxparser HINTS ${TRT_ROOT}/lib ${TRT_ROOT}/lib/aarch64-linux-gnu)
if(NOT TRT_INCLUDE_DIR OR NOT TRT_NVINFER OR NOT TRT_ONNXPARSER)
  message(FATAL_ERROR "TensorRT not found. Set -DTRT_ROOT=/path/to/tensorrt")
endif()
message(STATUS "TensorRT inc : ${TRT_INCLUDE_DIR}")

# ---- target ----------------------------------------------------------------
# Flat layout: every source and header sits next to this file.
add_executable(sar_ship_detect
  main.cpp
  config.cpp
  scene.cpp
  landmask.cpp
  engine.cpp
  render.cpp
  kernels.cu
)

target_include_directories(sar_ship_detect PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${GDAL_INCLUDE_DIRS}
  ${TRT_INCLUDE_DIR}
)

target_link_libraries(sar_ship_detect PRIVATE
  ${GDAL_LIBRARIES}
  ${TRT_NVINFER}
  ${TRT_ONNXPARSER}
  CUDA::cudart
  CUDA::nvjpeg
)

set_target_properties(sar_ship_detect PROPERTIES
  CUDA_SEPARABLE_COMPILATION ON
)