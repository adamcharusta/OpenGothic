// GROG: AI dialogue mode for DialogMenu ("Grog i klamstwa").
// The player types a line; the AI Game Master answers in character; accepted commands become engine actions.
#ifdef GROG_AI
#include "ui/dialogmenu.h"

#include <Tempest/Painter>

#include <chrono>

#include "grog/groghost.h"
#include "utils/gthfont.h"
#include "world/objects/npc.h"
#include "world/world.h"
#include "gothic.h"
#include "resources.h"

using namespace Tempest;
using namespace grog::ai;

namespace {

constexpr size_t kMaxInputChars = 300;

void appendUtf8(std::string& out, uint32_t cp) {
  if(cp < 0x80) {
    out += char(cp);
  } else if(cp < 0x800) {
    out += char(0xC0 | (cp >> 6));
    out += char(0x80 | (cp & 0x3F));
  } else {
    out += char(0xE0 | (cp >> 12));
    out += char(0x80 | ((cp >> 6) & 0x3F));
    out += char(0x80 | (cp & 0x3F));
  }
}

void popUtf8(std::string& s) {
  while(!s.empty()) {
    unsigned char c = static_cast<unsigned char>(s.back());
    s.pop_back();
    if((c & 0xC0) != 0x80)
      break;
  }
}

}  // namespace

bool DialogMenu::grogStart(Npc& p, Npc& ot) {
  auto& host = GrogHost::inst();
  auto  id   = host.aiNpcId(ot);
  if(!id)
    return false;  // not a pirate with a card: original dialogues

  grog          = GrogState();
  grog.active   = true;
  grog.npcId    = *id;
  state         = State::Active;
  depth         = 0;
  curentIsPl    = true;
  choiceAnimTime= 0;
  dlgSel        = 0;

  GameScript::DlgChoice end;
  end.title = host.ui("KONIEC", "END");
  choice    = {end};

  std::string hint = host.takeHint();
  if(!hint.empty())
    Gothic::inst().onPrint(hint);
  update();
  return true;
  }

void DialogMenu::grogSay(Npc& npc, std::string_view gameText) {
  const uint64_t len = gameText.size();
  current.txt     = std::string(gameText);
  current.msgTime = std::min<uint64_t>(1500 + 55*len, 15000);
  current.time    = current.msgTime + (dlgAnimation ? ANIM_TIME*2 : 0);
  currentSnd      = SoundEffect();
  curentIsPl      = (&npc==pl);
  npc.setAiOutputBarrier(current.msgTime,false);
  update();
  }

bool DialogMenu::grogKeyDown(KeyEvent& e) {
  if(e.key==Event::K_ESCAPE) {
    if(current.time>0)
      skipPhrase(); else
      grogEnd();
    return true;
    }
  if(e.key==Event::K_Return) {
    if(grog.pending.valid() || current.time>0 || haveToWaitOutput())
      return true;
    if(grog.input.empty()) {
      grogEnd();
      return true;
      }
    auto* session = GrogHost::inst().gameSession();
    if(session==nullptr || pl==nullptr) {
      grogEnd();
      return true;
      }
    std::string line = std::move(grog.input);
    grog.input.clear();
    grogSay(*pl, toGameText(line));
    grog.pending = session->talkAsync(grog.npcId, line, GrogHost::inst().gameTimeS());
    return true;
    }
  if(e.key==Event::K_Back) {
    popUtf8(grog.input);
    update();
    return true;
    }
  if(e.code>=32 && e.code!=127 && (e.code<0xD800 || e.code>0xDFFF) && utf8Length(grog.input)<kMaxInputChars) {
    appendUtf8(grog.input, e.code);
    update();
    return true;
    }
  // Navigation keys etc.: swallow, there is nothing to navigate.
  return e.key!=Event::K_F1 && !(Event::K_F1<=e.key && e.key<=Event::K_F12);
  }

void DialogMenu::grogTick() {
  GrogHost::inst().tick();
  if(!grog.active && !grog.endAfter)
    return;
  if(grog.endAfter && current.time==0 && !haveToWaitOutput()) {
    grog.endAfter = false;
    choice.clear();  // the regular tick closes the dialog
    return;
    }
  if(!grog.pending.valid() || current.time>0)
    return;
  if(grog.pending.wait_for(std::chrono::seconds(0))!=std::future_status::ready)
    return;

  TurnResult r = grog.pending.get();
  if(other==nullptr || pl==nullptr)
    return;
  grogSay(*other, toGameText(r.speech));

  auto& host = GrogHost::inst();
  if(auto* data = host.data()) {
    auto actions = toEngineActions(r.verdicts, grog.npcId, *data);
    bool fight = false;
    for(auto& a:actions)
      fight |= (a.kind==EngineAction::StartFight);
    if(auto* world = Gothic::inst().world())
      host.execute(actions, *world, *pl);
    if(fight || r.endDialog) {
      grog.active   = false;
      grog.endAfter = true;
      }
    }
  if(r.error && r.usedFallback) {
    std::string hint = host.takeHint();
    if(!hint.empty())
      Gothic::inst().onPrint(hint);
    }
  }

void DialogMenu::grogEnd() {
  if(!grog.active)
    return;
  if(grog.pending.valid())
    GrogHost::inst().adopt(std::move(grog.pending));
  grog.active = false;
  auto* session = GrogHost::inst().gameSession();
  if(session!=nullptr && other!=nullptr) {
    grogSay(*other, toGameText(session->farewell(grog.npcId)));
    grog.endAfter = true;
    } else {
    choice.clear();
    }
  update();
  }

void DialogMenu::grogReset() {
  if(grog.pending.valid())
    GrogHost::inst().adopt(std::move(grog.pending));
  grog = GrogState();
  }

int DialogMenu::grogPaintInput(Painter* p, int x, int y, int w) {
  const float scale = Gothic::interfaceScale(this);
  auto fnt = Resources::font(Resources::FontType::Hi, scale);
  std::string text;
  if(grog.pending.valid())
    text = "> ...";
  else
    text = "> " + toGameText(grog.input) + "_";
  Size sz = fnt.textSize(w, text);
  if(p!=nullptr)
    fnt.drawText(*p, x, y, w, 0, text, AlignLeft);
  return sz.h + int(8*scale);
  }

#endif
