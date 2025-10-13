#include "Player.h"
#include "World.h"
#include "ScriptMgr.h"
#include "Chat.h"
#include "LoginDatabase.h"
#include "WorldSession.h"
#include "AccountMgr.h"
#include "Realm.h" // Added to define REALM_ID_CURRENT
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <filesystem>

// --- Global Configuration Variables ---
// Loaded manually from the .conf file
static bool      g_ModuleEnabled = true;
static bool      g_BypassForGMsEnabled = false;
static uint32    g_GMLevelBypass = 3;
static std::unordered_set<uint32> g_WhitelistedAccounts;

// --- Module Internals ---
static std::unordered_map<uint32, bool> g_launcherStatusByAccount;
static std::mutex g_launcherStatusMutex;


class LauncherCheckWorldScript : public WorldScript
{
public:
    LauncherCheckWorldScript() : WorldScript("LauncherCheckWorldScript") { }

private:
    void LoadConfiguration()
    {
        // Reset to defaults before loading
        g_ModuleEnabled = true;
        g_BypassForGMsEnabled = false;
        g_GMLevelBypass = 3;
        g_WhitelistedAccounts.clear();

        std::string configFilePath = "./configs/modules/mod-launcher-check.conf";

        if (!std::filesystem::exists(configFilePath))
        {
            LOG_WARN("module", "[Launcher Check] 구성 파일 {}을(를) 찾을 수 없어 기본 설정을 사용합니다.", configFilePath);
            return;
        }

        std::ifstream configFile(configFilePath);
        if (!configFile.is_open())
        {
            LOG_ERROR("module", "[Launcher Check] 구성 파일 {}을(를) 열 수 없어 기본 설정을 사용하고 있습니다.", configFilePath);
            return;
        }

        LOG_INFO("module", "[Launcher Check] {}에서 설정값을 로드합니다.", configFilePath);
        std::string line;
        while (std::getline(configFile, line))
        {
            // Skip comments and empty lines
            if (line.empty() || line[0] == '#' || line.find_first_not_of(" 	") == std::string::npos)
            {
                continue;
            }

            std::istringstream iss(line);
            std::string key, equals, value;
            if (std::getline(iss, key, '=') && std::getline(iss, value))
            {
                // Trim whitespace from key and value
                key.erase(0, key.find_first_not_of(" 	"));
                key.erase(key.find_last_not_of(" 	") + 1);
                value.erase(0, value.find_first_not_of(" 	"));
                value.erase(value.find_last_not_of(" 	") + 1);

                if (key == "mod-launcher-check.Enabled")
                {
                    g_ModuleEnabled = (value == "1");
                }
                else if (key == "mod-launcher-check.BypassForGMsEnabled")
                {
                    g_BypassForGMsEnabled = (value == "1");
                }
                else if (key == "mod-launcher-check.GMLevelBypass")
                {
                    g_GMLevelBypass = std::stoul(value);
                }
                else if (key == "mod-launcher-check.Whitelist")
                {
                    // Remove quotes if present
                    if (value.length() >= 2 && value.front() == '"' && value.back() == '"')
                    {
                        value = value.substr(1, value.length() - 2);
                    }

                    if (!value.empty())
                    {
                        std::stringstream ss(value);
                        std::string item;
                        while (std::getline(ss, item, ','))
                        {
                            try
                            {
                                item.erase(0, item.find_first_not_of(" 	"));
                                item.erase(item.find_last_not_of(" 	") + 1);
                                g_WhitelistedAccounts.insert(std::stoul(item));
                            }
                            catch (const std::exception& e)
                            {
                                LOG_ERROR("module", "[Launcher Check] 허용 목록에 잘못된 계정 ID '{}'이 있습니다: {}.", item, e.what());
                            }
                        }
                    }
                }
            }
        }
        configFile.close();
        LOG_INFO("module", "[Launcher Check] 허용 목록에 있는 {}개의 계정을 로드했습니다.", g_WhitelistedAccounts.size());
    }

public:
    void OnBeforeConfigLoad(bool reload) override
    {
        // Load config on startup and reload
        LoadConfiguration();
    }

    void OnStartup() override
    {
        LOG_INFO("module", "mod-launcher-check is {}.", g_ModuleEnabled ? "enabled" : "disabled");
    }
};

class LauncherCheckAccountScript : public AccountScript
{
public:
    LauncherCheckAccountScript() : AccountScript("LauncherCheckAccountScript") {}

