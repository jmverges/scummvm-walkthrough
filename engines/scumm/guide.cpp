/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "common/config-manager.h"
#include "common/events.h"
#include "common/file.h"
#include "common/fs.h"
#include "common/archive.h"
#include "common/system.h"
#include "common/textconsole.h"
#include "graphics/paletteman.h"
#include "graphics/surface.h"
#include "gui/gui-manager.h"
#include "gui/widget.h"
#include "common/translation.h"

#include "scumm/actor.h"
#include "scumm/object.h"
#include "scumm/guide.h"
#include "scumm/scumm.h"
#include "scumm/verbs.h"

#include <stdarg.h>

namespace Scumm {

// The game has to sit idle this long before the next step: a sentence often
// ends with a line of speech or a short animation that is not a script.
static const uint32 kIdleSettle = 600;
// A step that never starts (nothing happens after it) is given up on.
static const uint32 kStepTimeout = 30000;

ScummGuide::ScummGuide(ScummEngine *vm)
	: _vm(vm), _current(0), _remaining(0), _state(kWaitIdle), _stateSince(0),
	  _idleSince(0), _executing(false), _testNext(0), _testStart(0) {
	// Per game in scummvm.ini: guide_file (default walkthrough.txt in the
	// game's folder), guide_record (a file to append the sentences played
	// to), and for testing guide_test (a script of timed input) and
	// guide_shots (where its screenshots go).
	if (ConfMan.hasKey("guide_record"))
		_recordPath = ConfMan.get("guide_record");
	if (ConfMan.hasKey("guide_file"))
		load(ConfMan.get("guide_file"));
	else if (!load((ConfMan.getPath("path").appendComponent("walkthrough.txt")).toString()))
		loadBundled();
	if (ConfMan.hasKey("guide_test"))
		loadTestScript(ConfMan.get("guide_test"));
	_shotDir = ConfMan.hasKey("guide_shots") ? ConfMan.get("guide_shots") : ".";
	// Where the player had got to, kept per game.
	if (ConfMan.hasKey("guide_step"))
		_current = CLIP<int>(ConfMan.getInt("guide_step"), 0, _steps.size());
}

ScummGuide::~ScummGuide() {
}

void ScummGuide::logf(const char *fmt, ...) {
	va_list va;
	va_start(va, fmt);
	Common::String s = Common::String::vformat(fmt, va);
	va_end(va);
	g_system->logMessage(LogMessageType::kInfo, ("guide: " + s + "\n").c_str());
}

Common::String ScummGuide::objectName(int obj) const {
	if (obj <= 0)
		return "";
	const byte *name = _vm->getObjOrActorName(obj);
	return name ? Common::String((const char *)name) : Common::String();
}

// A file by its full path (Common::File only looks in the game's folders).
static Common::SeekableReadStream *openPath(const Common::String &path) {
	Common::FSNode node(Common::Path(path, '/'));
	return node.exists() ? node.createReadStream() : nullptr;
}

// --------------------------------------------------------------- recording

static void appendLine(const Common::String &path, const Common::String &line) {
	// Re-read and rewrite: DumpFile has no append, and a walkthrough is small.
	Common::String text;
	Common::SeekableReadStream *in = openPath(path);
	if (in) {
		text = in->readString(0, in->size());
		delete in;
	}
	Common::DumpFile out;
	if (out.open(Common::Path(path, '/'), true)) {
		out.writeString(text);
		out.writeString(line);
		out.writeString("\n");
		out.close();
	}
}

void ScummGuide::onSentence(int verb, int objectA, int objectB) {
	if (_executing)
		return;
	Common::String c = Common::String::format("; room %d: verb %d '%s'", _vm->_currentRoom, verb,
		objectName(objectA).c_str());
	if (objectB)
		c += Common::String::format(" with '%s'", objectName(objectB).c_str());
	logf("sentence %d %d %d %s", verb, objectA, objectB, c.c_str());
	if (_recordPath.empty())
		return;
	appendLine(_recordPath, Common::String::format("s %d %d %d %s", verb, objectA, objectB, c.c_str()));
}

void ScummGuide::onVerbClick(int verbid) {
	if (_executing || verbid <= 0)
		return;
	logf("verb click %d (room %d)", verbid, _vm->_currentRoom);
	if (_recordPath.empty())
		return;
	appendLine(_recordPath, Common::String::format("v %d ; room %d", verbid, _vm->_currentRoom));
}

// ----------------------------------------------------------------- loading

bool ScummGuide::load(const Common::String &path) {
	Common::SeekableReadStream *in = openPath(path);
	if (!in)
		return false;
	parse(*in, path);
	delete in;
	return true;
}

void ScummGuide::parse(Common::SeekableReadStream &f, const Common::String &path) {
	_steps.clear();
	Common::String section;
	while (!f.eos()) {
		Common::String line = f.readLine();
		line.trim();
		if (line.empty() || line.hasPrefix("#"))
			continue;
		if (line.hasPrefix("==")) {
			section = line.substr(2);
			section.trim();
			continue;
		}
		Step st;
		st.section = section;
		st.verb = st.objectA = st.objectB = 0;
		size_t semi = line.findFirstOf(';');
		if (semi != Common::String::npos) {
			st.comment = line.substr(semi + 1);
			st.comment.trim();
			line = line.substr(0, semi);
		}
		if (line.hasPrefix("s ")) {
			st.kind = Step::kSentence;
			sscanf(line.c_str() + 2, "%d %d %d", &st.verb, &st.objectA, &st.objectB);
		} else if (line.hasPrefix("v ")) {
			st.kind = Step::kVerb;
			sscanf(line.c_str() + 2, "%d", &st.verb);
		} else {
			continue;
		}
		_steps.push_back(st);
	}
	_current = 0;
	logf("%d steps in %s", _steps.size(), path.c_str());
}

// The walkthroughs that come with ScummVM, in its data files:
// walkthroughs/<gameid>[-demo].txt.
void ScummGuide::loadBundled() {
	// Not every demo carries GF_DEMO (it can come from the data's MD5 alone),
	// but the detector always names it a demo in the description.
	const bool demo = (_vm->_game.features & GF_DEMO) || ConfMan.get("description").contains("Demo");
	Common::String name = Common::String::format("walkthroughs/%s%s.txt", _vm->_game.gameid, demo ? "-demo" : "");
	Common::SeekableReadStream *in = SearchMan.createReadStreamForMember(Common::Path(name, '/'));
	if (!in) {
		logf("no walkthrough (looked for %s)", name.c_str());
		return;
	}
	parse(*in, name);
	delete in;
}

Common::String ScummGuide::section(int i) const {
	return i >= 0 && i < (int)_steps.size() ? _steps[i].section : Common::String();
}

void ScummGuide::setCurrent(int i) {
	_current = CLIP<int>(i, 0, _steps.size());
	ConfMan.setInt("guide_step", _current);
	ConfMan.flushToDisk();
}

Common::String ScummGuide::describe(int i) const {
	if (i < 0 || i >= (int)_steps.size())
		return "";
	const Step &st = _steps[i];
	return st.comment.empty() ? Common::String::format("%s %d %d %d", st.kind == Step::kVerb ? "v" : "s",
		st.verb, st.objectA, st.objectB) : st.comment;
}

// ---------------------------------------------------------------- playback

void ScummGuide::play(int count) {
	if (_current >= (int)_steps.size()) {
		logf("nothing left to play");
		return;
	}
	_remaining = count;
	_state = kWaitIdle;
	_idleSince = 0;
	logf("play from step %d (%s)", _current + 1, count < 0 ? "to the end" : "some steps");
}

void ScummGuide::stop() {
	_remaining = 0;
}

// Waiting for the player: input on, no cutscene, nobody talking, no
// sentence queued or running, and the player's actor standing still.
bool ScummGuide::isIdle() const {
	if (_vm->_userPut <= 0)
		return false;
	if (_vm->vm.cutSceneStackPointer != 0)
		return false;
	if (_vm->_haveMsg)
		return false;
	if (_vm->_sentenceNum > 0)
		return false;
	if (_vm->VAR_SENTENCE_SCRIPT != 0xFF && _vm->isScriptRunning(_vm->VAR(_vm->VAR_SENTENCE_SCRIPT)))
		return false;
	if (_vm->VAR_EGO != 0xFF) {
		int ego = _vm->VAR(_vm->VAR_EGO);
		if (ego > 0 && ego < _vm->_numActors) {
			Actor *a = _vm->derefActorSafe(ego, "guide");
			if (a && a->_moving)
				return false;
		}
	}
	return true;
}

// The objects a sentence names must be here: in the room or carried.
bool ScummGuide::available(const Step &st) const {
	if (st.kind == Step::kVerb) {
		for (int i = 1; i < _vm->_numVerbs; i++)
			if (_vm->_verbs[i].verbid == st.verb && _vm->_verbs[i].curmode == 1)
				return true;
		return false;
	}
	if (st.objectA > 0 && st.objectA >= _vm->_numActors && _vm->whereIsObject(st.objectA) == WIO_NOT_FOUND)
		return false;
	if (st.objectB > 0 && st.objectB >= _vm->_numActors && _vm->whereIsObject(st.objectB) == WIO_NOT_FOUND)
		return false;
	return true;
}

void ScummGuide::execute(const Step &st) {
	_executing = true;
	if (st.kind == Step::kSentence) {
		logf("step %d: sentence %d %d %d %s", _current + 1, st.verb, st.objectA, st.objectB, st.comment.c_str());
		_vm->doSentence(st.verb, st.objectA, st.objectB);
	} else {
		logf("step %d: verb %d %s", _current + 1, st.verb, st.comment.c_str());
		_vm->runInputScript(kVerbClickArea, st.verb, 1);
	}
	_executing = false;
}

void ScummGuide::tick() {
	runTestScript();
	if (_remaining == 0 || _current >= (int)_steps.size()) {
		if (_remaining != 0) {
			logf("walkthrough finished");
			_remaining = 0;
		}
		return;
	}
	uint32 now = g_system->getMillis();
	bool idle = isIdle();
	if (!idle) {
		_idleSince = 0;
		if (_state == kWaitIdle)
			return;
	} else if (!_idleSince) {
		_idleSince = now;
	}

	if (_state == kRunning) {
		// Done when the game has been idle a while again; a step that
		// changed nothing visible (already done, or refused) is idle at
		// once and ends the same way.
		if (idle && now - _idleSince >= kIdleSettle && now - _stateSince >= kIdleSettle) {
			logf("step %d done (room %d)", _current + 1, _vm->_currentRoom);
			setCurrent(_current + 1);
			if (_remaining > 0)
				_remaining--;
			_state = kWaitIdle;
		} else if (now - _stateSince > kStepTimeout * 4) {
			logf("step %d never finished; stopping", _current + 1);
			_remaining = 0;
		}
		return;
	}

	// kWaitIdle: start the next step once the game has settled.
	if (!idle || now - _idleSince < kIdleSettle)
		return;
	const Step &st = _steps[_current];
	if (!available(st)) {
		if (!_stateSince || _state != kWaitIdle)
			_stateSince = now;
		if (now - _stateSince > kStepTimeout) {
			logf("step %d: its object or verb is not here (room %d); stopping", _current + 1, _vm->_currentRoom);
			_remaining = 0;
			_stateSince = 0;
		}
		return;
	}
	if (_remaining > 0)    // easy mode: say what is being done
		g_system->displayMessageOnOSD(Common::U32String(describe(_current)));
	execute(st);
	_state = kRunning;
	_stateSince = now;
	_idleSince = 0;
}

// ------------------------------------------------------------ test scripts

// One event per line: "<ms> <command> [args]", ms from the start.
//   key <name>          escape, return, space, f1..f12, or one character
//   click X Y / rclick X Y / move X Y   in game coordinates
//   shot NAME           the game screen to guide_shots/NAME.ppm
//   play [N] / stop     the walkthrough
//   objects             log the room's objects and the verbs on screen
//   quit
void ScummGuide::loadTestScript(const Common::String &path) {
	Common::SeekableReadStream *in = openPath(path);
	if (!in) {
		logf("test script %s not found", path.c_str());
		return;
	}
	Common::SeekableReadStream &f = *in;
	while (!f.eos()) {
		Common::String line = f.readLine();
		line.trim();
		if (line.empty() || line.hasPrefix("#"))
			continue;
		TestEvent ev;
		char cmd[32] = "", arg[128] = "";
		ev.x = ev.y = 0;
		unsigned at = 0;
		if (sscanf(line.c_str(), "%u %31s %127s", &at, cmd, arg) < 2)
			continue;
		ev.at = at;
		ev.cmd = cmd;
		ev.arg = arg;
		sscanf(line.c_str(), "%*u %*s %d %d", &ev.x, &ev.y);
		_test.push_back(ev);
	}
	delete in;
	logf("test script: %d events", _test.size());
}

static Common::KeyCode keyFor(const Common::String &name, uint16 &ascii) {
	ascii = 0;
	if (name == "escape") { ascii = 27; return Common::KEYCODE_ESCAPE; }
	if (name == "return") { ascii = 13; return Common::KEYCODE_RETURN; }
	if (name == "space") { ascii = ' '; return Common::KEYCODE_SPACE; }
	if (name == "period") { ascii = '.'; return Common::KEYCODE_PERIOD; }
	if (name.size() >= 2 && name[0] == 'f') {
		int n = (int)strtol(name.c_str() + 1, nullptr, 10);
		if (n >= 1 && n <= 12) { ascii = Common::ASCII_F1 + n - 1; return (Common::KeyCode)(Common::KEYCODE_F1 + n - 1); }
	}
	if (name.size() == 1) { ascii = name[0]; return (Common::KeyCode)name[0]; }
	return Common::KEYCODE_INVALID;
}

void ScummGuide::runTestScript() {
	if (_testNext >= _test.size())
		return;
	uint32 now = g_system->getMillis();
	if (!_testStart)
		_testStart = now;
	Common::EventManager *em = g_system->getEventManager();
	while (_testNext < _test.size() && now - _testStart >= _test[_testNext].at) {
		const TestEvent &t = _test[_testNext++];
		Common::Event ev;
		if (t.cmd == "key") {
			uint16 ascii;
			ev.kbd.keycode = keyFor(t.arg, ascii);
			ev.kbd.ascii = ascii;
			ev.type = Common::EVENT_KEYDOWN;
			em->pushEvent(ev);
			ev.type = Common::EVENT_KEYUP;
			em->pushEvent(ev);
		} else if (t.cmd == "click" || t.cmd == "rclick" || t.cmd == "move") {
			ev.mouse = Common::Point(t.x, t.y);
			ev.type = Common::EVENT_MOUSEMOVE;
			em->pushEvent(ev);
			g_system->warpMouse(t.x, t.y);
			if (t.cmd != "move") {
				ev.type = t.cmd == "click" ? Common::EVENT_LBUTTONDOWN : Common::EVENT_RBUTTONDOWN;
				em->pushEvent(ev);
				ev.type = t.cmd == "click" ? Common::EVENT_LBUTTONUP : Common::EVENT_RBUTTONUP;
				em->pushEvent(ev);
			}
		} else if (t.cmd == "shot") {
			saveShot(t.arg);
		} else if (t.cmd == "play") {
			play(t.x > 0 ? t.x : -1);
		} else if (t.cmd == "stop") {
			stop();
		} else if (t.cmd == "objects") {
			logf("room %d, ego %d, idle %d", _vm->_currentRoom,
				_vm->VAR_EGO != 0xFF ? _vm->VAR(_vm->VAR_EGO) : -1, isIdle());
			for (int i = 1; i < _vm->_numLocalObjects; i++) {
				const ObjectData &od = _vm->_objs[i];
				if (od.obj_nr) {
					Common::String verbs;
					for (int v = 1; v < 256; v++)
						if (_vm->getVerbEntrypoint(od.obj_nr, v))
							verbs += Common::String::format(" %d", v);
					logf("  object %d '%s' at %d,%d %dx%d state %d verbs:%s", od.obj_nr, objectName(od.obj_nr).c_str(),
						od.x_pos, od.y_pos, od.width, od.height, od.state, verbs.c_str());
				}
			}
			for (int i = 1; i < _vm->_numVerbs; i++)
				if (_vm->_verbs[i].verbid && _vm->_verbs[i].curmode)
					logf("  verb slot %d id %d mode %d", i, _vm->_verbs[i].verbid, _vm->_verbs[i].curmode);
			for (int i = 0; i < _vm->_numInventory; i++)
				if (_vm->_inventory[i])
					logf("  carried %d '%s'", _vm->_inventory[i], objectName(_vm->_inventory[i]).c_str());
		} else if (t.cmd == "save") {
			Common::Error e = _vm->saveGameState(t.x, t.arg.empty() ? "guide" : t.arg);
			logf("save slot %d: %s", t.x, e.getDesc().c_str());
		} else if (t.cmd == "load") {
			Common::Error e = _vm->loadGameState(t.x);
			logf("load slot %d: %s", t.x, e.getDesc().c_str());
		} else if (t.cmd == "goto") {
			setCurrent(t.x > 0 ? t.x - 1 : 0);
			logf("current step %d", _current + 1);
		} else if (t.cmd == "menu") {
			ev.type = Common::EVENT_MAINMENU;
			em->pushEvent(ev);
		} else if (t.cmd == "quit") {
			ev.type = Common::EVENT_QUIT;
			em->pushEvent(ev);
		}
	}
}

void ScummGuide::saveShot(const Common::String &name) {
	Graphics::Surface *s = g_system->lockScreen();
	if (!s)
		return;
	byte pal[256 * 3];
	g_system->getPaletteManager()->grabPalette(pal, 0, 256);
	Common::DumpFile out;
	Common::String path = _shotDir + "/" + name + ".ppm";
	if (out.open(Common::Path(path, '/'), true)) {
		out.writeString(Common::String::format("P6\n%d %d\n255\n", s->w, s->h));
		for (int y = 0; y < s->h; y++) {
			for (int x = 0; x < s->w; x++) {
				byte r, g, b;
				if (s->format.bytesPerPixel == 1) {
					byte c = *(const byte *)s->getBasePtr(x, y);
					r = pal[c * 3]; g = pal[c * 3 + 1]; b = pal[c * 3 + 2];
				} else {
					uint32 px = s->format.bytesPerPixel == 2 ? *(const uint16 *)s->getBasePtr(x, y)
						: *(const uint32 *)s->getBasePtr(x, y);
					s->format.colorToRGB(px, r, g, b);
				}
				out.writeByte(r); out.writeByte(g); out.writeByte(b);
			}
		}
		out.close();
		logf("shot %s", path.c_str());
	}
	g_system->unlockScreen();
}

// --------------------------------------------------------------- the dialog

enum {
	kGuideNextCmd = 'GNXT',
	kGuidePlayCmd = 'GPLY',
	kGuideNormalCmd = 'GNRM',
	kGuideHelpCmd = 'GHLP',
	kGuidePrevStepCmd = 'GPRV',
	kGuideSkipStepCmd = 'GSKP',
	kGuideCloseCmd = 'GCLS'
};

GuideDialog::GuideDialog(ScummGuide *guide)
	: GUI::Dialog(0, 0, 10, 10), _guide(guide) {
	const int W = g_system->getOverlayWidth(), H = g_system->getOverlayHeight();
	const int w = MIN(W - 20, 560), h = MIN(H - 20, 300);
	_x = (W - w) / 2;
	_y = (H - h) / 2;
	_w = w;
	_h = h;
	_backgroundType = GUI::ThemeEngine::kDialogBackgroundDefault;

	const int pad = 14, line = 22, bw = (w - pad * 2 - 10) / 2, bh = 26;
	new GUI::StaticTextWidget(this, pad, pad, w - pad * 2, line, _("Walkthrough"), Graphics::kTextAlignCenter);
	_where = new GUI::StaticTextWidget(this, pad, pad + line + 6, w - pad * 2, line, Common::U32String(), Graphics::kTextAlignLeft,
		Common::U32String(), GUI::ThemeEngine::kFontStyleNormal);
	_next = new GUI::StaticTextWidget(this, pad, pad + line * 2 + 6, w - pad * 2, line, Common::U32String(), Graphics::kTextAlignLeft);
	_after = new GUI::StaticTextWidget(this, pad, pad + line * 3 + 6, w - pad * 2, line, Common::U32String(), Graphics::kTextAlignLeft,
		Common::U32String(), GUI::ThemeEngine::kFontStyleNormal);

	int y = pad + line * 4 + 20;
	new GUI::ButtonWidget(this, pad, y, bw, bh, _("Do the next step"), _("Easy mode: the game does this one action for you"), kGuideNextCmd);
	new GUI::ButtonWidget(this, pad + bw + 10, y, bw, bh, _("Play to the end"), _("Movie mode: the game plays itself; open the menu to stop"), kGuidePlayCmd);
	y += bh + 8;
	new GUI::ButtonWidget(this, pad, y, bw, bh, _("Normal mode"), _("You play: stop the walkthrough"), kGuideNormalCmd);
	new GUI::ButtonWidget(this, pad + bw + 10, y, bw, bh, _("Controls help"), Common::U32String(), kGuideHelpCmd);
	y += bh + 8;
	new GUI::ButtonWidget(this, pad, y, bw / 2 - 4, bh, _("< Step back"), _("If the walkthrough is ahead of your game"), kGuidePrevStepCmd);
	new GUI::ButtonWidget(this, pad + bw / 2 + 4, y, bw / 2 - 4, bh, _("Step on >"), _("If you already did this step yourself"), kGuideSkipStepCmd);
	new GUI::ButtonWidget(this, pad + bw + 10, y, bw, bh, _("Close"), Common::U32String(), kGuideCloseCmd);
	refresh();
}

void GuideDialog::refresh() {
	const int cur = _guide->current(), n = _guide->stepCount();
	if (cur >= n) {
		_where->setLabel(Common::U32String::format("%d / %d", n, n));
		_next->setLabel(_("The walkthrough is finished."));
		_after->setLabel(Common::U32String());
		return;
	}
	_where->setLabel(Common::U32String(Common::String::format("%d / %d   ", cur + 1, n) + _guide->section(cur) +
		(_guide->isPlaying() ? "   (playing)" : "")));
	_next->setLabel(Common::U32String("> " + _guide->describe(cur)));
	_after->setLabel(cur + 1 < n ? Common::U32String("then: " + _guide->describe(cur + 1)) : Common::U32String());
}

void GuideDialog::handleCommand(GUI::CommandSender *sender, uint32 cmd, uint32 data) {
	switch (cmd) {
	case kGuideNextCmd: setResult(kNextStep); close(); break;
	case kGuidePlayCmd: setResult(kPlayToEnd); close(); break;
	case kGuideNormalCmd: setResult(kNormal); close(); break;
	case kGuideHelpCmd: setResult(kShowHelp); close(); break;
	case kGuideCloseCmd: setResult(kClosed); close(); break;
	case kGuidePrevStepCmd: _guide->setCurrent(_guide->current() - 1); refresh(); g_gui.scheduleTopDialogRedraw(); break;
	case kGuideSkipStepCmd: _guide->setCurrent(_guide->current() + 1); refresh(); g_gui.scheduleTopDialogRedraw(); break;
	default: GUI::Dialog::handleCommand(sender, cmd, data);
	}
}

} // End of namespace Scumm
