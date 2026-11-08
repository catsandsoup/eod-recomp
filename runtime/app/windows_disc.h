#pragma once
#include "disc_import.h"
// Normalize Windows UTF-16 arguments to the UTF-8 runtime path convention.
void windows_utf8_arguments(int &argc, char **&argv);
bool windows_prepare_disc(std::string &root, const std::string &exe_dir, bool headless, const DiscId &id);
