#include "utils/utils.hpp"

#include <switch.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <iomanip>
#include <regex>
#include <cctype>
#include <borealis.hpp>
#include <SimpleIniParser.hpp>


using namespace simpleIniParser;

namespace {
    bool isWhitespace(unsigned char c) {
        switch (c) {
            case ' ':
            case '\t':
            case '\n':
            case '\r':
            case '\f':
            case '\v':
                return true;
            default:
                return false;
        }
    }

    bool isContinuationByte(unsigned char c) {
        return (c & 0xC0) == 0x80;
    }

    size_t utf8SequenceLength(unsigned char lead) {
        if ((lead & 0x80) == 0x00)
            return 1;
        if ((lead & 0xE0) == 0xC0)
            return 2;
        if ((lead & 0xF0) == 0xE0)
            return 3;
        if ((lead & 0xF8) == 0xF0)
            return 4;
        return 0;
    }

    bool isValidUtf8(const std::string& value) {
        for (size_t i = 0; i < value.size(); ++i) {
            unsigned char c = static_cast<unsigned char>(value[i]);
            size_t sequenceLength = utf8SequenceLength(c);

            if (sequenceLength == 0)
                return false;

            if (sequenceLength == 1) {
                if (c == 0x7F)
                    return false;
                continue;
            }

            if (i + sequenceLength > value.size())
                return false;

            for (size_t j = 1; j < sequenceLength; ++j) {
                if (!isContinuationByte(static_cast<unsigned char>(value[i + j])))
                    return false;
            }

            if (sequenceLength == 2 && c < 0xC2)
                return false;

            if (sequenceLength == 3) {
                unsigned char c1 = static_cast<unsigned char>(value[i + 1]);
                if (c == 0xE0 && c1 < 0xA0)
                    return false;
                if (c == 0xED && c1 >= 0xA0)
                    return false;
            }

            if (sequenceLength == 4) {
                unsigned char c1 = static_cast<unsigned char>(value[i + 1]);
                if (c == 0xF0 && c1 < 0x90)
                    return false;
                if (c == 0xF4 && c1 >= 0x90)
                    return false;
                if (c > 0xF4)
                    return false;
            }

            i += sequenceLength - 1;
        }

        return true;
    }

    std::string copyNacpString(const char* value, size_t maxLength) {
        std::string result;
        result.reserve(maxLength);

        for (size_t i = 0; i < maxLength; ++i) {
            unsigned char c = static_cast<unsigned char>(value[i]);

            if (c == 0)
                break;

            if (c < 0x20 || c == 0x7F) {
                if (!result.empty())
                    break;
                continue;
            }

            size_t sequenceLength = utf8SequenceLength(c);
            if (sequenceLength == 0 || i + sequenceLength > maxLength) {
                if (!result.empty())
                    break;
                continue;
            }

            if (sequenceLength == 1) {
                result.push_back(static_cast<char>(c));
                continue;
            }

            bool validSequence = true;
            for (size_t j = 1; j < sequenceLength; ++j) {
                if (!isContinuationByte(static_cast<unsigned char>(value[i + j]))) {
                    validSequence = false;
                    break;
                }
            }

            if (!validSequence) {
                if (!result.empty())
                    break;
                continue;
            }

            result.append(value + i, sequenceLength);
            i += sequenceLength - 1;
        }

        return result;
    }

    bool tryGetValidTitle(const NacpLanguageEntry& entry, const std::string& titleId, int languageIndex, std::string& title) {
        std::string rawTitle = copyNacpString(entry.name, sizeof(entry.name));
        std::string sanitizedTitle = utils::sanitizeTitle(rawTitle);
        bool valid = utils::isValidGameTitle(sanitizedTitle);

        brls::Logger::debug(
            "NACP title candidate tid={} lang={} raw='{}' sanitized='{}' valid={}",
            titleId,
            languageIndex,
            utils::debugStringPreview(rawTitle),
            utils::debugStringPreview(sanitizedTitle),
            valid);

        if (!valid)
            return false;

        title = sanitizedTitle;
        return true;
    }
}

namespace utils {

