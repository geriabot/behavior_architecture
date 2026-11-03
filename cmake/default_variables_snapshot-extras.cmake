# Copyright 2026 Rodrigo Pérez-Rodríguez
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# The package's default variables, from the base library alone: kept here, right after the
# config files of the base's exports (cmake/default_variables_package_hook.cmake), and restored
# by the package's last config file (default_variables-extras.cmake).

foreach(_behavior_architecture_var TARGETS LIBRARIES INCLUDE_DIRS LIBRARY_DIRS DEPENDENCIES
    RECURSIVE_DEPENDENCIES DEFINITIONS LINK_FLAGS)
  if(DEFINED behavior_architecture_${_behavior_architecture_var})
    set(_behavior_architecture_default_${_behavior_architecture_var}
      "${behavior_architecture_${_behavior_architecture_var}}")
  else()
    unset(_behavior_architecture_default_${_behavior_architecture_var})
  endif()
endforeach()
set(_behavior_architecture_default_variables_kept TRUE)
unset(_behavior_architecture_var)
