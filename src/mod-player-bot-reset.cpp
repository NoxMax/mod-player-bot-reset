#include "mod-player-bot-reset.h"
#include "ScriptMgr.h"
#include "Player.h"
#include "Common.h"
#include "Chat.h"
#include "Log.h"
#include "PlayerbotAIBase.h"
#include "Configuration/Config.h"
#include "PlayerbotMgr.h"
#include "PlayerbotAI.h"
#include "AutoMaintenanceOnLevelupAction.h"
#include "ObjectMgr.h"
#include "WorldSession.h"
#include "Item.h"
#include "RandomPlayerbotMgr.h"
#include "ObjectAccessor.h"
#include "PlayerbotFactory.h"

// -----------------------------------------------------------------------------
// GLOBALS: Configuration Values
// -----------------------------------------------------------------------------
static uint8 g_ResetBotMaxLevel      = 80;
static uint8 g_ResetBotChancePercent = 100;
static bool  g_DebugMode             = false;
static bool  g_ScaledChance          = false;

// When true, bots at or above g_ResetBotMaxLevel are reset only after they have
// accumulated at least g_MinTimePlayed seconds at that level.
static bool  g_RestrictResetByPlayedTime = false;
static uint32 g_MinTimePlayed             = 86400; // in seconds (1 Day)
static uint32 g_PlayedTimeCheckFrequency  = 60;    // in seconds (default check frequency)

// -----------------------------------------------------------------------------
// LOAD CONFIGURATION USING sConfigMgr
// -----------------------------------------------------------------------------
static void LoadPlayerBotResetConfig()
{
    g_ResetBotMaxLevel = static_cast<uint8>(sConfigMgr->GetOption<uint32>("ResetBotLevel.MaxLevel", 80));
    if (g_ResetBotMaxLevel < 2 || g_ResetBotMaxLevel > 80)
    {
        LOG_ERROR("server.loading", "[mod-player-bot-reset] Invalid ResetBotLevel.MaxLevel value: {}. Using default value 80.", g_ResetBotMaxLevel);
        g_ResetBotMaxLevel = 80;
    }

    g_ResetBotChancePercent = static_cast<uint8>(sConfigMgr->GetOption<uint32>("ResetBotLevel.ResetChance", 100));
    if (g_ResetBotChancePercent > 100)
    {
        LOG_ERROR("server.loading", "[mod-player-bot-reset] Invalid ResetBotLevel.ResetChance value: {}. Using default value 100.", g_ResetBotChancePercent);
        g_ResetBotChancePercent = 100;
    }

    g_DebugMode   = sConfigMgr->GetOption<bool>("ResetBotLevel.DebugMode", false);
    g_ScaledChance = sConfigMgr->GetOption<bool>("ResetBotLevel.ScaledChance", false);

    g_RestrictResetByPlayedTime = sConfigMgr->GetOption<bool>("ResetBotLevel.RestrictTimePlayed", false);
    g_MinTimePlayed             = sConfigMgr->GetOption<uint32>("ResetBotLevel.MinTimePlayed", 86400);
    g_PlayedTimeCheckFrequency  = sConfigMgr->GetOption<uint32>("ResetBotLevel.PlayedTimeCheckFrequency", 60);
}

// -----------------------------------------------------------------------------
// UTILITY FUNCTIONS: Detect if a Player is a Bot
// -----------------------------------------------------------------------------
static bool IsPlayerBot(Player* player)
{
    if (!player)
    {
        LOG_ERROR("server.loading", "[mod-player-bot-reset] IsPlayerBot called with nullptr.");
        return false;
    }

    PlayerbotAI* botAI = sPlayerbotsMgr->GetPlayerbotAI(player);
    return botAI && botAI->IsBotAI();
}

static bool IsPlayerRandomBot(Player* player)
{
    if (!player)
    {
        LOG_ERROR("server.loading", "[mod-player-bot-reset] IsPlayerRandomBot called with nullptr.");
        return false;
    }
    return sRandomPlayerbotMgr->IsRandomBot(player);
}

