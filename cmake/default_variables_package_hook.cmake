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

# ament_package() hook (registered in CMakeLists.txt right after the base's exports): adds the
# config file that keeps the default variables, after the config files of the base's exports
list(APPEND ${PROJECT_NAME}_CONFIG_EXTRAS
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/default_variables_snapshot-extras.cmake")
