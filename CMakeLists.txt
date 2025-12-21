cmake_minimum_required(VERSION 3.8)
project(behavior_architecture)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

set(CMAKE_CXX_STANDARD 17)

find_package(ament_cmake REQUIRED)
find_package(ament_index_cpp REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rclcpp_cascade_lifecycle REQUIRED)
find_package(behaviortree_cpp REQUIRED)
find_package(std_msgs REQUIRED)
find_package(std_srvs REQUIRED)
find_package(social_bt_nodes REQUIRED)

set(dependencies
  rclcpp
  ament_index_cpp
  rclcpp_cascade_lifecycle
  behaviortree_cpp
  std_msgs
  std_srvs
  social_bt_nodes
)

include_directories(include)

# Base orchestrator library (to be used by other packages)
add_library(${PROJECT_NAME} SHARED
  src/behavior_architecture/base_orchestrator.cpp
  src/behavior_architecture/behavior_runner.cpp
)
ament_target_dependencies(${PROJECT_NAME} ${dependencies})

# Install library
install(TARGETS ${PROJECT_NAME}
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)

# Install headers
install(DIRECTORY include/
  DESTINATION include
)

# Install behaviors directory
install(DIRECTORY behaviors/
  DESTINATION share/${PROJECT_NAME}/behaviors
)

# Install launch directory
install(DIRECTORY launch/
  DESTINATION share/${PROJECT_NAME}/launch
)

# Example: Restaurant orchestrator
add_executable(restaurant_example
  src/examples/restaurant_main.cpp
  src/examples/restaurant_orchestrator.cpp
)
target_link_libraries(restaurant_example ${PROJECT_NAME})
ament_target_dependencies(restaurant_example ${dependencies})

install(TARGETS restaurant_example
  DESTINATION lib/${PROJECT_NAME}
)

# Example: Simple 2-state orchestrator
add_executable(simple_example
  src/examples/simple_main.cpp
  src/examples/simple_orchestrator.cpp
)
target_link_libraries(simple_example ${PROJECT_NAME})
ament_target_dependencies(simple_example ${dependencies})

install(TARGETS simple_example
  DESTINATION lib/${PROJECT_NAME}
)

if(BUILD_TESTING)
  find_package(ament_lint_auto REQUIRED)
  set(ament_cmake_cpplint_FOUND TRUE)
  ament_lint_auto_find_test_dependencies()
endif()

# Export information
ament_export_include_directories(include)
ament_export_libraries(${PROJECT_NAME})
ament_export_dependencies(${dependencies})

ament_package()
