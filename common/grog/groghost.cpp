// GROG: "Grog i klamstwa" mod - AI Game Master host.
#ifdef GROG_AI
#include "groghost.h"

#include <grog/ai/io.h>

#include <Tempest/Log>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <chrono>
#include <filesystem>

#include "game/gamescript.h"
#include "world/objects/npc.h"
#include "world/aiqueue.h"
#include "world/waypoint.h"
#include "world/world.h"
#include "gothic.h"

#ifdef _WIN32
#include <windows.h>
#endif

using namespace grog::ai;
namespace fs = std::filesystem;

namespace {

// Mod root = parent of the directory holding the executable (package layout: <root>/bin/Gothic2Notr.exe).
fs::path modRoot() {
#ifdef _WIN32
  wchar_t buf[MAX_PATH] = {};
  DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  fs::path exe(std::wstring(buf, n));
#else
  std::error_code ec;
  fs::path exe = fs::read_symlink("/proc/self/exe", ec);
#endif
  return exe.parent_path().parent_path();
}

// Per-user folder (writable even when the mod is installed under Program Files).
fs::path userDir() {
#ifdef _WIN32
  wchar_t buf[MAX_PATH] = {};
  DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
  if(n>0 && n<MAX_PATH)
    return fs::path(std::wstring(buf, n)) / "GrogIKlamstwa";
#endif
  if(const char* home = std::getenv("HOME"))
    return fs::path(home) / ".grogiklamstwa";
  return fs::temp_directory_path() / "GrogIKlamstwa";
}

bool writableDir(const fs::path& dir) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  std::ofstream probe(dir / ".write-test", std::ios::binary);
  bool ok = bool(probe);
  probe.close();
  fs::remove(dir / ".write-test", ec);
  return ok;
}

int64_t nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string upper(std::string_view s) {
  std::string u(s);
  std::transform(u.begin(), u.end(), u.begin(), [](unsigned char c) { return char(std::toupper(c)); });
  return u;
}

Npc* findNpc(World& world, std::string_view instance) {
  if(instance.empty())
    return nullptr;
  size_t id = world.script().findSymbolIndex(instance);
  if(id==size_t(-1))
    return nullptr;
  return world.findNpcByInstance(id);
}

}  // namespace

GrogHost& GrogHost::inst() {
  static GrogHost h;
  return h;
}

std::string GrogHost::ui(const char* pl, const char* en) const {
  bool polish = config.language==Language::Pl;
  return toGameText(polish ? pl : en);
}

void GrogHost::onStartGame(bool newGame) {
  session.reset();
  telemetry.reset();
  director.reset();
  actor.reset();
  hint.clear();
  hintShown      = false;
  newGamePending = newGame;
  startTicks     = nowMs();

  const fs::path root = modRoot();
  auto data = loadGameData(root / "mod" / "data");
  if(!data.ok()) {
    Tempest::Log::e("[grog] cannot load mod data: ", data.error().message);
    hint = toGameText("Grog i kłamstwa: brak danych moda (mod/data). / Mod data missing.");
    return;
    }
  gameData = data.value();

  // GrogAI.ini: next to the mod first, then in the user's folder (%APPDATA%\GrogIKlamstwa).
  auto ini = readTextFile(root / "GrogAI.ini");
  if(!ini.ok())
    ini = readTextFile(userDir() / "GrogAI.ini");
  if(!ini.ok()) {
    hint = toGameText("Brak pliku GrogAI.ini - skopiuj GrogAI.example.ini (obok moda albo do %APPDATA%/GrogIKlamstwa) "
                      "i wpisz klucz API. / GrogAI.ini not found - copy GrogAI.example.ini and add your API key.");
    ini = std::string("[ai]\nprovider = mock\n");
    }
  auto cfg = parseConfig(ini.value(), systemEnv());
  if(!cfg.ok()) {
    Tempest::Log::e("[grog] ", cfg.error().message);
    hint = toGameText(cfg.error().message);
    cfg = parseConfig("[ai]\nprovider = mock\n", systemEnv());
    }
  config = cfg.value();

  http     = makeHttpClient();
  actor    = makeProvider(config.actor, *http);
  director = makeProvider(config.director, *http);

  uint64_t seed = config.seed.value_or(uint64_t(std::chrono::system_clock::now().time_since_epoch().count()));
  fs::path logDir = pathFromUtf8(config.logDir);
  if(logDir.is_relative())
    logDir = root / logDir;
  if(!writableDir(logDir))
    logDir = userDir() / "logs";
  std::error_code ec;
  fs::create_directories(logDir, ec);
  logFile.close();
  logFile.open(logDir / ("session-" + std::to_string(seed) + ".jsonl"), std::ios::binary | std::ios::app);
  telemetry = std::make_unique<Telemetry>(logFile, true, gameData->pricing);
  session   = std::make_unique<grog::ai::GameSession>(config, *gameData, *actor, *director, *telemetry, seed);
  Tempest::Log::i("[grog] session started: ", redactedSummary(config), " seed=", seed);
  }

void GrogHost::onSessionExit() {
  // Wait for answers still in flight: they reference the session.
  for(auto& f:orphans)
    if(f.valid())
      f.wait();
  orphans.clear();
  session.reset();
  telemetry.reset();
  }

void GrogHost::tick() {
  orphans.erase(std::remove_if(orphans.begin(), orphans.end(), [](std::future<TurnResult>& f) {
                  return !f.valid() || f.wait_for(std::chrono::seconds(0))==std::future_status::ready;
                }),
                orphans.end());
  }

