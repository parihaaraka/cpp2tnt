#pragma once

#include <string>

std::string hex_dump(const char *begin, const char *end, const char *pos = nullptr);

std::string get_trace();
