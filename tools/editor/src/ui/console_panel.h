#pragma once

#include <vector>
#include <string>
#include "../validator/schema_validator.h"

namespace eod::editor {

struct ConsoleEntry {
    ValidationSeverity severity;
    std::string timestamp;
    std::string message;
};

class ConsolePanel {
public:
    ConsolePanel();
    ~ConsolePanel() = default;

    void Log(ValidationSeverity severity, const std::string& message);
    void SetValidationReport(const ValidationReport& report);
    void Render(bool& out_request_validate);

private:
    std::vector<ConsoleEntry> entries_;
    ValidationReport latest_report_;
    bool filter_info_ = true;
    bool filter_warn_ = true;
    bool filter_error_ = true;
};

} // namespace eod::editor