void GrogHost::adopt(std::future<TurnResult>&& f) {
  if(f.valid())
    orphans.push_back(std::move(f));
  }

std::string GrogHost::takeHint() {
  if(hintShown || hint.empty())
    return {};
  hintShown = true;
  return hint;
  }

int64_t GrogHost::gameTimeS() const { return (nowMs()-startTicks)/1000; }

std::optional<std::string> GrogHost::aiNpcId(const Npc& npc) const {
  if(!gameData || session==nullptr)
    return std::nullopt;
  auto* world = Gothic::inst().world();
  if(world==nullptr)
    return std::nullopt;
  auto* sym = world->script().findSymbol(size_t(npc.instanceSymbol()));
  if(sym==nullptr)
    return std::nullopt;
  auto id = npcIdForInstance(*gameData, sym->name());
  if(id && gameData->cards.count(*id))
    return id;
  return std::nullopt;
  }

void GrogHost::onWorldLoaded(World& world) {
  if(!newGamePending || !gameData)
    return;
  newGamePending = false;
  if(upper(world.name())!=upper(gameData->start.world))
    return;
  setupWorld(world);
  }

void GrogHost::setupWorld(World& world) {
  auto& script = world.script();
  Npc*  hero   = world.player();
  if(hero==nullptr || session==nullptr)
    return;

  if(auto* wp = world.findPoint(gameData->start.waypoint))
    hero->setPosition(wp->position());
  size_t armor = script.findSymbolIndex(gameData->start.armor);
  if(armor!=size_t(-1)) {
    if(hero->itemCount(armor)==0)
      hero->addItem(armor,1);
    hero->useItem(armor);
    }
  if(auto& h = hero->handlePtr())
    h->level = gameData->start.level;

  // Every cast member must be in the camp.
  for(auto& m:gameData->cast.npcs) {
    size_t id = script.findSymbolIndex(m.instance);
    if(id!=size_t(-1) && world.findNpcByInstance(id)==nullptr)
      world.addNpc(id, gameData->start.waypoint);
    }

  // Story items where the scenario/cards put them.
  WorldState ws = session->worldSnapshot();
  for(auto& [itemId, holder]:ws.itemHolder) {
    auto it = gameData->items.find(itemId);
    if(it==gameData->items.end())
      continue;
    size_t item = script.findSymbolIndex(it->second);
    if(item==size_t(-1))
      continue;
    if(holder.rfind("place:",0)==0) {
      auto wp = gameData->cast.placeWaypoints.find(holder.substr(6));
      if(wp!=gameData->cast.placeWaypoints.end())
        world.addItem(item, wp->second);
      } else if(Npc* npc = findNpc(world, instanceForNpc(*gameData, holder))) {
      npc->addItem(item,1);
      }
    }
  Tempest::Log::i("[grog] world prepared: ", world.name());
  }

void GrogHost::execute(const std::vector<EngineAction>& actions, World& world, Npc& hero) {
  auto& script = world.script();
  for(auto& a:actions) {
    switch(a.kind) {
      case EngineAction::QuestEntry: {
        std::string topic = ui("Kufel Grega", "Greg's tankard");
        script.grogQuestEntry(topic, a.text);
        Gothic::inst().onPrint(ui("Nowy wpis w dzienniku", "New log entry"));
        break;
        }
      case EngineAction::GiveItemToPlayer: {
        Npc*   npc  = findNpc(world, a.npcInstance);
        size_t item = script.findSymbolIndex(a.itemInstance);
        if(npc!=nullptr && item!=size_t(-1) && npc->itemCount(item)>0) {
          npc->delItem(item,1);
          hero.addItem(item,1);
          Gothic::inst().onPrint(ui("Otrzymano przedmiot", "Item received"));
          }
        break;
        }
      case EngineAction::StartFight:
      case EngineAction::NpcFightNpc: {
        Npc* npc    = findNpc(world, a.npcInstance);
        Npc* target = a.kind==EngineAction::StartFight ? &hero : findNpc(world, a.npc2Instance);
        size_t zs   = script.findSymbolIndex("ZS_ATTACK");
        if(npc!=nullptr && target!=nullptr && zs!=size_t(-1)) {
          auto& st = script.aiState(ScriptFn(zs));
          npc->setTarget(target);
          npc->aiPush(AiQueue::aiStartState(st.funcIni,0,target,nullptr,""));
          }
        break;
        }
      case EngineAction::NpcGoto: {
        Npc* npc = findNpc(world, a.npcInstance);
        if(auto* wp = world.findPoint(a.waypoint); npc!=nullptr && wp!=nullptr)
          npc->aiPush(AiQueue::aiGoToPoint(*wp));
        break;
        }
      case EngineAction::NpcApproachPlayer: {
        if(Npc* npc = findNpc(world, a.npcInstance)) {
          npc->aiPush(AiQueue::aiGoToNpc(&hero));
          npc->aiPush(AiQueue::aiProcessInfo(hero));
          }
        break;
        }
      case EngineAction::SpawnMonster: {
        size_t id = script.findSymbolIndex(a.monsterInstance);
        if(id!=size_t(-1))
          world.addNpc(id, a.waypoint);
        break;
        }
      case EngineAction::AmbientLine: {
        if(Npc* npc = findNpc(world, a.npcInstance))
          Gothic::inst().onPrint(std::string(npc->displayName())+": "+a.text);
        break;
        }
      }
    }
  }

#endif
