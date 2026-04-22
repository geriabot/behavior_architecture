#ifndef BEHAVIOR_ARCHITECTURE__YAML_UTILS_HPP_
#define BEHAVIOR_ARCHITECTURE__YAML_UTILS_HPP_

#include <string>

namespace behavior_architecture {

std::string append_yaml_multiline_block(const std::string& base, const std::string& block_name, const std::string& block_content);

} // namespace behavior_architecture

#endif // BEHAVIOR_ARCHITECTURE__YAML_UTILS_HPP_