    void OnAccountLogin(uint32 accountId) override
    {
        if (!g_ModuleEnabled)
        {
            return;
        }

        bool needsKick = true;

        // 1. Whitelist check
        if (g_WhitelistedAccounts.count(accountId))
        {
            needsKick = false;
            LOG_INFO("module", "[Launcher Check] 허용된 계정 {}이 런처 검사를 통과했습니다.", accountId);
        }
        // 2. GM bypass check
                else if (g_BypassForGMsEnabled && AccountMgr::GetSecurity(accountId, 0) >= g_GMLevelBypass)
        {
            needsKick = false;
            LOG_INFO("module", "[Launcher Check] GM 계정 {} (보안 레벨: {})이 런처 검사를 통과했습니다.", accountId, AccountMgr::GetSecurity(accountId, 0));
        }
        // 3. Launcher status check from DB
        else
        {
            QueryResult result = LoginDatabase.Query("SELECT online FROM account WHERE id = {}", accountId);

            if (result)
            {
                Field* fields = result->Fetch();
                if (fields[0].Get<uint32>() == 2)
                {
                    needsKick = false;
                    LoginDatabase.Execute("UPDATE account SET online = 1 WHERE id = {}", accountId);
                    LOG_INFO("module", "[Launcher Check] 계정 {}이 런처를 사용한 것으로 확인되었습니다.", accountId);
                }
                else
                {
                    LOG_INFO("module", "[Launcher Check] 계정 {}은 런처를 사용하지않은 것으로 확인되었습니다.(상태: {}).", accountId, fields[0].Get<uint32>());
                }
            }
            else
            {
                LOG_WARN("module", "[Launcher Check] 계정{}에 대한 DB 쿼리가 실패했습니다. 런처 상태를 확인할 수 없습니다.", accountId);
            }
        }

        std::lock_guard<std::mutex> lock(g_launcherStatusMutex);
        g_launcherStatusByAccount[accountId] = needsKick;
    }
};

class LauncherCheckPlayerScript : public PlayerScript
{
private:
    std::unordered_map<ObjectGuid, time_t> pendingKicks;
    std::mutex pendingKicksMutex;

public:
    LauncherCheckPlayerScript() : PlayerScript("LauncherCheckPlayerScript") { }

    void OnPlayerLogin(Player* player) override
    {
        if (!g_ModuleEnabled)
        {
            return;
        }

        uint32 accountId = player->GetSession()->GetAccountId();
        bool shouldKick = false;

        {
            std::lock_guard<std::mutex> lock(g_launcherStatusMutex);
            auto it = g_launcherStatusByAccount.find(accountId);
            if (it != g_launcherStatusByAccount.end())
            {
                shouldKick = it->second;
                g_launcherStatusByAccount.erase(it);
            }
        }

        if (shouldKick)
        {
            LOG_INFO("module", "[Launcher Check] Player {} (Account: {}) is scheduled for kick in 30 seconds for not using the launcher.", player->GetName(), accountId);
            ChatHandler(player->GetSession()).PSendSysMessage("|cffff0000[System] 경고: 공식 런처를 이용해 접속해야 합니다. 10초 후 연결이 해제됩니다.|r");

            std::lock_guard<std::mutex> lock(pendingKicksMutex);
            pendingKicks[player->GetGUID()] = time(nullptr) + 10;
        }
        else
        {
                        bool wasBypassed = g_WhitelistedAccounts.count(accountId) ||
                              (g_BypassForGMsEnabled && AccountMgr::GetSecurity(accountId, 0) >= g_GMLevelBypass);

            if (!wasBypassed)
            {
                 ChatHandler(player->GetSession()).PSendSysMessage("|cff00ff00[System] 환영합니다! 런처를 이용한 접속으로 확인되었습니다.|r");
            }
        }
    }

    void OnPlayerUpdate(Player* player, uint32 /*p_time*/) override
    {
        if (!g_ModuleEnabled)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(pendingKicksMutex);
        auto it = pendingKicks.find(player->GetGUID());
        if (it != pendingKicks.end())
        {
            if (time(nullptr) >= it->second)
            {
                player->GetSession()->KickPlayer();
            }
        }
    }

    void OnPlayerLogout(Player* player) override
    {
        std::lock_guard<std::mutex> lock(pendingKicksMutex);
        pendingKicks.erase(player->GetGUID());

        std::lock_guard<std::mutex> lock2(g_launcherStatusMutex);
        g_launcherStatusByAccount.erase(player->GetSession()->GetAccountId());
    }
};

void Addmod_launcher_checkScripts()
{
    new LauncherCheckWorldScript();
    new LauncherCheckAccountScript();
    new LauncherCheckPlayerScript();
}
