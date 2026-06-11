#pragma once

#include <functional>
#include <string>
#include <vector>

struct CloudConfigEntry {
    std::string id;
    std::string name;
    std::string description;
    bool        isPublic = false;
    std::string updatedAt;
};

struct CloudLineupEntry {
    std::string id;
    std::string map;
    std::string title;
    std::string description;
    std::string grenadeType;
    int         spotCount = 0;
    int         downloadCount = 0;
};

namespace cloud_api {

void initFromEnvironment();

bool hasToken();
const std::string& accessToken();

bool listConfigs(std::vector<CloudConfigEntry>& out, std::string& error);
bool downloadConfig(const std::string& id, std::string& jsonOut, std::string& error);
bool uploadConfig(const std::string& name, const std::string& description,
                  const std::string& json, bool isPublic,
                  std::string& outId, std::string& error);
bool deleteConfig(const std::string& id, std::string& error);

bool listLineupPacks(const std::string& mapFilter,
                     std::vector<CloudLineupEntry>& out, std::string& error);
bool downloadLineupPack(const std::string& id, std::string& jsonOut, std::string& error);
bool uploadLineupPack(const std::string& title, const std::string& map,
                      const std::string& description, const std::string& grenadeType,
                      const std::string& json, bool isPublic,
                      std::string& outId, std::string& error);

} // namespace cloud_api
