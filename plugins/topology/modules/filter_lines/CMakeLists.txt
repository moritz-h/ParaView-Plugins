cmake_minimum_required(VERSION 3.12)

pv_module(filter_lines ${PROJECT_NAME} "" filter_lines_target)

# Find and link Eigen
find_package(Eigen3 REQUIRED NO_MODULE)

if(NOT TARGET Eigen3::Eigen)
  message(FATAL_ERROR "Target for eigen3 not found")
endif()

target_link_libraries(${filter_lines_target} PRIVATE Eigen3::Eigen)
