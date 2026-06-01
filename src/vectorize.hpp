#ifndef VECTORIZE_HPP
#define VECTORIZE_HPP

#include <array>
#include <string>
#include <string_view>
#include <unordered_map>

std::array<float, 14> vectorizeTransaction(std::string_view body);
const std::unordered_map<std::string, float>& getMccRisk();

#endif
