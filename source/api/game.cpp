#include "api/game.hpp"
#include "api/net.hpp"
#include "utils/config.hpp"
#include "utils/utils.hpp"
#include <curl/curl.h>

#include <regex>

Game::Game(const std::string& m_title, const std::string& m_tid) {
    selectedTitle = m_title;
    title = utils::sanitizeTitle(m_title);
    tid = m_tid;

    brls::Logger::debug(
        "Selected installed title tid={} raw='{}' sanitized='{}'",
        tid,
        utils::debugStringPreview(selectedTitle),
        utils::debugStringPreview(title));

    if (!utils::isValidGameTitle(title)) {
        brls::Logger::error(
            "Rejecting malformed title before GameBanana lookup tid={} raw='{}' sanitized='{}'",
            tid,
            utils::debugStringPreview(selectedTitle),
            utils::debugStringPreview(title));
        gamebananaID = 0;
        return;
    }

    searchGame();
    parseJson();
    if (gamebananaID > 0)
        loadCategories();

}

void Game::searchGame() {
    auto curl = curl_easy_init();
    if (!curl) {
        brls::Logger::error("Failed to initialize curl for game lookup: {} ({})", title, tid);
        gamebananaID = -1;
        return;
    }

    char* escapedTitle = curl_easy_escape(curl, title.c_str(), static_cast<int>(title.length()));
    if (!escapedTitle) {
        brls::Logger::error("Failed to URL-encode title for GameBanana lookup tid={} title='{}'", tid, utils::debugStringPreview(title));
        curl_easy_cleanup(curl);
        gamebananaID = -1;
        return;
    }

    std::string title_url(escapedTitle);
    curl_free(escapedTitle);
    curl_easy_cleanup(curl);

    cfg::Config config;
    std::string endpoint = config.getStrictSearch() ?
        "https://gamebanana.com/apiv11/Util/Game/NameMatch?_sName={}" :
        "https://gamebanana.com/apiv11/Util/Search/Results?_sModelName=Game&_sOrder=best_match&_sSearchString={}%20%28Switch%29";
    std::string requestUrl = fmt::format(endpoint, title_url);

    brls::Logger::debug(
        "GameBanana lookup tid={} title='{}' query='{}' url={}",
        tid,
        utils::debugStringPreview(selectedTitle),
        utils::debugStringPreview(title),
        requestUrl);

    try {
        json = net::downloadRequest(requestUrl);
    } catch (const std::exception& e) {
        brls::Logger::error("Failed to search for game: " + title + " - " + e.what());
        gamebananaID = -1;
        return;
    }

    int recordCount = 0;
    if (json.is_object() && json.contains("_aMetadata") && json.at("_aMetadata").is_object() && json.at("_aMetadata").contains("_nRecordCount") && json.at("_aMetadata").at("_nRecordCount").is_number_integer()) {
        recordCount = json.at("_aMetadata").at("_nRecordCount").get<int>();
    } else if (json.is_object() && json.contains("_aRecords") && json.at("_aRecords").is_array()) {
        recordCount = static_cast<int>(json.at("_aRecords").size());
        brls::Logger::error("GameBanana search metadata missing for tid={} title='{}'; using _aRecords size", tid, utils::debugStringPreview(title));
    }

    brls::Logger::debug("GameBanana lookup result tid={} title='{}' recordCount={}", tid, utils::debugStringPreview(title), recordCount);

    if(json.empty()) {
        brls::Logger::error("Failed to search for game: {} ({})", title, tid);
    }
}

