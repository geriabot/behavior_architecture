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
# The LLM orchestrators (llm, mcp, llm_bt) need llm_planner_interfaces and llm_bt_builder, from
# llm.repos. Without them the base (BaseOrchestrator, BehaviorRunner, mission_executor with the
# plugin orchestrators) is built alone.
find_package(llm_planner_interfaces QUIET)
find_package(llm_bt_builder QUIET)
if(llm_planner_interfaces_FOUND AND llm_bt_builder_FOUND)
  set(WITH_LLM_ORCHESTRATORS ON)
  message(STATUS "behavior_architecture: building the LLM orchestrators (llm, mcp, llm_bt)")
else()
  set(WITH_LLM_ORCHESTRATORS OFF)
  message(STATUS "behavior_architecture: llm_planner_interfaces or llm_bt_builder not found: "
    "the LLM orchestrators and their executables are not built (import llm.repos to build them)")
endif()
find_package(geometry_msgs REQUIRED)
find_package(tf2_msgs REQUIRED)
find_package(tf2_ros REQUIRED)

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

# Generic mission executor (YAML-configured): the example orchestrators, the orchestrators of
# the libraries listed in the YAML (orchestrator_libraries) and, if built, the LLM orchestrators
add_executable(mission_executor
  src/mission_executor.cpp
  src/yaml_utils.cpp
  src/examples/restaurant_orchestrator.cpp
  src/examples/simple_orchestrator.cpp
)
target_link_libraries(mission_executor ${PROJECT_NAME} yaml-cpp)
ament_target_dependencies(mission_executor ${dependencies})
if(WITH_LLM_ORCHESTRATORS)
  target_sources(mission_executor PRIVATE
    src/llm_plan_orchestrator.cpp
    src/mcp_llm_plan_orchestrator.cpp
  )
  target_compile_definitions(mission_executor PRIVATE BEHAVIOR_ARCHITECTURE_WITH_LLM)
  ament_target_dependencies(mission_executor llm_planner_interfaces llm_bt_builder)
endif()

install(TARGETS mission_executor
  DESTINATION lib/${PROJECT_NAME}
)

# Test Plan Orchestrator (test_plan_executor)
add_executable(test_plan_executor
  src/test_plan_executor_main.cpp
  src/test_plan_orchestrator.cpp
)
target_link_libraries(test_plan_executor ${PROJECT_NAME} yaml-cpp)
ament_target_dependencies(test_plan_executor ${dependencies})

install(TARGETS test_plan_executor
  DESTINATION lib/${PROJECT_NAME}
)
# Static TF publisher node
add_executable(static_tf_publisher src/static_tf_publisher.cpp)
target_link_libraries(static_tf_publisher yaml-cpp)
ament_target_dependencies(static_tf_publisher rclcpp geometry_msgs tf2 tf2_msgs tf2_ros)

install(TARGETS static_tf_publisher
  DESTINATION lib/${PROJECT_NAME}
)

# Example: Restaurant orchestrator
add_executable(restaurant_example
  src/examples/restaurant_main.cpp
  src/examples/restaurant_orchestrator.cpp
  src/yaml_utils.cpp
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
  src/yaml_utils.cpp
)
target_link_libraries(simple_example ${PROJECT_NAME} yaml-cpp)
ament_target_dependencies(simple_example ${dependencies} ament_index_cpp)

install(TARGETS simple_example
  DESTINATION lib/${PROJECT_NAME}
)

if(WITH_LLM_ORCHESTRATORS)
  # LLM-driven plan executor
  add_executable(llm_plan_executor
    src/llm_plan_executor_main.cpp
    src/llm_plan_orchestrator.cpp
    src/mcp_llm_plan_orchestrator.cpp
    src/yaml_utils.cpp
  )
  target_link_libraries(llm_plan_executor ${PROJECT_NAME} yaml-cpp)
  ament_target_dependencies(llm_plan_executor ${dependencies} llm_planner_interfaces llm_bt_builder)

  install(TARGETS llm_plan_executor
    DESTINATION lib/${PROJECT_NAME}
  )

  add_executable(llm_bt_executor
    src/llm_bt_executor_main.cpp
    src/llm_bt_orchestrator.cpp
    src/yaml_utils.cpp
  )
  target_link_libraries(llm_bt_executor ${PROJECT_NAME} yaml-cpp)
  ament_target_dependencies(llm_bt_executor ${dependencies} llm_bt_builder)

  install(TARGETS llm_bt_executor
    DESTINATION lib/${PROJECT_NAME}
  )

  # Client to send a mission to the LLM orchestrator

  add_executable(start_mission src/start_mission.cpp)
  target_link_libraries(start_mission yaml-cpp)
  ament_target_dependencies(start_mission rclcpp llm_planner_interfaces geometry_msgs tf2_msgs)

  install(TARGETS start_mission
    DESTINATION lib/${PROJECT_NAME}
  )
