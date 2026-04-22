#include "behavior_architecture/yaml_utils.hpp"
#include <sstream>

namespace behavior_architecture {

std::string append_yaml_multiline_block(const std::string& base, const std::string& block_name, const std::string& block_content) {
  std::ostringstream oss;
  oss << base;
  if (!base.empty() && base.back() != '\n') {
    oss << "\n";
  }
  oss << block_name << ": |\n";
  std::istringstream iss(block_content);
  std::string line;
  while (std::getline(iss, line)) {
    oss << "  " << line << "\n";
  }
  return oss.str();
}


} // namespace behavior_architecture
