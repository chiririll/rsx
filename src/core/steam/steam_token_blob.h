#pragma once

#include <cstddef>
#include <string>
#include <vector>

// Length-prefixed auth token blob: u32 userLen, user bytes, u32 tokenLen, token bytes.
std::vector<char> SerializeAuthToken(const std::string& username, const std::string& token);
bool DeserializeAuthToken(const void* data, size_t size, std::string& outUsername, std::string& outToken);