endif()

# Interpretation (docs/INTERPRETATION_DESIGN.md): the Interpret service, the server (Python),
# the C++ client for orchestrators and the BT nodes Interpret and InterpretationResult. These
# targets use only target_link_libraries with the targets of each dependency, which builds the
# same on Jazzy and Lyrical; nothing above depends on them.
#
# Interpretation is opt-in for the packages that find this one: what the interface and the
# client export is kept out of the package's default variables (behavior_architecture_TARGETS,
# _LIBRARIES, _INCLUDE_DIRS, _DEPENDENCIES), which hold only the base library, as without
# interpretation; a package links interpretation by naming it (behavior_architecture::interpretation
# or ${behavior_architecture_INTERPRETATION_TARGETS}), from cmake/interpretation-extras.cmake.in.
#
# The base's exports are declared first, as without interpretation: the order of the first
# calls is the order in which the config file computes the default variables
ament_export_include_directories(include)
ament_export_libraries(${PROJECT_NAME})
ament_export_dependencies(${dependencies})
# Right after the config files of those exports (the hooks of ament_package() run in the order
# in which they are registered, and rosidl's are registered later), the config file keeps the
# default variables (cmake/default_variables_snapshot-extras.cmake), which its last file
# restores (cmake/default_variables-extras.cmake.in): whatever the config files of rosidl add
# to them, in any version, a package that does not ask for interpretation receives the
# variables of the base alone
set(behavior_architecture_default_variables_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
ament_register_extension("ament_package" "behavior_architecture_default_variables"
  "default_variables_package_hook.cmake")

find_package(ament_cmake_python REQUIRED)
find_package(rosidl_default_generators REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(nlohmann_json REQUIRED)

# The export lists that rosidl_generate_interfaces() fills: what it adds is moved to the opt-in
# files
set(_interpretation_export_lists
  _AMENT_CMAKE_EXPORT_TARGETS
  _AMENT_EXPORT_LIBRARIES
  _AMENT_CMAKE_EXPORT_DEPENDENCIES
  _AMENT_EXPORT_RELATIVE_INCLUDE_DIRECTORIES
  _AMENT_EXPORT_ABSOLUTE_INCLUDE_DIRECTORIES
)
foreach(_list ${_interpretation_export_lists})
  set(_interpretation_saved${_list} "${${_list}}")
endforeach()

# The interface has its own target name: ${PROJECT_NAME} is the base library
rosidl_generate_interfaces(${PROJECT_NAME}_interfaces
  "srv/Interpret.srv"
  "srv/CancelInterpretation.srv"
  DEPENDENCIES sensor_msgs
)
rosidl_get_typesupport_target(interpretation_typesupport_target
  ${PROJECT_NAME}_interfaces "rosidl_typesupport_cpp")
# The typesupport dispatch (C, C++ and Python) loads the libraries of an interface by its
# package's name (lib<package>__<typesupport>.so): the generated libraries get the names that
# a target named ${PROJECT_NAME} would have given them.
get_property(_interpretation_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(_target ${_interpretation_targets})
  get_target_property(_type ${_target} TYPE)
  if(_target MATCHES "^${PROJECT_NAME}_interfaces__(.+)$" AND
      (_type STREQUAL "SHARED_LIBRARY" OR _type STREQUAL "MODULE_LIBRARY"))
    set_target_properties(${_target} PROPERTIES OUTPUT_NAME "${PROJECT_NAME}__${CMAKE_MATCH_1}")
  endif()
endforeach()
# rosidl has already exported those libraries by their former names, for the variables
# ${PROJECT_NAME}_LIBRARIES__<typesupport> of the packages that generate interfaces depending on
# this one: export them by their names
list(TRANSFORM _ROSIDL_CMAKE_EXPORT_TYPESUPPORT_LIBRARIES
  REPLACE ":${PROJECT_NAME}_interfaces__" ":${PROJECT_NAME}__")
# Some builds of Jazzy's rosidl_generator_py export the Python bindings library with an empty
# suffix (the variable rosidl_generator_py_suffix is not set), which puts it in
# ${PROJECT_NAME}_TARGETS: export it in ${PROJECT_NAME}_TARGETS__rosidl_generator_py, as the
# builds that set the suffix do
list(TRANSFORM _ROSIDL_CMAKE_EXPORT_TYPESUPPORT_TARGETS
  REPLACE "^:(${PROJECT_NAME}_interfaces__rosidl_generator_py)$" "__rosidl_generator_py:\\1")

# Out of the default lists: the interface's export sets (installed here, as
# ament_export_targets() would) and the dependencies that its targets need
set(_interpretation_export_sets "")
foreach(_export ${_AMENT_CMAKE_EXPORT_TARGETS})
  if(NOT _export IN_LIST _interpretation_saved_AMENT_CMAKE_EXPORT_TARGETS)
    list(APPEND _interpretation_export_sets "${_export}")
    install(EXPORT "${_export}"
      DESTINATION share/${PROJECT_NAME}/cmake
      NAMESPACE "${PROJECT_NAME}::"
      FILE "${_export}Export.cmake"
    )
  endif()
endforeach()
set(_interpretation_export_dependencies ${_AMENT_CMAKE_EXPORT_DEPENDENCIES})
list(REMOVE_ITEM _interpretation_export_dependencies
  ${_interpretation_saved_AMENT_CMAKE_EXPORT_DEPENDENCIES})
list(APPEND _interpretation_export_dependencies rclcpp nlohmann_json)
list(REMOVE_DUPLICATES _interpretation_export_dependencies)
foreach(_list ${_interpretation_export_lists})
  set(${_list} "${_interpretation_saved${_list}}")
endforeach()

# The server: the Python package behavior_interpretation (the generated Python module is
# behavior_architecture.srv) and its executable
ament_python_install_package(behavior_interpretation)
install(PROGRAMS scripts/interpretation_server
  DESTINATION lib/${PROJECT_NAME}
)

# The non-blocking client, for orchestrators (by composition); exported as
# behavior_architecture::interpretation
add_library(${PROJECT_NAME}_interpretation SHARED
  src/interpretation/interpretation_client.cpp
)
set_target_properties(${PROJECT_NAME}_interpretation PROPERTIES EXPORT_NAME interpretation)
target_include_directories(${PROJECT_NAME}_interpretation PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:include>")
target_link_libraries(${PROJECT_NAME}_interpretation
  ${interpretation_typesupport_target}
  rclcpp::rclcpp
  ${sensor_msgs_TARGETS}
  nlohmann_json::nlohmann_json
)

# The BT plugin library (BT_RegisterNodesFromPlugin): Interpret and InterpretationResult
add_library(${PROJECT_NAME}_interpretation_bt_nodes SHARED
  src/interpretation/interpret_bt_nodes.cpp
)
target_include_directories(${PROJECT_NAME}_interpretation_bt_nodes PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:include>")
target_link_libraries(${PROJECT_NAME}_interpretation_bt_nodes
  ${PROJECT_NAME}_interpretation
  behaviortree_cpp::behaviortree_cpp
)
target_compile_definitions(${PROJECT_NAME}_interpretation_bt_nodes PRIVATE BT_PLUGIN_EXPORT)

install(TARGETS ${PROJECT_NAME}_interpretation
  EXPORT export_${PROJECT_NAME}_interpretation
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)
install(EXPORT export_${PROJECT_NAME}_interpretation
  DESTINATION share/${PROJECT_NAME}/cmake
  NAMESPACE "${PROJECT_NAME}::"
  FILE "export_${PROJECT_NAME}_interpretationExport.cmake"
)
list(APPEND _interpretation_export_sets "export_${PROJECT_NAME}_interpretation")
# The plugin is loaded by name (plugin_libraries), not linked: it is not exported
install(TARGETS ${PROJECT_NAME}_interpretation_bt_nodes
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)

if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)
  find_package(ament_cmake_pytest REQUIRED)
  # Tests that create ROS nodes run in a ROS domain of their own when ament_cmake_ros is
  # available, so that they do not see (or disturb) other ROS processes of the computer
  find_package(ament_cmake_ros QUIET)
  if(ament_cmake_ros_FOUND)
    set(_interpretation_add_gtest ament_add_ros_isolated_gtest)
    set(_interpretation_add_pytest ament_add_ros_isolated_pytest_test)
  else()
    set(_interpretation_add_gtest ament_add_gtest)
    set(_interpretation_add_pytest ament_add_pytest_test)
  endif()

  # The server, with the fake and recorded backends (no model is contacted)
  cmake_language(CALL ${_interpretation_add_pytest} test_interpretation_server
    "${CMAKE_CURRENT_SOURCE_DIR}/test/interpretation"
    APPEND_ENV PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}
    TIMEOUT 300
  )

  # The client and the BT nodes, against a fake server node
  cmake_language(CALL ${_interpretation_add_gtest} test_interpretation_client
    test/interpretation/test_interpretation_client.cpp
    SKIP_LINKING_MAIN_LIBRARIES
    TIMEOUT 120
  )
  if(TARGET test_interpretation_client)
    target_link_libraries(test_interpretation_client
      ${PROJECT_NAME}_interpretation
      rclcpp_lifecycle::rclcpp_lifecycle
    )
  endif()

  cmake_language(CALL ${_interpretation_add_gtest} test_interpretation_bt_nodes
    test/interpretation/test_interpretation_bt_nodes.cpp
    SKIP_LINKING_MAIN_LIBRARIES
    TIMEOUT 120
  )
  if(TARGET test_interpretation_bt_nodes)
    target_link_libraries(test_interpretation_bt_nodes
      ${PROJECT_NAME}_interpretation
      behaviortree_cpp::behaviortree_cpp
    )
    target_compile_definitions(test_interpretation_bt_nodes PRIVATE
      INTERPRETATION_BT_PLUGIN="$<TARGET_FILE:${PROJECT_NAME}_interpretation_bt_nodes>")
    add_dependencies(test_interpretation_bt_nodes ${PROJECT_NAME}_interpretation_bt_nodes)
  endif()

  # The base: runners that cannot build their tree, the runners of the configuration and of
  # code, and the files of the episodic memory
  cmake_language(CALL ${_interpretation_add_gtest} test_base_orchestrator
    test/base/test_base_orchestrator.cpp
    SKIP_LINKING_MAIN_LIBRARIES
    TIMEOUT 120
  )
  if(TARGET test_base_orchestrator)
    target_link_libraries(test_base_orchestrator ${PROJECT_NAME})
    ament_target_dependencies(test_base_orchestrator ${dependencies})
  endif()
  ament_add_gtest(test_episodic_memory_files test/base/test_episodic_memory_files.cpp)

  # test_plan_executor runs the trees of a directory (with a plugin that has no dependencies)
  cmake_language(CALL ${_interpretation_add_pytest} test_test_plan_executor
    "${CMAKE_CURRENT_SOURCE_DIR}/test/base/test_test_plan_executor.py"
    ENV
      TEST_PLAN_EXECUTOR=$<TARGET_FILE:test_plan_executor>
      TEST_PLAN_PLUGIN=$<TARGET_FILE:${PROJECT_NAME}_interpretation_bt_nodes>
    TIMEOUT 120
  )
endif()

if(BUILD_TESTING)
  find_package(ament_lint_auto REQUIRED)
  set(ament_cmake_cpplint_FOUND TRUE)
  ament_lint_auto_find_test_dependencies()
endif()

# Export information: the base's, declared above, before the interface. The opt-in
# interpretation targets are defined first (rosidl's extras expect them), and their
# dependencies are found after the default variables are computed.
ament_package(
  CONFIG_EXTRAS cmake/interpretation-extras.cmake.in
  CONFIG_EXTRAS_POST
    cmake/interpretation_dependencies-extras.cmake.in
    cmake/default_variables-extras.cmake.in
)
