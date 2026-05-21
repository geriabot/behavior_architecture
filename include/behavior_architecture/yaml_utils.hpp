#ifndef BEHAVIOR_ARCHITECTURE__YAML_UTILS_HPP_
#define BEHAVIOR_ARCHITECTURE__YAML_UTILS_HPP_

#include <string>
#include <vector>

namespace behavior_architecture {

std::string append_yaml_multiline_block(const std::string& base, const std::string& block_name, const std::string& block_content);
std::string append_yaml_string_list(const std::string & base, const std::string & key, const std::vector<std::string> & values);

} // namespace behavior_architecture

#endif // BEHAVIOR_ARCHITECTURE__YAML_UTILS_HPP_
