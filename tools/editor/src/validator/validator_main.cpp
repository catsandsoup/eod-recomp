#include "schema_validator.h"
#include <iostream>
#include <string>
#include <filesystem>

namespace fs = std::filesystem;
using namespace eod::editor;

void PrintUsage(const char* exe_name) {
    std::cout << "Usage: " << exe_name << " [options]\n"
              << "Options:\n"
              << "  --project <dir>    Path to mod project directory\n"
              << "  --mod-dir <dir>    Alias for --project\n"
              << "  --file <file>      Validate single manifest file\n"
              << "  --strict           Treat warnings as errors\n"
              << "  --self-test        Run built-in validation suite\n"
              << "  --help, -h         Show this help information\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        PrintUsage(argv[0]);
        return 1;
    }

    std::string project_dir;
    std::string single_file;
    bool strict = false;
    bool self_test = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--project" || arg == "--mod-dir") {
            if (i + 1 < argc) {
                project_dir = argv[++i];
            } else {
                std::cerr << "Error: " << arg << " requires directory argument\n";
                return 1;
            }
        } else if (arg == "--file") {
            if (i + 1 < argc) {
                single_file = argv[++i];
            } else {
                std::cerr << "Error: --file requires file path argument\n";
                return 1;
            }
        } else if (arg == "--strict") {
            strict = true;
        } else if (arg == "--self-test") {
            self_test = true;
        } else if (arg == "--help" || arg == "-h") {
            PrintUsage(argv[0]);
            return 0;
        } else if (project_dir.empty() && fs::is_directory(arg)) {
            project_dir = arg;
        } else if (single_file.empty() && fs::is_regular_file(arg)) {
            single_file = arg;
        }
    }

    if (self_test) {
        std::cout << "[INFO] Running eod_mod_validator self-test...\n";
        bool passed = SchemaValidator::RunSelfTest();
        if (passed) {
            std::cout << "[SUCCESS] Self-test passed cleanly.\n";
            return 0;
        } else {
            std::cerr << "[FAILURE] Self-test reported unexpected validation failures.\n";
            return 2;
        }
    }

    SchemaValidator validator;
    ValidationReport report;

    if (!single_file.empty()) {
        std::string base_dir = project_dir.empty() ? fs::path(single_file).parent_path().string() : project_dir;
        std::cout << "[INFO] Validating manifest file: " << single_file << "\n";
        validator.ValidateFile(single_file, base_dir, report);
    } else if (!project_dir.empty()) {
        std::cout << "[INFO] Validating mod project at: " << project_dir << "\n";
        validator.ValidateModProject(project_dir, report);
    } else {
        std::cerr << "Error: No project directory or manifest file specified.\n";
        PrintUsage(argv[0]);
        return 1;
    }

    report.Print(std::cout);

    if (!report.IsValid()) {
        std::cerr << "\n[FAILED] Validation encountered " << report.GetErrorCount() << " error(s).\n";
        return 1;
    }

    if (strict && report.GetWarningCount() > 0) {
        std::cerr << "\n[FAILED] Strict mode enabled: " << report.GetWarningCount() << " warning(s) treated as errors.\n";
        return 1;
    }

    std::cout << "\n[PASSED] Mod package is structurally valid and ready for runtime mounting.\n";
    return 0;
}
