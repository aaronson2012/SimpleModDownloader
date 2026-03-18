#pragma once

#include <vector>
#include <string>
#include <ctime>

namespace utils {
    std::vector<std::pair<std::string, std::string>> getInstalledGames();
    uint8_t* getIconFromTitleId(const std::string& titleId);
    std::string sanitizeTitle(const std::string& title);
    bool isValidGameTitle(const std::string& title);
    std::string debugStringPreview(const std::string& value, size_t maxLength = 64);
    std::string removeHtmlTags(const std::string& str);
    std::string getModInstallPath();
    std::string timestamp_to_date(time_t timestamp);
    std::string file_size_to_string(int file_size);
}
