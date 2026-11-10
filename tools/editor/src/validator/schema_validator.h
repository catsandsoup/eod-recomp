#pragma once

#include <string>
#include <vector>
#include <iostream>
#include "../third_party/nlohmann/json.hpp"

namespace eod::editor {

enum class ValidationSeverity {
    Info,
    Warning,
    Error
};

struct ValidationIssue {
    ValidationSeverity severity;
    std::string rule_id;
    std::string file_path;
    std::string json_pointer;
    std::string message;
};

class ValidationReport {
public:
    void AddError(std::string rule_id, std::string file_path, std::string json_pointer, std::string message);
    void AddWarning(std::string rule_id, std::string file_path, std::string json_pointer, std::string message);
    void AddInfo(std::string rule_id, std::string file_path, std::string json_pointer, std::string message);

    bool IsValid() const { return error_count_ == 0; }
    size_t GetErrorCount() const { return error_count_; }
    size_t GetWarningCount() const { return warning_count_; }
    size_t GetInfoCount() const { return info_count_; }
    const std::vector<ValidationIssue>& GetIssues() const { return issues_; }
    void Clear();

    void Print(std::ostream& os) const;

private:
    std::vector<ValidationIssue> issues_;
    size_t error_count_ = 0;
    size_t warning_count_ = 0;
    size_t info_count_ = 0;
};

class SchemaValidator {
public:
    SchemaValidator() = default;

    // Direct manifest object validation
    bool ValidateVehicleManifest(const nlohmann::json& j,
                                const std::string& manifest_path,
                                const std::string& base_dir,
                                ValidationReport& report) const;

    bool ValidateTrackManifest(const nlohmann::json& j,
                              const std::string& manifest_path,
                              const std::string& base_dir,
                              ValidationReport& report) const;

    bool ValidateEventManifest(const nlohmann::json& j,
                              const std::string& manifest_path,
                              const std::string& base_dir,
                              ValidationReport& report) const;

    bool ValidateProjectileManifest(const nlohmann::json& j,
                                   const std::string& manifest_path,
                                   const std::string& base_dir,
                                   ValidationReport& report) const;

    bool ValidatePlacedProps(const nlohmann::json& j,
                            const std::string& manifest_path,
                            const std::string& base_dir,
                            ValidationReport& report) const;

    bool ValidateSpline(const nlohmann::json& j,
                        const std::string& manifest_path,
                        ValidationReport& report) const;

    // Validates a single JSON manifest file by determining its schema/type
    bool ValidateFile(const std::string& file_path,
                      const std::string& base_dir,
                      ValidationReport& report) const;

    // Full project or mod directory scanning and validation
    bool ValidateModProject(const std::string& project_dir,
                           ValidationReport& report) const;

    // Self-test for automated CTest verification
    static bool RunSelfTest();

private:
    void CheckAssetExists(const std::string& relative_path,
                          const std::string& base_dir,
                          const std::string& manifest_path,
                          const std::string& json_ptr,
                          const std::string& rule_id,
                          ValidationReport& report) const;

    void CheckAudioFormat(const std::string& audio_path,
                          const std::string& manifest_path,
                          const std::string& json_ptr,
                          ValidationReport& report) const;
};

} // namespace eod::editor
