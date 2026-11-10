#include "console_panel.h"
#include "../../third_party/imgui/imgui.h"
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace eod::editor {

ConsolePanel::ConsolePanel() {
    Log(ValidationSeverity::Info, "Test Drive: Eve of Destruction Modding Editor Shell initialized.");
    Log(ValidationSeverity::Info, "Runtime math structures and Dear ImGui docking enabled.");
}

void ConsolePanel::Log(ValidationSeverity severity, const std::string& message) {
    auto now = std::chrono::system_clock::now();
    std::time_t now_c = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf;
#if defined(_WIN32)
    localtime_s(&tm_buf, &now_c);
#else
    localtime_r(&now_c, &tm_buf);
#endif
    std::stringstream ss;
    ss << std::put_time(&tm_buf, "%H:%M:%S");

    entries_.push_back({severity, ss.str(), message});
}

void ConsolePanel::SetValidationReport(const ValidationReport& report) {
    latest_report_ = report;
    std::string summary = "Validation complete: " + std::to_string(report.GetErrorCount()) + " errors, " +
                          std::to_string(report.GetWarningCount()) + " warnings.";
    Log(report.IsValid() ? ValidationSeverity::Info : ValidationSeverity::Error, summary);
}

void ConsolePanel::Render(bool& out_request_validate) {
    if (ImGui::Begin("Console & Validator")) {
        if (ImGui::BeginTabBar("ConsoleTabs")) {
            // Tab 1: Console Log
            if (ImGui::BeginTabItem("Console Log")) {
                if (ImGui::Button("Clear")) {
                    entries_.clear();
                }
                ImGui::SameLine();
                ImGui::Checkbox("Info", &filter_info_);
                ImGui::SameLine();
                ImGui::Checkbox("Warnings", &filter_warn_);
                ImGui::SameLine();
                ImGui::Checkbox("Errors", &filter_error_);
                ImGui::Separator();

                ImGui::BeginChild("LogScrollRegion", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
                for (const auto& entry : entries_) {
                    if (entry.severity == ValidationSeverity::Info && !filter_info_) continue;
                    if (entry.severity == ValidationSeverity::Warning && !filter_warn_) continue;
                    if (entry.severity == ValidationSeverity::Error && !filter_error_) continue;

                    ImVec4 col(0.8f, 0.8f, 0.8f, 1.0f);
                    const char* tag = "[INFO]";
                    if (entry.severity == ValidationSeverity::Warning) {
                        col = ImVec4(1.0f, 0.8f, 0.2f, 1.0f);
                        tag = "[WARN]";
                    } else if (entry.severity == ValidationSeverity::Error) {
                        col = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
                        tag = "[ERROR]";
                    }

                    ImGui::TextColored(col, "[%s] %s %s", entry.timestamp.c_str(), tag, entry.message.c_str());
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            // Tab 2: Schema Validator
            if (ImGui::BeginTabItem("Schema Validator")) {
                if (ImGui::Button("Run Project Validation")) {
                    out_request_validate = true;
                }
                ImGui::SameLine();
                if (latest_report_.IsValid()) {
                    ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.3f, 1.0f), "✓ All manifests valid conforming to MODDING_SUITE_FOUNDATIONS.md");
                } else {
                    ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "✗ %zu Error(s), %zu Warning(s)",
                                       latest_report_.GetErrorCount(), latest_report_.GetWarningCount());
                }
                ImGui::Separator();

                if (ImGui::BeginTable("ValidatorTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
                    ImGui::TableSetupColumn("Severity", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                    ImGui::TableSetupColumn("Rule ID", ImGuiTableColumnFlags_WidthFixed, 140.0f);
                    ImGui::TableSetupColumn("Location", ImGuiTableColumnFlags_WidthFixed, 160.0f);
                    ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableHeadersRow();

                    for (const auto& issue : latest_report_.GetIssues()) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        if (issue.severity == ValidationSeverity::Error) {
                            ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "Error");
                        } else if (issue.severity == ValidationSeverity::Warning) {
                            ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "Warning");
                        } else {
                            ImGui::TextColored(ImVec4(0.5f, 0.8f, 1, 1), "Info");
                        }

                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextUnformatted(issue.rule_id.c_str());

                        ImGui::TableSetColumnIndex(2);
                        std::string loc = issue.file_path;
                        if (!issue.json_pointer.empty()) loc += " (" + issue.json_pointer + ")";
                        ImGui::TextUnformatted(loc.c_str());

                        ImGui::TableSetColumnIndex(3);
                        ImGui::TextUnformatted(issue.message.c_str());
                    }
                    ImGui::EndTable();
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

} // namespace eod::editor