void Game::parseJson() {
    if(!json.is_object()) {
        brls::Logger::error("Skipping GameBanana parse for tid={} because search response is not a JSON object", tid);
        return;
    }

    if(!json.contains("_aRecords") || !json.at("_aRecords").is_array()) {
        brls::Logger::error("Skipping GameBanana parse for tid={} title='{}' because _aRecords is missing", tid, utils::debugStringPreview(title));
        return;
    }

    const auto& records = json.at("_aRecords");
    if(records.empty()) {
        brls::Logger::error("GameBanana returned no records for tid={} title='{}'", tid, utils::debugStringPreview(title));
        return;
    }

    size_t pos = 0;
    cfg::Config config;
    if(config.getStrictSearch()) {
        for (size_t i = 0; i < records.size(); ++i) {
            if (!records[i].is_object() || !records[i].contains("_sName") || !records[i].at("_sName").is_string())
                continue;

            std::string sName = utils::sanitizeTitle(records[i].at("_sName").get<std::string>());
            if (sName.find("Switch") != std::string::npos) { // Disembiguation for multiplat titles
                pos = i;
                break;
            }
        }
    }

    const auto& record = records[pos];
    if (!record.is_object()) {
        brls::Logger::error("Skipping GameBanana record {} for tid={} because it is not an object", pos, tid);
        return;
    }

    if (!record.contains("_idRow") || !record.at("_idRow").is_number_integer()) {
        brls::Logger::error("Skipping GameBanana record {} for tid={} because _idRow is missing", pos, tid);
        return;
    }

    if (record.contains("_sName") && record.at("_sName").is_string()) {
        std::string parsedTitle = utils::sanitizeTitle(record.at("_sName").get<std::string>());
        if (utils::isValidGameTitle(parsedTitle)) {
            title = parsedTitle;
        } else {
            brls::Logger::error(
                "GameBanana record returned malformed name for tid={} id={}: '{}'",
                tid,
                record.at("_idRow").get<int>(),
                utils::debugStringPreview(parsedTitle));
        }
    } else {
        brls::Logger::error("GameBanana record {} for tid={} is missing _sName; keeping sanitized installed title", pos, tid);
    }

    gamebananaID = record.at("_idRow").get<int>();
    brls::Logger::debug("Selected GameBanana record tid={} id={} title='{}' index={}", tid, gamebananaID, utils::debugStringPreview(title), pos);
}

void Game::loadCategories() {
    std::string requestUrl = fmt::format("https://gamebanana.com/apiv11/Game/{}/ProfilePage", gamebananaID);
    brls::Logger::debug("Loading GameBanana metadata tid={} id={} url={}", tid, gamebananaID, requestUrl);
    json = net::downloadRequest(requestUrl);

    categories.clear();
    bannerURL.clear();

    if(!json.is_object()) {
        brls::Logger::error("Failed to load GameBanana metadata for game: {} ({})", title, tid);
        return;
    }

    if (json.contains("_aPreviewMedia") && json.at("_aPreviewMedia").is_object() && json.at("_aPreviewMedia").contains("_aImages") && json.at("_aPreviewMedia").at("_aImages").is_array()) {
        for (const auto& image : json.at("_aPreviewMedia").at("_aImages")) {
            if (!image.is_object() || !image.contains("_sType") || !image.at("_sType").is_string())
                continue;

            if (image.at("_sType").get<std::string>() != "banner")
                continue;

            if (image.contains("_sUrl") && image.at("_sUrl").is_string()) {
                bannerURL = image.at("_sUrl").get<std::string>();
                break;
            }
        }
    } else {
        brls::Logger::error("GameBanana metadata for tid={} id={} is missing preview images", tid, gamebananaID);
    }

    if (bannerURL.empty())
        brls::Logger::error("No GameBanana banner found for tid={} id={} title='{}'", tid, gamebananaID, utils::debugStringPreview(title));

    int index = 0;
    if (json.contains("_aModRootCategories") && json.at("_aModRootCategories").is_array()) {
        for(const auto& tag : json.at("_aModRootCategories")) {
            if (!tag.is_object() || !tag.contains("_sName") || !tag.at("_sName").is_string() || !tag.contains("_idRow") || !tag.at("_idRow").is_number_integer()) {
                brls::Logger::error("Skipping malformed GameBanana category entry for tid={} id={}", tid, gamebananaID);
                continue;
            }

            categories.push_back(Category(utils::sanitizeTitle(tag.at("_sName").get<std::string>()), tag.at("_idRow").get<int>(), index));
            index++;
        }
    } else {
        brls::Logger::error("GameBanana metadata for tid={} id={} is missing _aModRootCategories", tid, gamebananaID);
    }

    brls::Logger::debug("Loaded GameBanana metadata tid={} id={} categories={} bannerUrl={}", tid, gamebananaID, categories.size(), bannerURL.empty() ? std::string("<none>") : bannerURL);
}
