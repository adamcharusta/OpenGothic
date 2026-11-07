#pragma once
// GROG: "Grog i klamstwa" mod - owner of the AI Game Master session inside the engine.
#ifdef GROG_AI

#include <grog/ai/bridge.h>
#include <grog/ai/llm.h>
#include <grog/ai/session.h>
#include <grog/ai/telemetry.h>

#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class Npc;
class World;

class GrogHost final {
  public:
    static GrogHost& inst();

    // Called when a new game starts / a game is loaded / the session ends.
    void onStartGame(bool newGame);
    void onWorldLoaded(World& world);
    void onSessionExit();
    void tick();

    bool ready() const { return session!=nullptr; }
    grog::ai::GameSession* gameSession() { return session.get(); }
    const grog::ai::GameData* data() const { return gameData ? &*gameData : nullptr; }

    // Card id of an AI-driven NPC, or nothing (original dialogues stay).
    std::optional<std::string> aiNpcId(const Npc& npc) const;

    // One-time, player-facing hint (cp1250) when the AI is not configured; empty after the first call.
    std::string takeHint();
    int64_t     gameTimeS() const;

    // Executes engine actions produced by the AI. `hero` is the player.
    void execute(const std::vector<grog::ai::EngineAction>& actions, World& world, Npc& hero);

    // Story items the hero carries -> AI world state (call before a conversation).
    void syncPlayerItems(World& world, Npc& hero);
    // Npc::onNoHealth hook: `victim` was knocked out (or killed) by `attacker`.
    void onKnockout(Npc& victim, Npc* attacker, bool death);

    // Save/load: JSON blob stored in the save archive as "grog/state.json".
    std::string saveJson();
    void        setPendingLoad(std::string json);

    // A dialog closed while an answer was pending: keep the future alive until it finishes.
    void adopt(std::future<grog::ai::TurnResult>&& f);

    std::string ui(const char* pl, const char* en) const;  // localized UI string, cp1250

  private:
    GrogHost() = default;
    void setupWorld(World& world);
    void tickDirector(World& world);
    void finishGame(const std::string& outcome);
    std::filesystem::path logDir;

    std::optional<grog::ai::GameData>             gameData;
    grog::ai::AiConfig                            config;
    std::unique_ptr<grog::ai::IHttpClient>        http;
    std::unique_ptr<grog::ai::ILlmProvider>       actor, director;
    std::ofstream                                 logFile;
    std::unique_ptr<grog::ai::Telemetry>          telemetry;
    std::unique_ptr<grog::ai::GameSession>        session;
    std::vector<std::future<grog::ai::TurnResult>> orphans;
    std::future<grog::ai::DirectorResult>          directorPending;
    std::string                                   pendingLoad;
    bool                                          finished = false;
    std::string                                   hint;
    bool                                          hintShown = false;
    bool                                          newGamePending = false;
    int64_t                                       startTicks = 0;
  };

#endif