// -----------------------------------------------------------------------------
// HELPER FUNCTION: Compute the Reset Chance
// -----------------------------------------------------------------------------
static uint8 ComputeResetChance(uint8 level)
{
    uint8 chance = g_ResetBotChancePercent;
    if (g_ScaledChance)
    {
        chance = static_cast<uint8>((static_cast<float>(level) / g_ResetBotMaxLevel) * g_ResetBotChancePercent);
        if (g_DebugMode)
        {
            LOG_INFO("server.loading", "[mod-player-bot-reset] ComputeResetChance: For level {} / {} with scaling, computed chance = {}%", level, g_ResetBotMaxLevel, chance);
        }
    }
    else if (g_DebugMode)
    {
        LOG_INFO("server.loading", "[mod-player-bot-reset] ComputeResetChance: For level {} / {} without scaling, chance = {}%", level, g_ResetBotMaxLevel, chance);
    }
    return chance;
}

// -----------------------------------------------------------------------------
// HELPER FUNCTION: Perform the Reset Actions for a Bot
// -----------------------------------------------------------------------------
static void ResetBot(Player* player, uint8 currentLevel)
{
    uint8 levelToResetTo = 1;
    if (player->getClass() == CLASS_DEATH_KNIGHT)
        levelToResetTo = 55;

    PlayerbotFactory newFactory(player, levelToResetTo);

    newFactory.Randomize(false);

    if (g_DebugMode)
    {
        PlayerbotAI* botAI = sPlayerbotsMgr->GetPlayerbotAI(player);
        std::string playerClassName = botAI ? botAI->GetChatHelper()->FormatClass(player->getClass()) : "Unknown";
        LOG_INFO("server.loading", "[mod-player-bot-reset] ResetBot: Bot '{}' - {} at level {} was reset to level {}.",
                 player->GetName(), playerClassName, currentLevel, levelToResetTo);
    }

    ChatHandler(player->GetSession()).SendSysMessage("[mod-player-bot-reset] Your level has been reset.");

}

// -----------------------------------------------------------------------------
// PLAYER SCRIPT: OnLogin and OnLevelChanged (Original Logic Preserved)
// -----------------------------------------------------------------------------
class ResetBotLevelPlayerScript : public PlayerScript
{
public:
    ResetBotLevelPlayerScript() : PlayerScript("ResetBotLevelPlayerScript") { }

    void OnPlayerLogin(Player* player) override
    {
        if (!player)
            return;
        ChatHandler(player->GetSession()).SendSysMessage("The [mod-player-bot-reset] module is active on this server.");
    }

    void OnPlayerLevelChanged(Player* player, uint8 /*oldLevel*/) override
    {
        if (!player)
        {
            LOG_ERROR("server.loading", "[mod-player-bot-reset] OnLevelChanged called with nullptr player.");
            return;
        }

        uint8 newLevel = player->GetLevel();
        if (newLevel == 1)
            return;

        // Special case for Death Knights.
        if (newLevel == 55 && player->getClass() == CLASS_DEATH_KNIGHT)
            return;

        if (!IsPlayerBot(player))
        {
            if (g_DebugMode)
                LOG_INFO("server.loading", "[mod-player-bot-reset] OnLevelChanged: Player '{}' is not a bot. Skipping reset check.", player->GetName());
            return;
        }

        if (!IsPlayerRandomBot(player))
        {
            if (g_DebugMode)
                LOG_INFO("server.loading", "[mod-player-bot-reset] OnLevelChanged: Player '{}' is not a random bot. Skipping reset check.", player->GetName());
            return;
        }

        // If time-played restriction is enabled and the bot is at (or above) the max level,
        // defer the reset to the periodic OnUpdate handler.
        if (g_RestrictResetByPlayedTime && newLevel >= g_ResetBotMaxLevel)
        {
            if (g_DebugMode)
                LOG_INFO("server.loading", "[mod-player-bot-reset] OnLevelChanged: Bot '{}' at level {} deferred to OnUpdate due to time-played restriction.", player->GetName(), newLevel);
            return;
        }

        uint8 resetChance = ComputeResetChance(newLevel);
        if (g_ScaledChance || newLevel >= g_ResetBotMaxLevel)
        {
            if (g_DebugMode)
                LOG_INFO("server.loading", "[mod-player-bot-reset] OnLevelChanged: Bot '{}' at level {} has reset chance {}%.", player->GetName(), newLevel, resetChance);
            if (urand(0, 99) < resetChance)
                ResetBot(player, newLevel);
        }
    }
};

