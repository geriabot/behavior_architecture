// Copyright 2026 Rodrigo Perez-Rodriguez
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "behavior_architecture/mcp_llm_plan_orchestrator.hpp"

#include "behavior_architecture/orchestrator_factory.hpp"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace behavior_architecture
{

// Register MCP orchestrator aliases for A/B tests while keeping llm unchanged.
static OrchestratorRegistrar<MCPLLMPlanOrchestrator> mcp_registrar("mcp");
static OrchestratorRegistrar<MCPLLMPlanOrchestrator> mcp_llm_registrar("mcp_llm");
static OrchestratorRegistrar<MCPLLMPlanOrchestrator> mcp_llm_plan_orchestrator_registrar("mcp_llm_plan_orchestrator");

MCPLLMPlanOrchestrator::MCPLLMPlanOrchestrator(BT::Blackboard::Ptr blackboard)
: LLMPlanOrchestrator(blackboard)
{
	std::string base_exec_dir = "exec";
	try {
		base_exec_dir = blackboard_->get<std::string>("llm_exec_dir");
	} catch (...) {
	}

	try {
		mcp_context_dir_ = blackboard_->get<std::string>("llm_mcp_context_dir");
	} catch (...) {
		mcp_context_dir_ = std::filesystem::path(base_exec_dir) / "mcp_context";
	}

	std::error_code ec;
	std::filesystem::create_directories(mcp_context_dir_, ec);
	if (ec) {
		RCLCPP_WARN(get_logger(), "Could not create MCP context dir '%s': %s",
			mcp_context_dir_.c_str(), ec.message().c_str());
	}

	mission_snapshot_path_ = mcp_context_dir_ / "mission_snapshot.json";
	failure_history_path_ = mcp_context_dir_ / "failure_history.json";
	plan_failure_case_base_path_ = mcp_context_dir_ / "plan_failure_episodic_memory.json";
	bt_failure_case_base_path_ = mcp_context_dir_ / "bt_failure_episodic_memory.json";
	plan_case_base_path_ = mcp_context_dir_ / "plan__success_episodic_memory.json";
	bt_case_base_path_ = mcp_context_dir_ / "bt_success_episodic_memory.json";

	state_snapshot_ = "IDLE";
	last_event_ = "orchestrator_initialized";
	execution_start_time_ = std::chrono::steady_clock::now();
	plan_was_replanned_ = false;
	step_had_fix_.clear();
	
	// Load previous execution cases for episodic memory persistence
	load_plan_failure_case_base();
	load_bt_failure_case_base();
	load_plan_case_base();
	load_bt_case_base();
	
	write_mission_snapshot(true, "MCP orchestrator initialized");
	write_failure_history();
}

void MCPLLMPlanOrchestrator::handle_start_mission(
	const llm_planner_interfaces::srv::StartMission::Request::SharedPtr req,
	llm_planner_interfaces::srv::StartMission::Response::SharedPtr resp)
{
	mission_name_snapshot_ = req->mission_name;
	goal_snapshot_ = req->goal;
	context_snapshot_ = req->context;
	failure_history_cache_.clear();
	step_had_fix_.clear();
	plan_was_replanned_ = false;
	last_event_ = "start_mission_request";

	LLMPlanOrchestrator::handle_start_mission(req, resp);

	write_mission_snapshot(resp->accepted, resp->message);
	write_failure_history();
}

void MCPLLMPlanOrchestrator::request_plan()
{
	last_event_ = "request_plan";
	write_mission_snapshot(true, "Goal accepted, planning...");
	LLMPlanOrchestrator::request_plan();
}

void MCPLLMPlanOrchestrator::request_replan()
{
	last_event_ = "request_replan";
	plan_was_replanned_ = true;
	write_mission_snapshot(true, "Replanning requested");
	LLMPlanOrchestrator::request_replan();
}

void MCPLLMPlanOrchestrator::request_generate_bt(const std::string & objective_yaml)
{
	last_event_ = "request_generate_bt";
	
	write_mission_snapshot(true, "BT generation requested");
	LLMPlanOrchestrator::request_generate_bt(objective_yaml);
}

void MCPLLMPlanOrchestrator::request_fix_bt(const std::string & broken_xml, const std::string & error_msg)
{
	last_event_ = "request_fix_bt";
	step_had_fix_[static_cast<int>(current_step_)] = true;
	write_mission_snapshot(true, "BT fix requested");
	LLMPlanOrchestrator::request_fix_bt(broken_xml, error_msg);
}

std::string MCPLLMPlanOrchestrator::collect_failure_reason()
{
	std::string reason = LLMPlanOrchestrator::collect_failure_reason();
	if (!reason.empty()) {
		FailureEntry entry;
		entry.timestamp = now_iso_utc();
		entry.reason = reason;
		entry.state = state_snapshot_;
		failure_history_cache_.push_back(entry);
		if (failure_history_cache_.size() > 200) {
			failure_history_cache_.erase(failure_history_cache_.begin());
		}
		last_event_ = "collect_failure_reason";
		record_bt_failure_case(reason);
		write_failure_history();
		write_mission_snapshot();
	}
	return reason;
}

void MCPLLMPlanOrchestrator::transition_to(State new_state)
{
	const State prev_state = state_;
	LLMPlanOrchestrator::transition_to(new_state);
	state_snapshot_ = state_name(new_state);
	last_event_ = "transition_to_" + state_snapshot_;
	write_mission_snapshot();

	// Record one BT case for each successful step transition from EXECUTING_BT.
	// Successful step leads to GENERATING_BT (next step) or SUCCESS (final step).
	if (prev_state == State::EXECUTING_BT &&
		(new_state == State::GENERATING_BT || new_state == State::SUCCESS) &&
		!last_bt_xml_.empty())
	{
		const int executed_step_id = static_cast<int>(current_step_) - 1;
		if (executed_step_id >= 0) {
			auto it = step_had_fix_.find(executed_step_id);
			const bool had_fix = (it != step_had_fix_.end()) ? it->second : false;
			if (!had_fix) {
				std::string bt_goal;
				if (static_cast<std::size_t>(executed_step_id) < steps_.size()) {
					bt_goal = steps_[executed_step_id].description;
				}
				record_successful_bt(executed_step_id, bt_goal, last_bt_xml_);
			}
		}
	}

	// Record one plan case only when mission completes successfully and no replanning was needed.
	if (new_state == State::SUCCESS && !plan_yaml_.empty() && !plan_was_replanned_) {
		record_successful_plan(plan_yaml_);
	}

	// Record terminal failures that may not pass through collect_failure_reason().
	if (new_state == State::FAILED) {
		std::string failure_cause = last_failure_reason_;
		if (failure_cause.empty()) {
			if (prev_state == State::WAITING_PLAN) {
				failure_cause = "Planning failed or returned empty plan";
			} else if (prev_state == State::WAITING_REPLAN) {
				failure_cause = "Replanning failed or returned empty plan";
			} else if (prev_state == State::GENERATING_BT || prev_state == State::WAITING_FIX_BT || prev_state == State::EXECUTING_BT) {
				failure_cause = "BT generation/fix/execution failed";
			} else {
				failure_cause = "Mission failed";
			}
		}

		if (prev_state == State::WAITING_PLAN || prev_state == State::WAITING_REPLAN) {
			record_plan_failure_case(failure_cause);
		} else if (prev_state == State::GENERATING_BT || prev_state == State::WAITING_FIX_BT) {
			record_bt_failure_case(failure_cause);
		}
	}
}

std::string MCPLLMPlanOrchestrator::now_iso_utc() const
{
	const auto now = std::chrono::system_clock::now();
	const std::time_t tt = std::chrono::system_clock::to_time_t(now);
	std::tm utc_tm{};
#ifdef _WIN32
	gmtime_s(&utc_tm, &tt);
#else
	gmtime_r(&tt, &utc_tm);
#endif
	std::ostringstream oss;
	oss << std::put_time(&utc_tm, "%Y-%m-%dT%H:%M:%SZ");
	return oss.str();
}

std::string MCPLLMPlanOrchestrator::json_escape(const std::string & input) const
{
	std::ostringstream ss;
	for (const char c : input) {
		switch (c) {
			case '"': ss << "\\\""; break;
			case '\\': ss << "\\\\"; break;
			case '\b': ss << "\\b"; break;
			case '\f': ss << "\\f"; break;
			case '\n': ss << "\\n"; break;
			case '\r': ss << "\\r"; break;
			case '\t': ss << "\\t"; break;
			default:
				if (static_cast<unsigned char>(c) < 0x20) {
					ss << "\\u"
						 << std::hex << std::setw(4) << std::setfill('0')
						 << static_cast<int>(static_cast<unsigned char>(c));
				} else {
					ss << c;
				}
		}
	}
	return ss.str();
}

void MCPLLMPlanOrchestrator::write_mission_snapshot(bool accepted, const std::string & message)
{
	std::ofstream out(mission_snapshot_path_);
	if (!out.is_open()) {
		RCLCPP_WARN(get_logger(), "Could not write mission snapshot: %s", mission_snapshot_path_.c_str());
		return;
	}

	out << "{\n";
	out << "  \"generated_at\": \"" << json_escape(now_iso_utc()) << "\",\n";
	out << "  \"mission_name\": \"" << json_escape(mission_name_snapshot_) << "\",\n";
	out << "  \"goal\": \"" << json_escape(goal_snapshot_) << "\",\n";
	out << "  \"context\": \"" << json_escape(context_snapshot_) << "\",\n";
	out << "  \"state\": \"" << json_escape(state_snapshot_) << "\",\n";
	out << "  \"last_event\": \"" << json_escape(last_event_) << "\",\n";
	out << "  \"accepted\": " << (accepted ? "true" : "false") << ",\n";
	out << "  \"message\": \"" << json_escape(message) << "\"\n";
	out << "}\n";
}

void MCPLLMPlanOrchestrator::write_failure_history()
{
	std::ofstream out(failure_history_path_);
	if (!out.is_open()) {
		RCLCPP_WARN(get_logger(), "Could not write failure history: %s", failure_history_path_.c_str());
		return;
	}

	out << "[\n";
	for (std::size_t i = 0; i < failure_history_cache_.size(); ++i) {
		const auto & e = failure_history_cache_[i];
		out << "  {\n";
		out << "    \"timestamp\": \"" << json_escape(e.timestamp) << "\",\n";
		out << "    \"reason\": \"" << json_escape(e.reason) << "\",\n";
		out << "    \"state\": \"" << json_escape(e.state) << "\"\n";
		out << "  }";
		if (i + 1 < failure_history_cache_.size()) {
			out << ",";
		}
		out << "\n";
	}
	out << "]\n";
}

void MCPLLMPlanOrchestrator::write_plan_failure_case(const PlanFailureCase & case_entry)
{
	plan_failure_case_base_cache_.push_back(case_entry);
	if (plan_failure_case_base_cache_.size() > 300) {
		plan_failure_case_base_cache_.erase(plan_failure_case_base_cache_.begin());
	}

	std::ofstream out(plan_failure_case_base_path_);
	if (!out.is_open()) {
		RCLCPP_WARN(get_logger(), "Could not write plan failure episodic memory: %s", plan_failure_case_base_path_.c_str());
		return;
	}

	out << "[\n";
	for (std::size_t i = 0; i < plan_failure_case_base_cache_.size(); ++i) {
		const auto & c = plan_failure_case_base_cache_[i];
		out << "  {\n";
		out << "    \"timestamp\": \"" << json_escape(c.timestamp) << "\",\n";
		out << "    \"mission_goal\": \"" << json_escape(c.mission_goal) << "\",\n";
		out << "    \"failure_cause\": \"" << json_escape(c.failure_cause) << "\",\n";
		out << "    \"state\": \"" << json_escape(c.state) << "\",\n";
		out << "    \"duration_sec\": " << c.duration_sec << ",\n";
		out << "    \"status\": \"" << json_escape(c.status) << "\"\n";
		out << "  }";
		if (i + 1 < plan_failure_case_base_cache_.size()) {
			out << ",";
		}
		out << "\n";
	}
	out << "]\n";
}

void MCPLLMPlanOrchestrator::write_bt_failure_case(const BTFailureCase & case_entry)
{
	bt_failure_case_base_cache_.push_back(case_entry);
	if (bt_failure_case_base_cache_.size() > 300) {
		bt_failure_case_base_cache_.erase(bt_failure_case_base_cache_.begin());
	}

	std::ofstream out(bt_failure_case_base_path_);
	if (!out.is_open()) {
		RCLCPP_WARN(get_logger(), "Could not write BT failure episodic memory: %s", bt_failure_case_base_path_.c_str());
		return;
	}

	out << "[\n";
	for (std::size_t i = 0; i < bt_failure_case_base_cache_.size(); ++i) {
		const auto & c = bt_failure_case_base_cache_[i];
		out << "  {\n";
		out << "    \"timestamp\": \"" << json_escape(c.timestamp) << "\",\n";
		out << "    \"mission_goal\": \"" << json_escape(c.mission_goal) << "\",\n";
		out << "    \"step_goal\": \"" << json_escape(c.step_goal) << "\",\n";
		out << "    \"step_id\": " << c.step_id << ",\n";
		out << "    \"bt_xml\": \"" << json_escape(c.bt_xml) << "\",\n";
		out << "    \"failure_cause\": \"" << json_escape(c.failure_cause) << "\",\n";
		out << "    \"state\": \"" << json_escape(c.state) << "\",\n";
		out << "    \"duration_sec\": " << c.duration_sec << ",\n";
		out << "    \"status\": \"" << json_escape(c.status) << "\"\n";
		out << "  }";
		if (i + 1 < bt_failure_case_base_cache_.size()) {
			out << ",";
		}
		out << "\n";
	}
	out << "]\n";
}

void MCPLLMPlanOrchestrator::record_plan_failure_case(const std::string & failure_cause)
{
	const auto now_steady = std::chrono::steady_clock::now();
	PlanFailureCase case_entry;
	case_entry.timestamp = now_iso_utc();
	case_entry.mission_goal = goal_snapshot_;
	case_entry.failure_cause = failure_cause;
	case_entry.state = state_snapshot_;
	case_entry.duration_sec = std::chrono::duration_cast<std::chrono::seconds>(
		now_steady - execution_start_time_).count();
	if (case_entry.duration_sec < 0) {
		case_entry.duration_sec = 0;
	}
	case_entry.status = "failure";

	write_plan_failure_case(case_entry);
}

void MCPLLMPlanOrchestrator::record_bt_failure_case(const std::string & failure_cause)
{
	BTFailureCase case_entry;
	case_entry.timestamp = now_iso_utc();
	case_entry.mission_goal = goal_snapshot_;
	case_entry.step_id = static_cast<int>(current_step_);
	case_entry.step_goal = (current_step_ < steps_.size()) ? steps_[current_step_].description : "";
	if (case_entry.step_goal.empty()) {
		case_entry.step_goal = "step_" + std::to_string(case_entry.step_id);
	}
	case_entry.bt_xml = last_bt_xml_;
	case_entry.failure_cause = failure_cause;
	case_entry.state = state_snapshot_;
	case_entry.duration_sec = static_cast<int>((now() - step_start_time_).seconds());
	if (case_entry.duration_sec < 0) {
		case_entry.duration_sec = 0;
	}
	case_entry.status = "failure";

	write_bt_failure_case(case_entry);
}

void MCPLLMPlanOrchestrator::write_plan_case(const PlanCase & case_entry)
{
	plan_case_base_cache_.push_back(case_entry);
	if (plan_case_base_cache_.size() > 100) {
		plan_case_base_cache_.erase(plan_case_base_cache_.begin());
	}

	std::ofstream out(plan_case_base_path_);
	if (!out.is_open()) {
		RCLCPP_WARN(get_logger(), "Could not write plan case base: %s", plan_case_base_path_.c_str());
		return;
	}

	out << "[\n";
	for (std::size_t i = 0; i < plan_case_base_cache_.size(); ++i) {
		const auto & c = plan_case_base_cache_[i];
		out << "  {\n";
		out << "    \"timestamp\": \"" << json_escape(c.timestamp) << "\",\n";
		out << "    \"goal\": \"" << json_escape(c.goal) << "\",\n";
		out << "    \"context\": \"" << json_escape(c.context) << "\",\n";
		out << "    \"plan_yaml\": \"" << json_escape(c.plan_yaml) << "\",\n";
		out << "    \"duration_sec\": " << c.duration_sec << ",\n";
		out << "    \"status\": \"" << json_escape(c.status) << "\"\n";
		out << "  }";
		if (i + 1 < plan_case_base_cache_.size()) {
			out << ",";
		}
		out << "\n";
	}
	out << "]\n";
}

void MCPLLMPlanOrchestrator::write_bt_case(const BTCase & case_entry)
{
	bt_case_base_cache_.push_back(case_entry);
	if (bt_case_base_cache_.size() > 100) {
		bt_case_base_cache_.erase(bt_case_base_cache_.begin());
	}

	std::ofstream out(bt_case_base_path_);
	if (!out.is_open()) {
		RCLCPP_WARN(get_logger(), "Could not write BT case base: %s", bt_case_base_path_.c_str());
		return;
	}

	out << "[\n";
	for (std::size_t i = 0; i < bt_case_base_cache_.size(); ++i) {
		const auto & c = bt_case_base_cache_[i];
		out << "  {\n";
		out << "    \"timestamp\": \"" << json_escape(c.timestamp) << "\",\n";
		out << "    \"mission_goal\": \"" << json_escape(c.mission_goal) << "\",\n";
		out << "    \"step_goal\": \"" << json_escape(c.step_goal) << "\",\n";
		out << "    \"step_id\": " << c.step_id << ",\n";
		out << "    \"bt_xml\": \"" << json_escape(c.bt_xml) << "\",\n";
		out << "    \"duration_sec\": " << c.duration_sec << ",\n";
		out << "    \"status\": \"" << json_escape(c.status) << "\"\n";
		out << "  }";
		if (i + 1 < bt_case_base_cache_.size()) {
			out << ",";
		}
		out << "\n";
	}
	out << "]\n";
}

void MCPLLMPlanOrchestrator::record_successful_plan(const std::string & plan_yaml)
{
	const auto now = std::chrono::steady_clock::now();
	const int duration = std::chrono::duration_cast<std::chrono::seconds>(
		now - execution_start_time_).count();

	PlanCase case_entry;
	case_entry.timestamp = now_iso_utc();
	case_entry.goal = goal_snapshot_;
	case_entry.context = context_snapshot_;
	case_entry.plan_yaml = plan_yaml;
	case_entry.duration_sec = duration;
	case_entry.status = "success";

	write_plan_case(case_entry);
	RCLCPP_INFO(get_logger(), "Recorded successful plan: %s (duration %d sec)", 
		goal_snapshot_.c_str(), duration);
}

void MCPLLMPlanOrchestrator::record_successful_bt(
	int step_id,
	const std::string & step_goal,
	const std::string & bt_xml)
{
	const int duration = static_cast<int>((now() - step_start_time_).seconds());

	BTCase case_entry;
	case_entry.timestamp = now_iso_utc();
	case_entry.mission_goal = goal_snapshot_;
	case_entry.step_goal = step_goal;
	case_entry.step_id = step_id;
	case_entry.bt_xml = bt_xml;
	case_entry.duration_sec = duration < 0 ? 0 : duration;
	case_entry.status = "success";

	write_bt_case(case_entry);
	RCLCPP_INFO(get_logger(), "Recorded successful BT: step %d (duration %d sec)", 
		step_id, duration);
}

void MCPLLMPlanOrchestrator::load_plan_case_base()
{
	plan_case_base_cache_.clear();
	
	try {
		if (!std::filesystem::exists(plan_case_base_path_)) {
			RCLCPP_DEBUG(get_logger(), "Plan case base file does not exist: %s", 
				plan_case_base_path_.c_str());
			return;
		}

		if (std::filesystem::file_size(plan_case_base_path_) == 0) {
			RCLCPP_DEBUG(get_logger(), "Plan case base file is empty: %s", 
				plan_case_base_path_.c_str());
			return;
		}

		std::ifstream in(plan_case_base_path_);
		if (!in.is_open()) {
			RCLCPP_WARN(get_logger(), "Could not open plan case base file: %s", 
				plan_case_base_path_.c_str());
			return;
		}

		std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		in.close();

		if (content.empty() || content[0] != '[') {
			RCLCPP_WARN(get_logger(), "Plan case base file has invalid format (not JSON array)");
			return;
		}

		// Simple JSON array parsing for case objects
		std::size_t pos = 0;
		std::size_t obj_start = content.find('{', pos);
		std::size_t obj_end = 0;
		int cases_loaded = 0;
		
		while (obj_start != std::string::npos) {
			try {
				int brace_count = 0;
				obj_end = obj_start;
				
				while ((int)obj_end < (int)content.length() && brace_count >= 0) {
					if (content[obj_end] == '{') brace_count++;
					if (content[obj_end] == '}') brace_count--;
					if (brace_count == 0) break;
					obj_end++;
				}
				
				if (brace_count != 0 || (int)obj_end >= (int)content.length()) {
					break;  // Malformed JSON object
				}
				
				std::string obj_str = content.substr(obj_start, obj_end - obj_start + 1);
				
				PlanCase case_entry;
				
				// Extract fields using simple string search
				auto extract_value = [&obj_str](const std::string & key) -> std::string {
					std::string search = "\"" + key + "\":";
					auto pos = obj_str.find(search);
					if (pos == std::string::npos) return "";
					
					pos += search.length();
					// Skip whitespace
					while (pos < obj_str.length() && std::isspace(obj_str[pos])) pos++;
					
					if (obj_str[pos] == '"') {
						// String value
						pos++;
						std::string value;
						while (pos < obj_str.length() && obj_str[pos] != '"') {
							if (obj_str[pos] == '\\' && pos + 1 < obj_str.length()) {
								pos++;  // Skip escape char
							}
							value += obj_str[pos++];
						}
						return value;
					} else if (std::isdigit(obj_str[pos]) || obj_str[pos] == '-') {
						// Numeric value
						std::string value;
						while ((int)pos < (int)obj_str.length() && (std::isdigit(obj_str[pos]) || obj_str[pos] == '-')) {
							value += obj_str[pos++];
						}
						return value;
					}
					return "";
				};
				
				case_entry.timestamp = extract_value("timestamp");
				case_entry.goal = extract_value("goal");
				case_entry.context = extract_value("context");
				case_entry.plan_yaml = extract_value("plan_yaml");
				case_entry.status = extract_value("status");
				
				std::string duration_str = extract_value("duration_sec");
				try {
					case_entry.duration_sec = std::stoi(duration_str);
				} catch (...) {
					case_entry.duration_sec = 0;
				}
				
				// Validate essential fields before adding
				if (!case_entry.goal.empty() && !case_entry.plan_yaml.empty() && 
				    case_entry.status == "success") {
					plan_case_base_cache_.push_back(case_entry);
					cases_loaded++;
				}
			} catch (...) {
				// Skip malformed case, continue with next
				RCLCPP_DEBUG(get_logger(), "Skipped malformed plan case object");
			}
			
			pos = obj_end + 1;
			obj_start = content.find('{', pos);
		}
		
		if (cases_loaded > 0) {
			RCLCPP_INFO(get_logger(), "Loaded %d plan cases from: %s", 
				cases_loaded, plan_case_base_path_.c_str());
		} else {
			RCLCPP_DEBUG(get_logger(), "No valid plan cases found in: %s", 
				plan_case_base_path_.c_str());
		}
		
	} catch (const std::exception & e) {
		RCLCPP_WARN(get_logger(), "Error loading plan case base: %s", e.what());
		plan_case_base_cache_.clear();
	} catch (...) {
		RCLCPP_WARN(get_logger(), "Unknown error loading plan case base");
		plan_case_base_cache_.clear();
	}
}

void MCPLLMPlanOrchestrator::load_plan_failure_case_base()
{
	plan_failure_case_base_cache_.clear();

	try {
		if (!std::filesystem::exists(plan_failure_case_base_path_)) {
			RCLCPP_DEBUG(get_logger(), "Plan failure episodic memory file does not exist: %s",
				plan_failure_case_base_path_.c_str());
			return;
		}

		if (std::filesystem::file_size(plan_failure_case_base_path_) == 0) {
			RCLCPP_DEBUG(get_logger(), "Plan failure episodic memory file is empty: %s",
				plan_failure_case_base_path_.c_str());
			return;
		}

		std::ifstream in(plan_failure_case_base_path_);
		if (!in.is_open()) {
			RCLCPP_WARN(get_logger(), "Could not open plan failure episodic memory file: %s",
				plan_failure_case_base_path_.c_str());
			return;
		}

		std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		in.close();

		if (content.empty() || content[0] != '[') {
			RCLCPP_WARN(get_logger(), "Plan failure episodic memory has invalid format (not JSON array)");
			return;
		}

		std::size_t pos = 0;
		std::size_t obj_start = content.find('{', pos);
		std::size_t obj_end = 0;
		int cases_loaded = 0;

		while (obj_start != std::string::npos) {
			try {
				int brace_count = 0;
				obj_end = obj_start;
				while ((int)obj_end < (int)content.length() && brace_count >= 0) {
					if (content[obj_end] == '{') brace_count++;
					if (content[obj_end] == '}') brace_count--;
					if (brace_count == 0) break;
					obj_end++;
				}
				if (brace_count != 0 || (int)obj_end >= (int)content.length()) {
					break;
				}

				std::string obj_str = content.substr(obj_start, obj_end - obj_start + 1);
				PlanFailureCase case_entry;
				auto extract_value = [&obj_str](const std::string & key) -> std::string {
					std::string search = "\"" + key + "\":";
					auto p = obj_str.find(search);
					if (p == std::string::npos) return "";
					p += search.length();
					while (p < obj_str.length() && std::isspace(obj_str[p])) p++;
					if (p < obj_str.length() && obj_str[p] == '"') {
						p++;
						std::string value;
						while (p < obj_str.length() && obj_str[p] != '"') {
							if (obj_str[p] == '\\' && p + 1 < obj_str.length()) p++;
							value += obj_str[p++];
						}
						return value;
					} else if (p < obj_str.length() && (std::isdigit(obj_str[p]) || obj_str[p] == '-')) {
						std::string value;
						while ((int)p < (int)obj_str.length() && (std::isdigit(obj_str[p]) || obj_str[p] == '-')) {
							value += obj_str[p++];
						}
						return value;
					}
					return "";
				};

				case_entry.timestamp = extract_value("timestamp");
				case_entry.mission_goal = extract_value("mission_goal");
				if (case_entry.mission_goal.empty()) case_entry.mission_goal = extract_value("goal");
				case_entry.failure_cause = extract_value("failure_cause");
				if (case_entry.failure_cause.empty()) case_entry.failure_cause = extract_value("reason");
				case_entry.state = extract_value("state");
				case_entry.status = extract_value("status");
				try { case_entry.duration_sec = std::stoi(extract_value("duration_sec")); }
				catch (...) { case_entry.duration_sec = 0; }

				if (!case_entry.mission_goal.empty() && !case_entry.failure_cause.empty()) {
					if (case_entry.status.empty()) case_entry.status = "failure";
					plan_failure_case_base_cache_.push_back(case_entry);
					cases_loaded++;
				}
			} catch (...) {
				RCLCPP_DEBUG(get_logger(), "Skipped malformed plan failure case object");
			}
			pos = obj_end + 1;
			obj_start = content.find('{', pos);
		}

		if (cases_loaded > 0) {
			RCLCPP_INFO(get_logger(), "Loaded %d plan failure cases from: %s", cases_loaded, plan_failure_case_base_path_.c_str());
		}
	} catch (const std::exception & e) {
		RCLCPP_WARN(get_logger(), "Error loading plan failure episodic memory: %s", e.what());
		plan_failure_case_base_cache_.clear();
	} catch (...) {
		RCLCPP_WARN(get_logger(), "Unknown error loading plan failure episodic memory");
		plan_failure_case_base_cache_.clear();
	}
}

void MCPLLMPlanOrchestrator::load_bt_failure_case_base()
{
	bt_failure_case_base_cache_.clear();

	try {
		if (!std::filesystem::exists(bt_failure_case_base_path_)) {
			RCLCPP_DEBUG(get_logger(), "BT failure episodic memory file does not exist: %s", bt_failure_case_base_path_.c_str());
			return;
		}
		if (std::filesystem::file_size(bt_failure_case_base_path_) == 0) {
			RCLCPP_DEBUG(get_logger(), "BT failure episodic memory file is empty: %s", bt_failure_case_base_path_.c_str());
			return;
		}

		std::ifstream in(bt_failure_case_base_path_);
		if (!in.is_open()) {
			RCLCPP_WARN(get_logger(), "Could not open BT failure episodic memory file: %s", bt_failure_case_base_path_.c_str());
			return;
		}
		std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		in.close();
		if (content.empty() || content[0] != '[') {
			RCLCPP_WARN(get_logger(), "BT failure episodic memory has invalid format (not JSON array)");
			return;
		}

		std::size_t pos = 0;
		std::size_t obj_start = content.find('{', pos);
		std::size_t obj_end = 0;
		int cases_loaded = 0;
		while (obj_start != std::string::npos) {
			try {
				int brace_count = 0;
				obj_end = obj_start;
				while ((int)obj_end < (int)content.length() && brace_count >= 0) {
					if (content[obj_end] == '{') brace_count++;
					if (content[obj_end] == '}') brace_count--;
					if (brace_count == 0) break;
					obj_end++;
				}
				if (brace_count != 0 || (int)obj_end >= (int)content.length()) break;

				std::string obj_str = content.substr(obj_start, obj_end - obj_start + 1);
				BTFailureCase case_entry;
				auto extract_value = [&obj_str](const std::string & key) -> std::string {
					std::string search = "\"" + key + "\":";
					auto p = obj_str.find(search);
					if (p == std::string::npos) return "";
					p += search.length();
					while (p < obj_str.length() && std::isspace(obj_str[p])) p++;
					if (p < obj_str.length() && obj_str[p] == '"') {
						p++;
						std::string value;
						while (p < obj_str.length() && obj_str[p] != '"') {
							if (obj_str[p] == '\\' && p + 1 < obj_str.length()) p++;
							value += obj_str[p++];
						}
						return value;
					} else if (p < obj_str.length() && (std::isdigit(obj_str[p]) || obj_str[p] == '-')) {
						std::string value;
						while ((int)p < (int)obj_str.length() && (std::isdigit(obj_str[p]) || obj_str[p] == '-')) value += obj_str[p++];
						return value;
					}
					return "";
				};

				case_entry.timestamp = extract_value("timestamp");
				case_entry.mission_goal = extract_value("mission_goal");
				if (case_entry.mission_goal.empty()) case_entry.mission_goal = extract_value("goal");
				case_entry.step_goal = extract_value("step_goal");
				if (case_entry.step_goal.empty()) case_entry.step_goal = "step_";
				case_entry.bt_xml = extract_value("bt_xml");
				case_entry.failure_cause = extract_value("failure_cause");
				if (case_entry.failure_cause.empty()) case_entry.failure_cause = extract_value("reason");
				case_entry.state = extract_value("state");
				case_entry.status = extract_value("status");
				try { case_entry.step_id = std::stoi(extract_value("step_id")); }
				catch (...) { case_entry.step_id = 0; }
				if (case_entry.step_goal == "step_") case_entry.step_goal += std::to_string(case_entry.step_id);
				try { case_entry.duration_sec = std::stoi(extract_value("duration_sec")); }
				catch (...) { case_entry.duration_sec = 0; }

				if (!case_entry.mission_goal.empty() && !case_entry.failure_cause.empty()) {
					if (case_entry.status.empty()) case_entry.status = "failure";
					bt_failure_case_base_cache_.push_back(case_entry);
					cases_loaded++;
				}
			} catch (...) {
				RCLCPP_DEBUG(get_logger(), "Skipped malformed BT failure case object");
			}
			pos = obj_end + 1;
			obj_start = content.find('{', pos);
		}

		if (cases_loaded > 0) {
			RCLCPP_INFO(get_logger(), "Loaded %d BT failure cases from: %s", cases_loaded, bt_failure_case_base_path_.c_str());
		}
	} catch (const std::exception & e) {
		RCLCPP_WARN(get_logger(), "Error loading BT failure episodic memory: %s", e.what());
		bt_failure_case_base_cache_.clear();
	} catch (...) {
		RCLCPP_WARN(get_logger(), "Unknown error loading BT failure episodic memory");
		bt_failure_case_base_cache_.clear();
	}
}

void MCPLLMPlanOrchestrator::load_bt_case_base()
{
	bt_case_base_cache_.clear();
	
	try {
		if (!std::filesystem::exists(bt_case_base_path_)) {
			RCLCPP_DEBUG(get_logger(), "BT case base file does not exist: %s", 
				bt_case_base_path_.c_str());
			return;
		}

		if (std::filesystem::file_size(bt_case_base_path_) == 0) {
			RCLCPP_DEBUG(get_logger(), "BT case base file is empty: %s", 
				bt_case_base_path_.c_str());
			return;
		}

		std::ifstream in(bt_case_base_path_);
		if (!in.is_open()) {
			RCLCPP_WARN(get_logger(), "Could not open BT case base file: %s", 
				bt_case_base_path_.c_str());
			return;
		}

		std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		in.close();

		if (content.empty() || content[0] != '[') {
			RCLCPP_WARN(get_logger(), "BT case base file has invalid format (not JSON array)");
			return;
		}

		std::size_t pos = 0;
		std::size_t obj_start = content.find('{', pos);
		std::size_t obj_end = 0;
		int cases_loaded = 0;
		
		while (obj_start != std::string::npos) {
			try {
				int brace_count = 0;
				obj_end = obj_start;
				
				while ((int)obj_end < (int)content.length() && brace_count >= 0) {
					if (content[obj_end] == '{') brace_count++;
					if (content[obj_end] == '}') brace_count--;
					if (brace_count == 0) break;
					obj_end++;
				}
				
				if (brace_count != 0 || (int)obj_end >= (int)content.length()) {
					break;
				}
				
				std::string obj_str = content.substr(obj_start, obj_end - obj_start + 1);
				
				BTCase case_entry;
				
				auto extract_value = [&obj_str](const std::string & key) -> std::string {
					std::string search = "\"" + key + "\":";
					auto pos = obj_str.find(search);
					if (pos == std::string::npos) return "";
					
					pos += search.length();
					while (pos < obj_str.length() && std::isspace(obj_str[pos])) pos++;
					
					if (obj_str[pos] == '"') {
						pos++;
						std::string value;
						while (pos < obj_str.length() && obj_str[pos] != '"') {
							if (obj_str[pos] == '\\' && pos + 1 < obj_str.length()) {
								pos++;
							}
							value += obj_str[pos++];
						}
						return value;
					} else if (std::isdigit(obj_str[pos]) || obj_str[pos] == '-') {
						std::string value;
						while ((int)pos < (int)obj_str.length() && (std::isdigit(obj_str[pos]) || obj_str[pos] == '-')) {
							value += obj_str[pos++];
						}
						return value;
					}
					return "";
				};
				
				case_entry.timestamp = extract_value("timestamp");
				case_entry.mission_goal = extract_value("mission_goal");
				if (case_entry.mission_goal.empty()) {
					case_entry.mission_goal = extract_value("goal");
				}
				case_entry.step_goal = extract_value("step_goal");
				if (case_entry.step_goal.empty()) {
					case_entry.step_goal = extract_value("bt_goal");
				}
				case_entry.bt_xml = extract_value("bt_xml");
				case_entry.status = extract_value("status");
				
				std::string step_id_str = extract_value("step_id");
				try {
					case_entry.step_id = std::stoi(step_id_str);
				} catch (...) {
					case_entry.step_id = 0;
				}
				if (case_entry.step_goal.empty()) {
					case_entry.step_goal = "step_" + std::to_string(case_entry.step_id);
				}
				
				std::string duration_str = extract_value("duration_sec");
				try {
					case_entry.duration_sec = std::stoi(duration_str);
				} catch (...) {
					case_entry.duration_sec = 0;
				}
				
				if (!case_entry.mission_goal.empty() && !case_entry.step_goal.empty() && !case_entry.bt_xml.empty() && 
				    case_entry.status == "success") {
					bt_case_base_cache_.push_back(case_entry);
					cases_loaded++;
				}
			} catch (...) {
				RCLCPP_DEBUG(get_logger(), "Skipped malformed BT case object");
			}
			
			pos = obj_end + 1;
			obj_start = content.find('{', pos);
		}
		
		if (cases_loaded > 0) {
			RCLCPP_INFO(get_logger(), "Loaded %d BT cases from: %s", 
				cases_loaded, bt_case_base_path_.c_str());
		} else {
			RCLCPP_DEBUG(get_logger(), "No valid BT cases found in: %s", 
				bt_case_base_path_.c_str());
		}
		
	} catch (const std::exception & e) {
		RCLCPP_WARN(get_logger(), "Error loading BT case base: %s", e.what());
		bt_case_base_cache_.clear();
	} catch (...) {
		RCLCPP_WARN(get_logger(), "Unknown error loading BT case base");
		bt_case_base_cache_.clear();
	}
}

}  // namespace behavior_architecture
