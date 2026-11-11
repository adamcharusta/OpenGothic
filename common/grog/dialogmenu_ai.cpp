// GROG: AI dialogue mode for DialogMenu ("Grog i klamstwa").
// The player types a line; the AI Game Master answers in character; accepted commands become engine actions.
#ifdef GROG_AI
#include "ui/dialogmenu.h"

#include <Tempest/Log>
#include <Tempest/MemReader>
#include <Tempest/Painter>
#include <Tempest/Sound>

#include <grog/ai/tts.h>

#include <algorithm>
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

template<class T>
bool isReady(const std::future<T>& f) {
  return f.wait_for(std::chrono::seconds(0))==std::future_status::ready;
}

// One-time hints: a missing/broken GrogAI.ini first, then what went wrong with a provider (UTF-8, once per kind).
void printHints(std::string_view providerHintUtf8) {
  std::string hint = GrogHost::inst().takeHint();
  if(!hint.empty())
    Gothic::inst().onPrint(hint);
  if(!providerHintUtf8.empty())
    Gothic::inst().onPrint(toGameText(providerHintUtf8));
}

}  // namespace

bool DialogMenu::grogStart(Npc& p, Npc& ot) {
  auto& host = GrogHost::inst();
  auto  id   = host.aiNpcId(ot);
  if(!id)
    return false;  // not a pirate with a card: original dialogues

  grogReset();  // hands any future still in flight to the host instead of destroying (= waiting for) it
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

  if(auto* world = Gothic::inst().world())
    host.syncPlayerItems(*world, p);
  std::string hint = host.takeHint();
  if(!hint.empty())
    Gothic::inst().onPrint(hint);
  // Sent by the Game Master: the NPC speaks first (the line is already in its AI memory).
  std::string openingUtf8;
  std::string opening = host.takeOpening(*id, &openingUtf8);
  if(!opening.empty()) {
    auto* session = host.gameSession();
    if(host.voiceOn() && session!=nullptr && !openingUtf8.empty()) {
      // Voice on: subtitle and audio appear together once the line is synthesized (grogTick).
      grog.speechText    = opening;
      grog.pendingSpeech = session->speakAsync(*id, std::move(openingUtf8), false);
      } else {
      grogSay(ot, opening);
      }
    }
  update();
  return true;
  }

void DialogMenu::grogSay(Npc& npc, std::string_view gameText, std::string_view wav) {
  const uint64_t len = gameText.size();
  current.txt     = std::string(gameText);
  current.msgTime = std::min<uint64_t>(1500 + 55*len, 15000);
  currentSnd      = SoundEffect();
  if(!wav.empty()) {
    // The same slot and volume path as the original voice lines (aiOutput).
    try {
      Tempest::MemReader rd(wav.data(), wav.size());
      Tempest::Sound     snd(rd);
      if(!snd.isEmpty()) {
        uint64_t ms = uint64_t(wavDurationMs(wav).value_or(int(snd.timeLength())));
        currentSnd      = soundDevice.load(snd);
        current.msgTime = std::max<uint64_t>(ms + 300, 1500);
        currentSnd.play();
        }
      }
    catch(...) {
      Log::e("[grog] unable to play the NPC voice line");
      currentSnd = SoundEffect();
      }
    }
  current.time    = current.msgTime + (dlgAnimation ? ANIM_TIME*2 : 0);
  curentIsPl      = (&npc==pl);
  npc.setAiOutputBarrier(current.msgTime,false);
  update();
  }

bool DialogMenu::grogKeyDown(KeyEvent& e) {
  if(!grog.active) {
    // A voiced farewell is being synthesized: Esc stops waiting for it, the dialog closes (endAfter).
    if(e.key==Event::K_ESCAPE && grog.pendingSpeech.valid()) {
      GrogHost::inst().adopt(std::move(grog.pendingSpeech));
      grog.speechText.clear();
      update();
      }
    return true;
    }
  if(e.key==Event::K_ESCAPE) {
    if(current.time>0)
      skipPhrase(); else
      grogEnd();
    return true;
    }
  if(e.key==Event::K_Return) {
    if(grog.pending.valid() || grog.pendingSpeech.valid() || current.time>0 || haveToWaitOutput())
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
  if(grog.pendingSpeech.valid()) {
    // Voiced opening or farewell: shown when ready; a farewell's endAfter waits for it.
    if(current.time>0 || !isReady(grog.pendingSpeech))
      return;
    Speech      s    = grog.pendingSpeech.get();
    std::string text = grog.speechText.empty() ? toGameText(s.text) : std::move(grog.speechText);
    grog.speechText.clear();
    if(other!=nullptr && !text.empty())
      grogSay(*other, text, s.audioWav);
    printHints(s.hint);
    return;
    }
  if(!grog.active && !grog.endAfter)
    return;
  if(grog.endAfter && current.time==0 && !haveToWaitOutput()) {
    grog.endAfter = false;
    choice.clear();  // the regular tick closes the dialog
    return;
    }
  if(!grog.pending.valid() || current.time>0)
    return;
  if(!isReady(grog.pending))
    return;

  TurnResult r = grog.pending.get();
  if(other==nullptr || pl==nullptr)
    return;
  grogSay(*other, toGameText(r.speech), r.audioWav);

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
  printHints(r.hint);  // LLM or voice hint, whenever there is one
  }

void DialogMenu::grogEnd() {
  if(!grog.active)
    return;
  auto& host = GrogHost::inst();
  if(grog.pending.valid())
    host.adopt(std::move(grog.pending));
  if(grog.pendingSpeech.valid())
    host.adopt(std::move(grog.pendingSpeech));  // an opening still being synthesized
  grog.speechText.clear();
  grog.active = false;
  auto* session = host.gameSession();
  if(session!=nullptr && other!=nullptr) {
    if(host.voiceOn())
      grog.pendingSpeech = session->farewellAsync(grog.npcId);  // said in grogTick, then endAfter closes
    else
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
  if(grog.pendingSpeech.valid())
    GrogHost::inst().adopt(std::move(grog.pendingSpeech));
  grog = GrogState();
  }

int DialogMenu::grogPaintInput(Painter* p, int x, int y, int w) {
  const float scale = Gothic::interfaceScale(this);
  auto fnt = Resources::font(Resources::FontType::Hi, scale);
  std::string text;
  if(grog.pending.valid() || grog.pendingSpeech.valid())
    text = "> ...";
  else
    text = "> " + toGameText(grog.input) + "_";
  Size sz = fnt.textSize(w, text);
  if(p!=nullptr)
    fnt.drawText(*p, x, y, w, 0, text, AlignLeft);
  return sz.h + int(8*scale);
  }

#endif
