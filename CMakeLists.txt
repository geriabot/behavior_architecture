cmake_minimum_required(VERSION 3.8)
project(behavior_architecture)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

set(CMAKE_CXX_STANDARD 17)

find_package(ament_cmake REQUIRED)
find_package(ament_index_cpp REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rclcpp_lifecycle REQUIRED)
find_package(behaviortree_cpp REQUIRED)
find_package(std_msgs REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(llm_planner_interfaces REQUIRED)
find_package(llm_bt_builder REQUIRED)

set(dependencies
  rclcpp
  ament_index_cpp
  rclcpp_lifecycle
  behaviortree_cpp
  std_msgs
)

include_directories(include)

# Base orchestrator library (to be used by other packages)
add_library(${PROJECT_NAME} SHARED
  src/behavior_architecture/base_orchestrator.cpp
  src/behavior_architecture/behavior_runner.cpp
  src/behavior_architecture/orchestrator_factory.cpp
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

# Install behaviors directory (with subdirectories)
install(DIRECTORY behaviors/
  DESTINATION share/${PROJECT_NAME}/behaviors
  FILES_MATCHING
  PATTERN "*.xml"
  PATTERN "*.md"
)

# Install launch directory
install(DIRECTORY launch/
  DESTINATION share/${PROJECT_NAME}/launch
)

# Install config directory
install(DIRECTORY config/
  DESTINATION share/${PROJECT_NAME}/config
)

# Generic mission executor (YAML-configured, supports both fixed and LLM modes)
add_executable(mission_executor
  src/mission_executor.cpp
  src/llm_plan_orchestrator.cpp
  src/examples/restaurant_orchestrator.cpp
  src/examples/simple_orchestrator.cpp
)
target_link_libraries(mission_executor ${PROJECT_NAME} yaml-cpp)
ament_target_dependencies(mission_executor ${dependencies} llm_planner_interfaces llm_bt_builder)

install(TARGETS mission_executor
  DESTINATION lib/${PROJECT_NAME}
)

# 
# Example: Restaurant orchestrator
add_executable(restaurant_example
  src/examples/restaurant_main.cpp
  src/examples/restaurant_orchestrator.cpp
)
target_link_libraries(restaurant_example ${PROJECT_NAME} yaml-cpp)
ament_target_dependencies(restaurant_example ${dependencies} ament_index_cpp)

install(TARGETS restaurant_example
  DESTINATION lib/${PROJECT_NAME}
)

# Example: Simple 2-state orchestrator
add_executable(simple_example
  src/examples/simple_main.cpp
  src/examples/simple_orchestrator.cpp
)
target_link_libraries(simple_example ${PROJECT_NAME} yaml-cpp)
ament_target_dependencies(simple_example ${dependencies} ament_index_cpp)

install(TARGETS simple_example
  DESTINATION lib/${PROJECT_NAME}
)

# LLM-driven plan executor
add_executable(llm_plan_executor
  src/llm_plan_executor_main.cpp
  src/llm_plan_orchestrator.cpp
)
target_link_libraries(llm_plan_executor ${PROJECT_NAME} yaml-cpp)
ament_target_dependencies(llm_plan_executor ${dependencies} llm_planner_interfaces llm_bt_builder)

install(TARGETS llm_plan_executor
  DESTINATION lib/${PROJECT_NAME}
)

# Test client for llm_plan_executor
add_executable(test_start_goal src/test_start_goal.cpp)
target_link_libraries(test_start_goal yaml-cpp)
ament_target_dependencies(test_start_goal rclcpp llm_planner_interfaces)

install(TARGETS test_start_goal
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