    Result nacpGetLanguageEntrySpecialLanguage(NacpStruct* nacp, NacpLanguageEntry** langentry, const SetLanguage LanguageChoosen) {
        Result rc=0;
        SetLanguage Language = LanguageChoosen;
        NacpLanguageEntry *entry = NULL;
        u32 i=0;

        if (nacp==NULL || langentry==NULL)
            return MAKERESULT(Module_Libnx, LibnxError_BadInput);

        *langentry = NULL;

        entry = &nacp->lang[SetLanguage_ENUS];

        if (entry->name[0]==0 && entry->author[0]==0) {
            for(i=0; i<16; i++) {
                entry = &nacp->lang[i];
                if (entry->name[0] || entry->author[0]) break;
            }
        }

        if (entry->name[0]==0 && entry->author[0]==0)
            return rc;

        *langentry = entry;

        return rc;
    }


    std::string formatApplicationId(u64 ApplicationId)
    {
        std::stringstream strm;
        strm << std::uppercase << std::setfill('0') << std::setw(16) << std::hex << ApplicationId;
        return strm.str();
    }

    std::string debugStringPreview(const std::string& value, size_t maxLength) {
        std::ostringstream out;
        size_t consumed = 0;

        for (unsigned char c : value) {
            if (consumed >= maxLength) {
                out << "...";
                break;
            }

            switch (c) {
                case '\n':
                    out << "\\n";
                    break;
                case '\r':
                    out << "\\r";
                    break;
                case '\t':
                    out << "\\t";
                    break;
                default:
                    if (c >= 0x20 && c != 0x7F) {
                        out << static_cast<char>(c);
                    } else {
                        out << "\\x" << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c) << std::dec;
                    }
                    break;
            }

            consumed++;
        }

