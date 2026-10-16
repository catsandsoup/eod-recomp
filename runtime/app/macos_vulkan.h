#pragma once
#include <string>
// Configure loader/SDL discovery only when the executable is inside a bundled
// app carrying our ICD. Plain command-line builds preserve the caller's setup.
void mac_app_init_vulkan();
void mac_app_init_vulkan_at(const std::string &executable);
