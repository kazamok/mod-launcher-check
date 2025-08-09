#include "Player.h"
#include "World.h"
#include "ScriptMgr.h"
#include "Chat.h"
#include "LoginDatabase.h"
#include "WorldSession.h"
#include "AccountMgr.h"
#include <unordered_map>
#include <mutex>

// A map to hold accounts that need to be checked when the player logs in.
static std::unordered_map<uint32, bool> g_launcherStatusByAccount;
static std::mutex g_launcherStatusMutex;

class LauncherCheckAccountScript : public AccountScript
{
public:
    LauncherCheckAccountScript() : AccountScript("LauncherCheckAccountScript") {}

    void OnAccountLogin(uint32 accountId) override
    {
        QueryResult result = LoginDatabase.Query("SELECT online FROM account WHERE id = {}", accountId);
        bool needsKick = true;

        if (result)
        {
            Field* fields = result->Fetch();
            if (fields[0].Get<uint32>() == 2)
            {
                needsKick = false;
                LoginDatabase.Execute("UPDATE account SET online = 1 WHERE id = {}", accountId);
                LOG_INFO("module", "[Launcher Check] Account {} verified via launcher.", accountId);
            }
            else
            {
                Field* fields = result->Fetch();
                LOG_INFO("module", "[Launcher Check] Account {} did not use launcher (status: {}).", accountId, fields[0].Get<uint32>());
            }
        }
        else
        {
            LOG_WARN("module", "[Launcher Check] DB query failed for account {}.", accountId);
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
            LOG_INFO("module", "[Launcher Check] Player {} (Account: {}) is scheduled for kick in 30 seconds.", player->GetName(), accountId);
            ChatHandler(player->GetSession()).PSendSysMessage("|cffff0000[System] 경고: 공식 런처를 통해 접속해야 합니다. 30초 후 연결이 해제됩니다.|r");
            
            std::lock_guard<std::mutex> lock(pendingKicksMutex);
            pendingKicks[player->GetGUID()] = time(nullptr) + 30;
        }
        else
        {
            ChatHandler(player->GetSession()).PSendSysMessage("|cff00ff00[System] 환영합니다! 런처 연결이 확인되었습니다.|r");
        }
    }

    void OnPlayerUpdate(Player* player, uint32 /*p_time*/) override
    {
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
    new LauncherCheckAccountScript();
    new LauncherCheckPlayerScript();
}
