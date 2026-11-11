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
#include "gamemusic.h"
#include "world/objects/item.h"

#include <zenkit/addon/daedalus.hh>

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
  if(directorPending.valid())
    directorPending.wait();
  directorPending = {};
  finished       = false;
  hintShown      = false;
  pendingFights.clear();
  disarmed.clear();
  openings.clear();
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
  logDir = pathFromUtf8(config.logDir);
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
  if(directorPending.valid())
    directorPending.wait();
  directorPending = {};
  // Wait for answers still in flight: they reference the session.
  for(auto& f:orphans)
    if(f.valid())
      f.wait();
  orphans.clear();
  session.reset();
  telemetry.reset();
  }

void GrogHost::tick() {
  if(auto* world = Gothic::inst().world()) {
    tickDirector(*world);
    tickFights(*world);
    }
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

int64_t GrogHost::gameTimeS() const {
  // World ticks are saved with the game, so the AI clock survives save/load.
  if(auto* world = Gothic::inst().world())
    return int64_t(world->tickCount()/1000);
  return (nowMs()-startTicks)/1000;
  }

void GrogHost::tickDirector(World& world) {
  if(session==nullptr || finished)
    return;
  if(directorPending.valid()) {
    if(directorPending.wait_for(std::chrono::seconds(0))!=std::future_status::ready)
      return;
    DirectorResult r = directorPending.get();
    if(Npc* hero = world.player(); hero!=nullptr && gameData)
      execute(toEngineActions(r.verdicts, kDirector, *gameData), world, *hero);
    return;
    }
  int64_t t = gameTimeS();
  if(session->directorDue(t))
    directorPending = session->directAsync("tick", t);
  }

void GrogHost::syncPlayerItems(World& world, Npc& hero) {
  if(session==nullptr || !gameData)
    return;
  std::set<std::string> carried;
  for(auto& [id, inst]:gameData->items) {
    size_t sym = world.script().findSymbolIndex(inst);
    if(sym!=size_t(-1) && hero.itemCount(sym)>0)
      carried.insert(id);
    }
  session->syncPlayerItems(carried);
  }

void GrogHost::onKnockout(Npc& victim, Npc* attacker, bool death) {
  if(session==nullptr || attacker==nullptr || death)
    return;
  Npc* npc = victim.isPlayer() ? attacker : (attacker->isPlayer() ? &victim : nullptr);
  if(npc==nullptr)
    return;
  if(auto id = aiNpcId(*npc))
    session->reportFight(*id, !victim.isPlayer(), gameTimeS());
  if(auto* world = Gothic::inst().world())
    if(auto* sym = world->script().findSymbol(size_t(npc->instanceSymbol())))
      rearm(*world, sym->name());
  }

void GrogHost::tickFights(World& world) {
  // A fight asked for in a conversation starts once the conversation is over (an NPC in ZS_Talk ignores it).
  if(!pendingFights.empty() && !Gothic::inst().isInDialog()) {
    auto fights = std::move(pendingFights);
    pendingFights.clear();
    Npc* hero = world.player();
    for(auto& f:fights) {
      Npc* npc    = findNpc(world, f.npcInstance);
      Npc* target = f.targetInstance.empty() ? hero : findNpc(world, f.targetInstance);
      if(npc!=nullptr && target!=nullptr && npc!=target)
        startFistFight(world, *npc, *target);
      }
    }
  const int64_t t = gameTimeS();
  std::vector<std::string> due;
  for(auto& d:disarmed)
    if(t>=d.untilS)
      due.push_back(d.npcInstance);
  for(auto& n:due)
    rearm(world, n);
  }

void GrogHost::startFistFight(World& world, Npc& npc, Npc& target) {
  // Fist fight: the NPC puts its weapons away for the brawl and gets them back afterwards.
  npc.closeWeapon(true);
  for(Item* w : {npc.currentMeleeWeapon(), npc.currentRangedWeapon()}) {
    if(w==nullptr)
      continue;
    size_t cls = w->clsId();
    npc.unequipItem(cls);
    if(auto* sym = world.script().findSymbol(size_t(npc.instanceSymbol())))
      disarmed.push_back({sym->name(), cls, gameTimeS()+180});
    }
  if(world.script().grogAttack(npc, target))
    return;
  // Fallback when the script helper is missing: push the attack state directly.
  size_t zs = world.script().findSymbolIndex("ZS_ATTACK");
  if(zs!=size_t(-1)) {
    auto& st = world.script().aiState(ScriptFn(zs));
    npc.setTarget(&target);
    npc.aiPush(AiQueue::aiStartState(st.funcIni,0,&target,nullptr,""));
    }
  }

void GrogHost::rearm(World& world, const std::string& npcInstance) {
  Npc* npc = findNpc(world, npcInstance);
  for(size_t i=0; i<disarmed.size();) {
    if(disarmed[i].npcInstance!=npcInstance) {
      ++i;
      continue;
      }
    if(npc!=nullptr && npc->itemCount(disarmed[i].weapon)>0)
      npc->useItem(disarmed[i].weapon);
    disarmed.erase(disarmed.begin()+long(i));
    }
  }

void GrogHost::onItemInit(zenkit::IItem& item, std::string_view instanceName) {
  if(!gameData)
    return;
  const std::string name0 = upper(instanceName);
  for(auto& [id, inst]:gameData->items) {
    if(upper(inst)!=name0)
      continue;
    auto name = gameData->itemGameNames.find(id);
    if(name!=gameData->itemGameNames.end() && name->second.count(config.language)) {
      item.name        = toGameText(name->second.at(config.language));
      item.description = item.name;
      }
    auto text = gameData->itemGameTexts.find(id);
    if(text!=gameData->itemGameTexts.end() && text->second.count(config.language)) {
      // Inventory shows text[0..5]/count[0..5]: one line of description, no value.
      for(auto& t:item.text)
        t.clear();
      for(auto& n:item.count)
        n = 0;
      item.text[0] = toGameText(text->second.at(config.language));
      }
    return;
    }
  }

std::string GrogHost::takeOpening(const std::string& npcId) {
  auto it = openings.find(npcId);
  if(it==openings.end())
    return {};
  std::string s = std::move(it->second);
  openings.erase(it);
  return s;
  }

std::string GrogHost::saveJson() {
  if(session==nullptr)
    return {};
  return session->saveState().dump();
  }

void GrogHost::setPendingLoad(std::string json) {
  pendingLoad = std::move(json);
  }

void GrogHost::finishGame(const std::string& outcome) {
  if(finished || session==nullptr)
    return;
  finished = true;
  const char* pl = outcome=="returned" ? "Greg odzyskal kufel! Klucz do skrzyni jest twoj."
                 : outcome=="fooled"   ? "Greg dal sie nabrac. Klucz do skrzyni jest twoj... na razie."
                 : outcome=="caught"   ? "Greg przejrzal oszustwo. Koniec przygody."
                                       : "Poddales sie. Greg jest wsciekly.";
  const char* en = outcome=="returned" ? "Greg got his tankard back! The chest key is yours."
                 : outcome=="fooled"   ? "Greg fell for it. The chest key is yours... for now."
                 : outcome=="caught"   ? "Greg saw through the trick. The adventure is over."
                                       : "You gave up. Greg is furious.";
  Gothic::inst().onPrint(ui(pl, en));
  auto sum = session->summary();
  std::error_code ec;
  fs::create_directories(logDir, ec);
  std::ofstream f(logDir / ("summary-" + std::to_string(sum.value("seed", uint64_t(0))) + ".json"), std::ios::binary);
  f << sum.dump(2) << "\n";
  if(telemetry)
    telemetry->event("ending", sum);
  }

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
  // The menu/loading theme otherwise plays on until the camp music takes over at the end of a musical phrase.
  GameMusic::inst().setMusic(zenkit::IMusicTheme(), GameMusic::mkTags(GameMusic::Std, GameMusic::Day));
  if(!pendingLoad.empty() && session!=nullptr) {
    auto j = nlohmann::json::parse(pendingLoad, nullptr, false);
    auto r = session->loadState(j);
    if(!r.ok()) {
      Tempest::Log::e("[grog] ", r.error().message);
      hint = ui("Ten zapis nie ma stanu AI (starsza wersja moda) - Mistrz Gry zaczyna od nowa.",
                "This save has no AI state (older mod version) - the Game Master starts over.");
      }
    pendingLoad.clear();
    finished = session->ending().has_value();
    }
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
  for(auto& [inst, count]:gameData->start.items) {
    size_t id = script.findSymbolIndex(inst);
    if(id==size_t(-1)) {
      Tempest::Log::e("[grog] start item not found: ", inst);
      continue;
      }
    hero->addItem(id, size_t(count));
    if(upper(inst).rfind("ITMW_",0)==0)
      hero->useItem(id);  // equip the starting weapon
    }

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
      case EngineAction::TakeItemFromPlayer: {
        Npc*   npc  = findNpc(world, a.npcInstance);
        size_t item = script.findSymbolIndex(a.itemInstance);
        if(npc!=nullptr && item!=size_t(-1) && hero.itemCount(item)>0) {
          hero.delItem(item,1);
          npc->addItem(item,1);
          Gothic::inst().onPrint(ui("Oddano przedmiot", "Item handed over"));
          }
        break;
        }
      case EngineAction::EndGame:
        finishGame(a.text);
        break;
      case EngineAction::StartFight:
        pendingFights.push_back({a.npcInstance, ""});
        break;
      case EngineAction::NpcFightNpc:
        pendingFights.push_back({a.npcInstance, a.npc2Instance});
        break;
      case EngineAction::NpcGoto: {
        Npc* npc = findNpc(world, a.npcInstance);
        if(auto* wp = world.findPoint(a.waypoint); npc!=nullptr && wp!=nullptr)
          npc->aiPush(AiQueue::aiGoToPoint(*wp));
        break;
        }
      case EngineAction::NpcApproachPlayer: {
        if(Npc* npc = findNpc(world, a.npcInstance)) {
          if(auto id = aiNpcId(*npc); id && !a.text.empty())
            openings[*id] = a.text;
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