// -----------------------------------------------------------------------------
// WORLD SCRIPT: Load Configuration on Startup
// -----------------------------------------------------------------------------
class ResetBotLevelWorldScript : public WorldScript
{
public:
    ResetBotLevelWorldScript() : WorldScript("ResetBotLevelWorldScript") { }

    void OnStartup() override
    {
        LoadPlayerBotResetConfig();
        LOG_INFO("server.loading", "[mod-player-bot-reset] Loaded and active with MaxLevel = {}, ResetChance = {}%, ScaledChance = {}.",
                 static_cast<int>(g_ResetBotMaxLevel),
                 static_cast<int>(g_ResetBotChancePercent),
                 g_ScaledChance ? "Enabled" : "Disabled");
    }
};

// -----------------------------------------------------------------------------
// WORLD SCRIPT: OnUpdate Check for Time-Played Based Reset at Max Level
// This handler runs every g_PlayedTimeCheckFrequency seconds and iterates over players.
// For each bot at or above g_ResetBotMaxLevel that has accumulated at least g_MinTimePlayed
// seconds at the current level, it applies the same reset chance logic and resets the bot if the check passes.
// -----------------------------------------------------------------------------
class ResetBotLevelTimeCheckWorldScript : public WorldScript
{
public:
    ResetBotLevelTimeCheckWorldScript() : WorldScript("ResetBotLevelTimeCheckWorldScript"), m_timer(0) { }

    void OnUpdate(uint32 diff) override
    {
        if (!g_RestrictResetByPlayedTime)
            return;

        m_timer += diff;
        if (m_timer < g_PlayedTimeCheckFrequency * 1000)
            return;
        m_timer = 0;

        if (g_DebugMode)
        {
            LOG_INFO("server.loading", "[mod-player-bot-reset] OnUpdate: Starting time-based reset check...");
        }

        auto const& allPlayers = ObjectAccessor::GetPlayers();
        for (auto const& itr : allPlayers)
        {
            Player* candidate = itr.second;
            if (!candidate || !candidate->IsInWorld())
                continue;
            if (!IsPlayerBot(candidate) || !IsPlayerRandomBot(candidate))
                continue;

            uint8 currentLevel = candidate->GetLevel();
            if (currentLevel < g_ResetBotMaxLevel)
                continue;

            // Only reset if the bot has played at least g_MinTimePlayed seconds at this level.
            if (candidate->GetLevelPlayedTime() < g_MinTimePlayed)
            {
                if (g_DebugMode)
                {
                    LOG_INFO("server.loading", "[mod-player-bot-reset] OnUpdate: Bot '{}' at level {} has insufficient played time ({} < {} seconds).",
                             candidate->GetName(), currentLevel, candidate->GetLevelPlayedTime(), g_MinTimePlayed);
                }
                continue;
            }

            uint8 resetChance = ComputeResetChance(currentLevel);
            if (g_DebugMode)
            {
                LOG_INFO("server.loading", "[mod-player-bot-reset] OnUpdate: Bot '{}' qualifies for time-based reset. Level: {}, LevelPlayedTime: {} seconds, computed reset chance: {}%.",
                         candidate->GetName(), currentLevel, candidate->GetLevelPlayedTime(), resetChance);
            }
            if (urand(0, 99) < resetChance)
            {
                if (g_DebugMode)
                {
                    LOG_INFO("server.loading", "[mod-player-bot-reset] OnUpdate: Reset chance check passed for bot '{}'. Resetting bot.", candidate->GetName());
                }
                ResetBot(candidate, currentLevel);
            }
        }
    }
private:
    uint32 m_timer;
};

// -----------------------------------------------------------------------------
// ENTRY POINT: Register Scripts
// -----------------------------------------------------------------------------
void Addmod_player_bot_resetScripts()
{
    new ResetBotLevelWorldScript();
    new ResetBotLevelPlayerScript();
    new ResetBotLevelTimeCheckWorldScript();
}