        return out.str();
    }

    std::string sanitizeTitle(const std::string& title) {
        std::string sanitized;
        sanitized.reserve(title.size());

        bool lastWasSpace = true;
        for (unsigned char c : title) {
            if (c == 0)
                break;

            if (c < 0x20 || c == 0x7F) {
                if (isWhitespace(c) && !sanitized.empty() && !lastWasSpace) {
                    sanitized.push_back(' ');
                    lastWasSpace = true;
                }
                continue;
            }

            if (isWhitespace(c)) {
                if (!sanitized.empty() && !lastWasSpace) {
                    sanitized.push_back(' ');
                    lastWasSpace = true;
                }
                continue;
            }

            sanitized.push_back(static_cast<char>(c));
            lastWasSpace = false;
        }

        if (!sanitized.empty() && sanitized.back() == ' ')
            sanitized.pop_back();

        return sanitized;
    }

    bool isValidGameTitle(const std::string& title) {
        std::string sanitized = sanitizeTitle(title);
        if (sanitized.size() < 2)
            return false;

        if (!isValidUtf8(sanitized))
            return false;

        size_t visibleCharacters = 0;
        size_t asciiAlphaNumericCharacters = 0;
        bool allHex = sanitized.size() == 16;

        for (unsigned char c : sanitized) {
            if (c < 0x20 || c == 0x7F)
                return false;

            if (!isWhitespace(c))
                visibleCharacters++;

            if (c < 0x80 && std::isalnum(c))
                asciiAlphaNumericCharacters++;

            if (!std::isxdigit(c))
                allHex = false;
        }

        if (visibleCharacters < 2)
            return false;

        if (allHex)
            return false;

        return asciiAlphaNumericCharacters > 0 || visibleCharacters >= 3;
    }

    std::vector<std::pair<std::string, std::string>> getInstalledGames() {
        std::vector<std::pair<std::string, std::string>> games;

        NsApplicationRecord* records = new NsApplicationRecord[64000]();
        uint64_t tid;
        NsApplicationControlData controlData;
        
        Result rc;
        int recordCount = 0;
        size_t controlSize = 0;

        rc = nsListApplicationRecord(records, 64000, 0, &recordCount);
        brls::Logger::debug("Installed title scan found {} application records", recordCount);
        for (auto i = 0; i < recordCount; ++i) {
            tid = records[i].application_id;
            std::string titleId = formatApplicationId(tid);
            rc = nsGetApplicationControlData(NsApplicationControlSource_Storage, tid, &controlData, sizeof(controlData), &controlSize);
            if (R_FAILED(rc)) {
                brls::Logger::error("Failed to read NACP for tid {}: rc={}", titleId, rc);
                continue; // Ou break je sais pas trop
            }

            std::string appName;
            if (!tryGetValidTitle(controlData.nacp.lang[SetLanguage_ENUS], titleId, SetLanguage_ENUS, appName)) {
                size_t languageCount = sizeof(controlData.nacp.lang) / sizeof(controlData.nacp.lang[0]);
                for (size_t languageIndex = 0; languageIndex < languageCount; ++languageIndex) {
                    if (languageIndex == SetLanguage_ENUS)
                        continue;

                    if (tryGetValidTitle(controlData.nacp.lang[languageIndex], titleId, static_cast<int>(languageIndex), appName))
                        break;
                }
            }

            if (appName.empty()) {
                brls::Logger::error("Skipping installed title {} because no plausible sanitized title was found", titleId);
                continue;
            }

            brls::Logger::debug("Installed title accepted tid={} sanitized='{}'", titleId, debugStringPreview(appName));
            games.emplace_back(appName, titleId);
        }

        delete[] records;

        return games;
    }

    uint8_t* getIconFromTitleId(const std::string& titleId) {
        if(titleId.empty()) return nullptr;

        uint8_t* icon = nullptr;
        NsApplicationControlData controlData;
        size_t controlSize  = 0;
        uint64_t tid;

        std::istringstream buffer(titleId);
        buffer >> std::hex >> tid;

        if (R_FAILED(nsGetApplicationControlData(NsApplicationControlSource_Storage, tid, &controlData, sizeof(controlData), &controlSize))){ return nullptr; }

        icon = new uint8_t[0x20000];
        memcpy(icon, controlData.icon, 0x20000);
        return icon;
    }

    std::string removeHtmlTags(const std::string& input) {
        //Replace <br> by \n
        std::regex brRegex("<br>");
        std::string output = std::regex_replace(input, brRegex, "\n");
        std::regex nbspRegex("&nbsp;");
        output = std::regex_replace(output, nbspRegex, " ");
        std::regex tagsRegex("<.*?>");
        return std::regex_replace(output, tagsRegex, "");
    }

    bool ends_with(const std::string& str, const std::string& suffix) {
        if (suffix.length() > str.length()) {
            return false;
        }
        return str.compare(str.length() - suffix.length(), suffix.length(), suffix) == 0;
    }

    bool starts_with(const std::string& str, const std::string& prefix) {
        if (prefix.length() > str.length()) {
            return false;
        }
        return str.compare(0, prefix.length(), prefix) == 0;
    }

    std::string getModInstallPath() {
        std::filesystem::path path(std::string("sdmc:/config/SimpleModManager/parameters.ini"));
        if(!std::filesystem::exists(path)) {
            return "mods";
        }
        Ini* config = Ini::parseFile(path.string());
        IniOption* path_mods = config->findFirstOption("stored-mods-base-folder");
        std::string pathString = path_mods->value;
        brls::Logger::debug("Mod install path: {}", pathString);
        if(ends_with(pathString, "/"))
            pathString.pop_back();
        if(starts_with(pathString, "/"))
            pathString.erase(0, 1);
        return pathString;
    }

    std::string timestamp_to_date(time_t timestamp) {
        std::tm* timeinfo = std::gmtime(&timestamp);
        std::ostringstream os;
        os << std::setfill('0') << std::setw(2) << timeinfo->tm_mday << "/"
        << std::setfill('0') << std::setw(2) << (timeinfo->tm_mon + 1) << "/"
        << (timeinfo->tm_year + 1900);
        return os.str();
    }

    std::string file_size_to_string(int file_size) {
        const double kb = 1024.0;
        const double mb = std::pow(kb, 2);
        const double gb = std::pow(kb, 3);

        std::ostringstream os;
        if (file_size >= gb) {
            os << std::fixed << std::setprecision(2) << file_size / gb << " GB";
        } else if (file_size >= mb) {
            os << std::fixed << std::setprecision(2) << file_size / mb << " MB";
        } else if (file_size >= kb) {
            os << std::fixed << std::setprecision(2) << file_size / kb << " KB";
        } else {
            os << file_size << " B";
        }

        return os.str();
    }
}
