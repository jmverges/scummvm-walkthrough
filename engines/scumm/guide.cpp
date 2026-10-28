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
#include "scumm/scumm_v6.h"
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
	  _idleSince(0), _executing(false), _testNext(0), _testStart(0), _controlOffset(0), _liveSince(0), _minigameRoom(0), _minigameNext(0), _minigameStage(0), _minigameX(0), _minigameY(0), _talkPending(0), _untilSeenFalse(false) {
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
	// guide_control: a file to append commands to while the game runs (for
	// writing walkthroughs): the test script's commands, without the time,
	// plus "s <verb> <a> [<b>]" and "v <verbid>", done once the game is idle.
	if (ConfMan.hasKey("guide_control"))
		_controlPath = ConfMan.get("guide_control");
	if (ConfMan.hasKey("guide_trace_scripts"))
		_traceScripts = " " + ConfMan.get("guide_trace_scripts") + " ";
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

void ScummGuide::onRunScript(int script, const int *args) {
	// A script the input script starts by itself, other than the sentence
	// script (talking to someone in Sam & Max): recorded as an "x" step.
	if (!_executing && _vm->VAR_VERB_SCRIPT != 0xFF && _vm->_currentScript != 0xFF &&
		_vm->vm.slot[_vm->_currentScript].number == _vm->VAR(_vm->VAR_VERB_SCRIPT) &&
		(_vm->VAR_SENTENCE_SCRIPT == 0xFF || script != _vm->VAR(_vm->VAR_SENTENCE_SCRIPT))) {
		int a0 = args ? args[0] : 0, a1 = args ? args[1] : 0, a2 = args ? args[2] : 0;
		Common::String line = Common::String::format("x %d %d %d %d %d ; room %d", script,
			script >= _vm->_numGlobalScripts ? _vm->_currentRoom : 0, a0, a1, a2, _vm->_currentRoom);
		// Sam & Max: the room's talk script (var 194) for the actor in var 224.
		if (_vm->_game.id == GID_SAMNMAX && script == _vm->_scummVars[194])
			line = Common::String::format("t %d ; room %d", _vm->_scummVars[224], _vm->_currentRoom);
		logf("input script starts %s", line.c_str());
		if (!_recordPath.empty())
			appendLine(_recordPath, line);
	}
	if (_traceScripts.empty() || (_traceScripts != " * " && !_traceScripts.contains(Common::String::format(" %d ", script))))
		return;
	Common::String a;
	for (int i = 0; args && i < 6; i++)
		a += Common::String::format(" %d", args[i]);
	int caller = _vm->_currentScript != 0xFF ? _vm->vm.slot[_vm->_currentScript].number : -1;
	logf("script %d args%s (room %d, from %d)", script, a.c_str(), _vm->_currentRoom, caller);
}

void ScummGuide::onVerbClick(int verbid, int x, int y) {
	if (_executing)
		return;
	if (verbid <= 0) {
		// A click in the verb area that is on no verb: the game's scripts
		// find what is under the mouse themselves (the dialogue icons in
		// Sam & Max's inventory bar). Recorded as a click at that spot.
		logf("verb area click %d,%d (room %d)", x, y, _vm->_currentRoom);
		if (!_recordPath.empty())
			appendLine(_recordPath, Common::String::format("c %d %d %d ; room %d", _vm->_currentRoom, x, y, _vm->_currentRoom));
		return;
	}
	logf("verb click %d (room %d)", verbid, _vm->_currentRoom);
	if (_recordPath.empty())
		return;
	appendLine(_recordPath, Common::String::format("v %d ; room %d", verbid, _vm->_currentRoom));
}

void ScummGuide::onTalk(int actor, const char *text) {
	if (!ConfMan.getBool("guide_log_talk"))
		return;
	// The message holds escape codes (0xFF code [args]); the sound one (10)
	// carries 14 bytes that may include zeros. Keep the printable text.
	const byte *p = (const byte *)text;
	Common::String t;
	for (int i = 0; i < 512; ) {
		byte c = p[i];
		if (c == 0)
			break;
		if (c == 0xFF || c == 0xFE) {
			byte code = p[i + 1];
			if (code == 10 || code == 12)
				i += 2 + 14;
			else if (code == 4 || code == 5 || code == 6 || code == 7 || code == 9 || code == 13 || code == 14)
				i += 4;
			else
				i += 2;
			continue;
		}
		if (c >= 32)
			t += (char)c;
		i++;
	}
	logf("talk %d: %s", actor, t.c_str());
}

void ScummGuide::onInventoryClick(int object) {
	if (_executing || object <= 0)
		return;
	logf("inventory click %d '%s' (room %d)", object, objectName(object).c_str(), _vm->_currentRoom);
	if (!_recordPath.empty())
		appendLine(_recordPath, Common::String::format("i %d ; room %d: '%s'", object, _vm->_currentRoom, objectName(object).c_str()));
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
		st.args[0] = st.args[1] = st.args[2] = 0;
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
		} else if (line.hasPrefix("x ")) {
			st.kind = Step::kScript;
			parseScript(line, st);
		} else if (line.hasPrefix("c ")) {
			st.kind = Step::kClick;
			sscanf(line.c_str() + 2, "%d %d %d", &st.verb, &st.objectA, &st.objectB);
		} else if (line.hasPrefix("i ")) {
			st.kind = Step::kInventory;
			sscanf(line.c_str() + 2, "%d", &st.verb);
		} else if (line.hasPrefix("t ")) {
			st.kind = Step::kTalk;
			sscanf(line.c_str() + 2, "%d", &st.verb);
		} else if (line.hasPrefix("w ")) {
			st.kind = Step::kWait;
			sscanf(line.c_str() + 2, "%d", &st.verb);
		} else if (line.hasPrefix("u ")) {
			st.kind = Step::kUntil;
			sscanf(line.c_str() + 2, "%d %d %d", &st.verb, &st.objectA, &st.objectB);
		} else if (line.hasPrefix("g ")) {
			st.kind = Step::kGoTo;
			sscanf(line.c_str() + 2, "%d", &st.verb);
		} else if (line.hasPrefix("q ")) {
			st.kind = Step::kUntilClass;
			sscanf(line.c_str() + 2, "%d %d", &st.verb, &st.objectA);
		} else if (line.hasPrefix("o ")) {
			st.kind = Step::kUntilClass;
			char when[8] = "";
			sscanf(line.c_str() + 2, "%d %d %7s", &st.verb, &st.objectA, when);
			st.objectB = 1;
			st.args[0] = !strcmp(when, "now");
		} else if (line.hasPrefix("a ")) {
			st.kind = Step::kUntilClass;
			sscanf(line.c_str() + 2, "%d", &st.verb);
			st.objectB = 2;
		} else if (line.hasPrefix("m ")) {
			st.kind = Step::kMinigame;
			char name[32] = "";
			sscanf(line.c_str() + 2, "%31s", name);
			st.name = name;
		} else {
			continue;
		}
		_steps.push_back(st);
	}
	_current = 0;
	logf("%d steps in %s", _steps.size(), path.c_str());
}

void ScummGuide::parseScript(const Common::String &line, Step &st) {
	st.args[0] = st.args[1] = st.args[2] = 0;
	sscanf(line.c_str() + 2, "%d %d %d %d %d", &st.verb, &st.objectA, &st.args[0], &st.args[1], &st.args[2]);
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
	if (st.kind == Step::kWait || st.kind == Step::kUntil || st.kind == Step::kUntilClass || st.kind == Step::kMinigame)
		return true;
	if (st.kind == Step::kScript)
		return st.objectA == 0 || st.objectA == _vm->_currentRoom;
	if (st.kind == Step::kClick)
		return st.verb == _vm->_currentRoom;
	if (st.kind == Step::kInventory)
		return _vm->whereIsObject(st.verb) == WIO_INVENTORY;
	if (st.kind == Step::kGoTo)
		return _vm->whereIsObject(st.verb) == WIO_ROOM;
	if (st.kind == Step::kTalk) {
		Actor *a = _vm->derefActorSafe(st.verb, "guide");
		return a && a->isInCurrentRoom() && a->_visible;
	}
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
	} else if (st.kind == Step::kScript) {
		logf("step %d: script %d %d %d %d %s", _current + 1, st.verb, st.args[0], st.args[1], st.args[2], st.comment.c_str());
		runStepScript(st);
	} else if (st.kind == Step::kClick) {
		logf("step %d: click %d,%d in room %d %s", _current + 1, st.objectA, st.objectB, st.verb, st.comment.c_str());
		click(st.objectA, st.objectB);
	} else if (st.kind == Step::kInventory) {
		logf("step %d: inventory %d %s", _current + 1, st.verb, st.comment.c_str());
		_vm->runInputScript(kInventoryClickArea, st.verb, 0);
	} else if (st.kind == Step::kTalk) {
		logf("step %d: talk to actor %d %s", _current + 1, st.verb, st.comment.c_str());
		talkTo(st.verb);
	} else if (st.kind == Step::kGoTo) {
		logf("step %d: walk to %d %s", _current + 1, st.verb, st.comment.c_str());
		goTo(st.verb);
	} else if (st.kind == Step::kWait) {
		logf("step %d: wait %d s %s", _current + 1, st.verb, st.comment.c_str());
	} else if (st.kind == Step::kUntil) {
		logf("step %d: until actor %d x in %d-%d %s", _current + 1, st.verb, st.objectA, st.objectB, st.comment.c_str());
	} else if (st.kind == Step::kUntilClass) {
		logf("step %d: until %d has class %d %s", _current + 1, st.verb, st.objectA, st.comment.c_str());
		_untilSeenFalse = false;
	} else if (st.kind == Step::kMinigame) {
		logf("step %d: minigame %s %s", _current + 1, st.name.c_str(), st.comment.c_str());
		_minigameRoom = _vm->_currentRoom;
		_minigameNext = 0;
		_minigameStage = 0;
	} else {
		logf("step %d: verb %d %s", _current + 1, st.verb, st.comment.c_str());
		_vm->runInputScript(kVerbClickArea, st.verb, 1);
	}
	_executing = false;
}

void ScummGuide::click(int x, int y) {
	Common::EventManager *em = g_system->getEventManager();
	Common::Event ev;
	ev.mouse = Common::Point(x, y);
	ev.type = Common::EVENT_MOUSEMOVE;
	em->pushEvent(ev);
	g_system->warpMouse(x, y);
	ev.type = Common::EVENT_LBUTTONDOWN;
	em->pushEvent(ev);
	ev.type = Common::EVENT_LBUTTONUP;
	em->pushEvent(ev);
}

// Sam & Max: the input script talks to the actor under the mouse when the
// cursor (var 177) is the mouth (877). Point it at the actor for one call.
static const int kSnmCursorVar = 177, kSnmTalkCursor = 877;

void ScummGuide::talkTo(int actor) {
	Actor *a = _vm->derefActorSafe(actor, "guide");
	if (!a)
		return;
	// A point inside the actor's picture: its feet are at getRealPos().
	Common::Point p = a->getRealPos();
	int y = -1;
	for (int dy = 10; dy <= 70; dy += 10) {
		if (_vm->getActorFromPos(p.x, p.y - dy) == actor) {
			y = p.y - dy;
			break;
		}
	}
	if (y >= 0) {
		// What a click with the mouth cursor does, through the input script.
		const int oldCursor = _vm->VAR(kSnmCursorVar);
		const int oldX = _vm->VAR(_vm->VAR_VIRT_MOUSE_X), oldY = _vm->VAR(_vm->VAR_VIRT_MOUSE_Y);
		_vm->VAR(kSnmCursorVar) = kSnmTalkCursor;
		_vm->VAR(_vm->VAR_VIRT_MOUSE_X) = p.x;
		_vm->VAR(_vm->VAR_VIRT_MOUSE_Y) = y;
		_vm->runInputScript(kSceneClickArea, 0, 1);
		_vm->VAR(kSnmCursorVar) = oldCursor;
		_vm->VAR(_vm->VAR_VIRT_MOUSE_X) = oldX;
		_vm->VAR(_vm->VAR_VIRT_MOUSE_Y) = oldY;
		return;
	}
	// The actor is not drawn where the engine can hit-test it (off screen,
	// or not yet drawn): do what the input script would, here. Walk to the
	// room's talking spot for that actor (arrays 185/186) or to the actor,
	// then face it and start the room's talk script (var 194) with var 224.
	logf("talk: actor %d not under a point, talking directly", actor);
	Actor *ego = _vm->derefActorSafe(_vm->VAR(_vm->VAR_EGO), "guide");
	ScummEngine_v6 *v6 = (ScummEngine_v6 *)_vm;
	int tx = v6->readArray(185, 0, actor), ty = v6->readArray(186, 0, actor);
	if (!ty) {
		tx = p.x + (p.x > ego->getRealPos().x ? -40 : 40);
		ty = p.y;
	}
	_vm->_scummVars[224] = actor;
	ego->startWalkActor(tx, ty, -1);
	_talkPending = actor;
}

// Minigames, played the way a player would (their own input), each frame.
bool ScummGuide::playMinigame(const Step &st) {
	uint32 now = g_system->getMillis();
	if (st.name == "vortex" && _minigameStage < 2) {
		// Sam & Max, the Mystery Vortex: Shuv-Oohl's den is behind one of
		// the four doors upstairs (345-348, var 252, chosen at random), and
		// a door can be reached only when the magnets' levers downstairs
		// (room 38: red 359, green 360, blue 361, worth 1, 2 and 4) add up
		// to that door's entry of array 250, also random. Starts downstairs.
		if (now < _minigameNext || !isIdle())
			return false;
		if (_minigameStage == 0 && _vm->_currentRoom == 38) {
			int want = ((ScummEngine_v6 *)_vm)->readArray(250, 0, _vm->_scummVars[252] - 345);
			for (int i = 0; i < 3; i++)
				if (_vm->getState(359 + i) != ((want >> i) & 1)) {
					logf("vortex: lever %d (door %d needs %d)", 359 + i, _vm->_scummVars[252], want);
					_vm->doSentence(3, 359 + i, 0);
					_minigameNext = now + 1500;
					return false;
				}
			_vm->doSentence(7, 355, 0);
			_minigameStage = 1;
			_minigameNext = now + 1500;
		} else if (_minigameStage == 1 && _vm->_currentRoom == 37) {
			logf("vortex: opening door %d", _vm->_scummVars[252]);
			_vm->doSentence(3, _vm->_scummVars[252], 0);
			_minigameStage = 2;
			_minigameRoom = 37;
			_minigameNext = now + 1500;
		}
		return false;
	}
	if (_vm->_currentRoom != _minigameRoom)
		return isIdle();
	if (now < _minigameNext)
		return false;
	if (st.name == "wakarat") {
		// Sam & Max, Wak-a-Rat (room 18): rats are actors 6-10 and can be hit
		// while they do not have class 6; script 202 is the hit on a hole.
		// Var 151 is set while the hammer swings; a hit then is lost.
		if (_vm->_scummVars[151])
			return false;
		for (int actor = 6; actor <= 10; actor++) {
			// setClass(rat, [134]) (bit 7: set) puts class 6 on a rat that is
			// down; the script's ifClassOfIs(rat, [6]) is true once it is off.
			if (_vm->getClass(actor, 6))
				continue;
			// The game's own hit (what a click on the rat's hole starts),
			// started this frame: a click would only land on the next one.
			int args[NUM_SCRIPT_LOCAL];
			memset(args, 0, sizeof(args));
			args[0] = actor - 4;
			_vm->runScript(202, false, true, args);
			_minigameNext = now + 60;
			logf("wakarat: rat %d up (hits %d, rats %d)", actor, _vm->_scummVars[153], _vm->_scummVars[152]);
			break;
		}
	} else if (st.name == "wires") {
		// Sam & Max, the robot's brain (room 50): the jacks start plugged or
		// loose at random. A loose one (466-470) shows in state 1 and its
		// square on the map (461-465) is red; plug them all until the map is
		// green.
		bool green = true;
		for (int i = 0; i < 5 && green; i++)
			green = _vm->getState(461 + i) == 0;
		if (green) {
			logf("wires: the map is green");
			return true;
		}
		for (int i = 0; i < 5; i++) {
			int obj = 466 + i;
			if (_vm->getState(obj) != 1)
				continue;
			for (int k = 1; k < _vm->_numLocalObjects; k++) {
				const ObjectData &o = _vm->_objs[k];
				if (o.obj_nr != obj)
					continue;
				// A point of the jack that no other wire covers.
				for (int y = o.y_pos + 2; y < o.y_pos + o.height; y += 4)
					for (int x = o.x_pos + 2; x < o.x_pos + o.width; x += 4)
						if (_vm->findObject(x, y) == obj) {
							logf("wires: plugging %d at %d,%d", obj, x, y);
							click(x - _vm->_screenStartStrip * 8, y);
							_minigameNext = now + 800;
							return false;
						}
			}
		}
		return false;
	} else if (st.name == "frogrock") {
		// Sam & Max, the Ball of Twine's viewer (room 78): the scenery
		// scrolls past (array 246; var 243 is the piece in the middle, var
		// 244 the one Shuv-Oohl's Frog Rock is). The lever (object 305) sets
		// the speed, var 245: 1-2 one way, 3 stopped, 4-5 the other way; a
		// right click raises it, a left click lowers it. Go slowly, stop on
		// Frog Rock, then leave.
		int speed = _vm->_scummVars[245], here = _vm->_scummVars[243], rock = _vm->_scummVars[244];
		int lx = 0, ly = 0;
		for (int i = 1; i < _vm->_numLocalObjects; i++)
			if (_vm->_objs[i].obj_nr == 305) {
				lx = _vm->_objs[i].x_pos + _vm->_objs[i].width / 2 - _vm->_screenStartStrip * 8;
				ly = _vm->_objs[i].y_pos + _vm->_objs[i].height / 2;
			}
		if (_minigameStage == 0) {
			if (speed < 2 || speed == 3 || speed > 3) {
				// From any speed to 2: slow, toward the higher pieces.
				Common::Event ev;
				ev.mouse = Common::Point(lx, ly);
				g_system->warpMouse(lx, ly);
				ev.type = Common::EVENT_MOUSEMOVE;
				g_system->getEventManager()->pushEvent(ev);
				ev.type = speed < 2 ? Common::EVENT_RBUTTONDOWN : Common::EVENT_LBUTTONDOWN;
				g_system->getEventManager()->pushEvent(ev);
				ev.type = speed < 2 ? Common::EVENT_RBUTTONUP : Common::EVENT_LBUTTONUP;
				g_system->getEventManager()->pushEvent(ev);
				_minigameNext = now + 400;
				return false;
			}
			if (here == rock) {
				Common::Event ev;
				ev.mouse = Common::Point(lx, ly);
				ev.type = Common::EVENT_RBUTTONDOWN;
				g_system->getEventManager()->pushEvent(ev);
				ev.type = Common::EVENT_RBUTTONUP;
				g_system->getEventManager()->pushEvent(ev);
				logf("frogrock: stopping on piece %d", here);
				_minigameStage = 1;
				_minigameNext = now + 1500;
			}
			return false;
		}
		if (_minigameStage == 1) {
			if (speed != 3)
				return false;
			if (here != rock) {   // stopped one off: go again
				logf("frogrock: stopped on %d, not %d", here, rock);
				_minigameStage = 0;
				return false;
			}
			_minigameStage = 2;
			_minigameNext = now + 8000;   // Sam says it is Frog Rock
			return false;
		}
		if (_minigameStage == 2 && isIdle()) {
			Common::Event ev;
			ev.kbd.keycode = Common::KEYCODE_ESCAPE;
			ev.kbd.ascii = 27;
			ev.type = Common::EVENT_KEYDOWN;
			g_system->getEventManager()->pushEvent(ev);
			ev.type = Common::EVENT_KEYUP;
			g_system->getEventManager()->pushEvent(ev);
			_minigameStage = 3;
			_minigameNext = now + 2000;
		}
	} else if (st.name == "gatorgolf") {
		// Sam & Max, Gator Golf (room 25): gators 11-15 swim in rows 1-5
		// (vars 309-313: position 1 left, 2 middle, 3 right; x of each in
		// arrays 314-318, the row's y in array 319). A fish landing in a
		// row's water moves its gator one place toward it (two past the
		// middle). The bridge: every gator in the middle, except the third,
		// which only must not be on the left. Aim beside the middle on the
		// gator's side, the way a player would: point (target), then swing.
		ScummEngine_v6 *v6 = (ScummEngine_v6 *)_vm;
		if (_vm->vm.cutSceneStackPointer || _vm->isScriptRunning(79) || _vm->isScriptRunning(200) ||
			_vm->isScriptRunning(205) || _vm->isScriptRunning(206))
			return false;
		if (_vm->VAR(_vm->VAR_VERB_SCRIPT) != 214)   // golf mode is over: Max is out
			return isIdle();
		int row = 0;
		for (int r = 1; r <= 5 && !row; r++) {
			int pos = _vm->_scummVars[308 + r];
			if (r == 3 ? pos == 1 : pos != 2)
				row = r;
		}
		if (!row)
			return false;
		if (_minigameStage == 0) {
			Actor *g11 = _vm->derefActorSafe(11, "guide");
			logf("gatorgolf: gators %d %d %d %d %d, gator 11 x %d", _vm->_scummVars[309], _vm->_scummVars[310], _vm->_scummVars[311], _vm->_scummVars[312],
				_vm->_scummVars[313], g11 ? g11->getRealPos().x : -1);
			int pos = _vm->_scummVars[308 + row];
			int mid = v6->readArray(313 + row, 0, 2);
			int y = v6->readArray(319, 0, row);
			// A gator's picture reaches right from where it stands, so on the
			// left bank there is no water between it and the middle: a fish on
			// the gator itself sends it to the right bank. From there, a fish in
			// the water just right of the middle brings it to the middle.
			Actor *g = _vm->derefActorSafe(10 + row, "guide");
			int gx = g ? g->getRealPos().x : mid;
			// The middle of the gator's picture (it bobs, so not an edge).
			// The fish has to land on the gator (it then swims to the other
			// bank) and the same fish then counts as landing in the water,
			// which pulls it back toward the fish: land it on the gator on the
			// far side of the middle and the gator ends in the middle. The
			// pictures are long enough to reach across.
			int x = gx, n = 0, sx = 0, sy = 0;
			for (int cy = y - 30; cy <= y + 4; cy += 2)
				for (int cx = gx - 160; cx <= gx + 160; cx += 2) {
					if (pos == 1 ? cx < mid + 6 : cx > mid - 6)
						continue;
					if (_vm->getActorFromPos(cx + 1, cy + 1) == 10 + row && _vm->getActorFromPos(cx - 1, cy - 1) == 10 + row) {
						n++;
						sx += cx;
						sy += cy;
					}
				}
			if (n) {
				x = sx / n;
				y = sy / n;
			}
			_minigameX = x;
			_minigameY = y;
			Common::Event ev;
			ev.mouse = Common::Point(x, y);
			ev.type = Common::EVENT_MOUSEMOVE;
			g_system->getEventManager()->pushEvent(ev);
			g_system->warpMouse(x, y);
			_minigameStage = 1;
			_minigameNext = now + 250;
		} else if (_minigameStage == 1) {
			click(_minigameX, _minigameY);   // the target
			_minigameStage = 2;
			_minigameNext = now + 500;
		} else {
			Actor *g = _vm->derefActorSafe(10 + row, "guide");
			logf("gatorgolf: row %d gator at %d (x %d; places %d %d %d), fish to %d,%d, tee %d, target %d,%d, cursor %d, there: actor %d object %d",
				row, _vm->_scummVars[308 + row], g ? g->getRealPos().x : -1, v6->readArray(313 + row, 0, 1), v6->readArray(313 + row, 0, 2),
				v6->readArray(313 + row, 0, 3), _minigameX, _minigameY, _vm->_scummVars[249], _vm->_scummVars[328], _vm->_scummVars[329], _vm->_scummVars[177],
				_vm->getActorFromPos(_minigameX + 1, _minigameY + 1), _vm->findObject(_minigameX, _minigameY));
			// The aim, as the target click sets it (the click's mouse
			// position reaches the scripts a frame late, so set it here).
			_vm->_scummVars[328] = _minigameX;
			_vm->_scummVars[329] = _minigameY;
			click(28, 176);                  // the swing button
			_minigameStage = 0;
			_minigameNext = now + 1500;
		}
	}
	return false;
}

void ScummGuide::goTo(int object) {
	// Where a sentence on the object would walk the player's actor.
	Actor *ego = _vm->derefActorSafe(_vm->VAR(_vm->VAR_EGO), "guide");
	if (!ego)
		return;
	int x, y, dir;
	_vm->getObjectXYPos(object, x, y, dir);
	ego->startWalkActor(x, y, dir);
}

void ScummGuide::runStepScript(const Step &st) {
	int args[NUM_SCRIPT_LOCAL];
	memset(args, 0, sizeof(args));
	args[0] = st.args[0];
	args[1] = st.args[1];
	args[2] = st.args[2];
	_vm->runScript(st.verb, false, false, args);
}

void ScummGuide::tick() {
	runTestScript();
	pollControl();
	if (_talkPending) {
		Actor *ego = _vm->derefActorSafe(_vm->VAR(_vm->VAR_EGO), "guide");
		if (ego && !ego->_moving) {
			Actor *a = _vm->derefActorSafe(_talkPending, "guide");
			if (a)
				ego->faceToObject(_talkPending);
			_vm->runScript(_vm->_scummVars[194], false, false, nullptr);
			_talkPending = 0;
		}
		return;
	}
	runLive();
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

	if (_state == kRunning && _current < (int)_steps.size() && _steps[_current].kind == Step::kMinigame) {
		if (playMinigame(_steps[_current])) {
			logf("step %d done (minigame over, room %d)", _current + 1, _vm->_currentRoom);
			setCurrent(_current + 1);
			if (_remaining > 0)
				_remaining--;
			_state = kWaitIdle;
		}
		return;
	}
	if (_state == kRunning && _current < (int)_steps.size() && _steps[_current].kind == Step::kUntil) {
		const Step &u = _steps[_current];
		Actor *a = _vm->derefActorSafe(u.verb, "guide");
		if (a && a->getRealPos().x > u.objectA && a->getRealPos().x < u.objectB) {
			logf("step %d done (actor %d at x %d)", _current + 1, u.verb, a->getRealPos().x);
			setCurrent(_current + 1);
			if (_remaining > 0)
				_remaining--;
			_state = kWaitIdle;
			_idleSince = now - kIdleSettle;   // the next step goes at once
		}
		return;
	}
	if (_state == kRunning && _current < (int)_steps.size() && _steps[_current].kind == Step::kUntilClass) {
		const Step &u = _steps[_current];
		// As the scripts' ifClassOfIs(obj, [class]): true when getClass() is
		// false for a plain class, true for class + 128.
		// It must become true while waiting (the sky car must arrive, not be
		// about to leave), so a condition already true is waited out first.
		bool holds;
		if (u.objectB == 2) {   // "a": the actor here and standing still
			Actor *a = _vm->derefActorSafe(u.verb, "guide");
			holds = a && a->isInCurrentRoom() && !a->_moving;
			if (holds)
				_untilSeenFalse = true;   // no edge needed
		} else if (u.objectB) {   // "o": the object's state
			holds = _vm->getState(u.verb) == u.objectA;
		} else {
			bool b = _vm->getClass(u.verb, u.objectA & 0x7F);
			holds = (u.objectA & 0x80) ? b : !b;
		}
		if (!holds)
			_untilSeenFalse = true;
		if (holds && _untilSeenFalse) {
			logf("step %d done (%d has class %d, idle %d)", _current + 1, u.verb, u.objectA, isIdle());
			setCurrent(_current + 1);
			if (_remaining > 0)
				_remaining--;
			// The moment is short (a sky car at its stop): the next step goes
			// at once, without waiting for the game to settle.
			if (_remaining != 0 && _current < (int)_steps.size() && available(_steps[_current])) {
				execute(_steps[_current]);
				_state = kRunning;
				_stateSince = now;
				_idleSince = 0;
			} else {
				_state = kWaitIdle;
			}
		}
		return;
	}
	if (_state == kRunning && _current < (int)_steps.size() && _steps[_current].kind == Step::kWait) {
		if (now - _stateSince >= (uint32)_steps[_current].verb * 1000) {
			logf("step %d done (waited)", _current + 1);
			setCurrent(_current + 1);
			if (_remaining > 0)
				_remaining--;
			_state = kWaitIdle;
		}
		return;
	}
	if (_state == kRunning && _current + 1 < (int)_steps.size() && _steps[_current + 1].kind == Step::kUntilClass &&
			_steps[_current + 1].args[0] == 1) {
		// "o ... now": the next step watches for a moment inside what this
		// one started (a speech that never settles), so it begins at once.
		logf("step %d done (the next one watches)", _current + 1);
		setCurrent(_current + 1);
		if (_remaining > 0)
			_remaining--;
		if (_remaining != 0) {
			execute(_steps[_current]);
			_state = kRunning;
			_stateSince = now;
			_idleSince = 0;
		} else {
			_state = kWaitIdle;
		}
		return;
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
		} else if (now - _stateSince > kStepTimeout * 20) {   // Bumpus's show runs two minutes
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
	while (_testNext < _test.size() && now - _testStart >= _test[_testNext].at) {
		const TestEvent &t = _test[_testNext++];
		runCommand(t);
	}
}

void ScummGuide::runCommand(const TestEvent &t) {
	Common::EventManager *em = g_system->getEventManager();
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
		if (_vm->_game.id == GID_SAMNMAX)
			logf("talk script (var 194) %d, cursor (var 177) %d", _vm->_scummVars[194], _vm->_scummVars[177]);
		logf("userput %d, cursor state %d, sentences %d, cutscene %d, sentence script running %d",
			_vm->_userPut, _vm->_cursor.state, _vm->_sentenceNum, _vm->vm.cutSceneStackPointer,
			_vm->isScriptRunning(_vm->VAR(_vm->VAR_SENTENCE_SCRIPT)));
		{
			Common::String running;
			for (int i = 0; i < NUM_SCRIPT_SLOT; i++)
				if (_vm->vm.slot[i].status != ssDead)
					running += Common::String::format(" %d@%X", _vm->vm.slot[i].number, _vm->vm.slot[i].offs);
			logf("running scripts:%s", running.c_str());
		}
		logf("room %d, ego %d, idle %d, screen left %d", _vm->_currentRoom,
			_vm->VAR_EGO != 0xFF ? _vm->VAR(_vm->VAR_EGO) : -1, isIdle(), _vm->_screenStartStrip * 8);
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
		for (int i = 1; i < _vm->_numActors; i++) {
			Actor *a = _vm->derefActorSafe(i, "guide");
			if (a && a->isInCurrentRoom()) {
				Common::String verbs;
				for (int v = 1; v < 256; v++)
					if (_vm->getVerbEntrypoint(i, v))
						verbs += Common::String::format(" %d", v);
				logf("  actor %d '%s' at %d,%d%s class %x verbs:%s", i, objectName(i).c_str(), a->getRealPos().x, a->getRealPos().y, a->_visible ? "" : " hidden", _vm->_classData[i],
					verbs.c_str());
			}
		}
		for (int i = 1; i < _vm->_numVerbs; i++)
			if (_vm->_verbs[i].verbid && _vm->_verbs[i].curmode)
				logf("  verb slot %d id %d mode %d", i, _vm->_verbs[i].verbid, _vm->_verbs[i].curmode);
		for (int i = 0; i < _vm->_numInventory; i++)
			if (_vm->_inventory[i])
				logf("  carried %d '%s' owner %d", _vm->_inventory[i], objectName(_vm->_inventory[i]).c_str(),
					_vm->getOwner(_vm->_inventory[i]));
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

// ------------------------------------------------------- live control file

void ScummGuide::pollControl() {
	if (_controlPath.empty())
		return;
	Common::SeekableReadStream *in = openPath(_controlPath);
	if (!in)
		return;
	if (in->size() < (int64)_controlOffset)
		_controlOffset = 0;    // the file was truncated: start again
	if (in->size() == (int64)_controlOffset) {
		delete in;
		return;
	}
	in->seek(_controlOffset);
	Common::String text = in->readString(0, in->size() - _controlOffset);
	// Only whole lines; a half-written one waits for the next poll.
	size_t lastNl = text.findLastOf('\n');
	delete in;
	if (lastNl == Common::String::npos)
		return;
	_controlOffset += lastNl + 1;
	text = text.substr(0, lastNl);
	size_t pos = 0;
	while (pos <= text.size()) {
		size_t nl = text.find('\n', pos);
		Common::String line = text.substr(pos, nl == Common::String::npos ? Common::String::npos : nl - pos);
		pos = nl == Common::String::npos ? text.size() + 1 : nl + 1;
		line.trim();
		if (line.empty() || line.hasPrefix("#"))
			continue;
		Step st;
		st.verb = st.objectA = st.objectB = 0;
		st.args[0] = st.args[1] = st.args[2] = 0;
		if (line.hasPrefix("s ")) {
			st.kind = Step::kSentence;
			sscanf(line.c_str() + 2, "%d %d %d", &st.verb, &st.objectA, &st.objectB);
			_live.push_back(st);
			continue;
		}
		if (line.hasPrefix("v ")) {
			st.kind = Step::kVerb;
			sscanf(line.c_str() + 2, "%d", &st.verb);
			_live.push_back(st);
			continue;
		}
		if (line.hasPrefix("x ")) {
			st.kind = Step::kScript;
			parseScript(line, st);
			_live.push_back(st);
			continue;
		}
		if (line.hasPrefix("c ")) {
			st.kind = Step::kClick;
			sscanf(line.c_str() + 2, "%d %d %d", &st.verb, &st.objectA, &st.objectB);
			_live.push_back(st);
			continue;
		}
		if (line.hasPrefix("i ")) {
			st.kind = Step::kInventory;
			sscanf(line.c_str() + 2, "%d", &st.verb);
			_live.push_back(st);
			continue;
		}
		if (line.hasPrefix("g ")) {
			st.kind = Step::kGoTo;
			sscanf(line.c_str() + 2, "%d", &st.verb);
			_live.push_back(st);
			continue;
		}
		if (line.hasPrefix("t ")) {
			st.kind = Step::kTalk;
			sscanf(line.c_str() + 2, "%d", &st.verb);
			_live.push_back(st);
			continue;
		}
		if (line.hasPrefix("q ") || line.hasPrefix("o ") || line.hasPrefix("a ")) {
			Step qs;
			qs.kind = Step::kUntilClass;
			qs.verb = qs.objectA = qs.objectB = 0;
			sscanf(line.c_str() + 2, "%d %d", &qs.verb, &qs.objectA);
			qs.objectB = line.hasPrefix("o ") ? 1 : line.hasPrefix("a ") ? 2 : 0;
			_steps.insert_at(_current, qs);
			play(1);
			continue;
		}
		if (line.hasPrefix("u ")) {
			Step us;
			us.kind = Step::kUntil;
			us.verb = us.objectA = us.objectB = 0;
			sscanf(line.c_str() + 2, "%d %d %d", &us.verb, &us.objectA, &us.objectB);
			_steps.insert_at(_current, us);
			play(1);
			continue;
		}
		if (line.hasPrefix("m ") || line.hasPrefix("w ")) {
			// Minigames and waits need the player's state machine: run them
			// as a one-step walkthrough of their own.
			Step ms;
			ms.kind = line.hasPrefix("m ") ? Step::kMinigame : Step::kWait;
			ms.verb = ms.objectA = ms.objectB = 0;
			char name[32] = "";
			sscanf(line.c_str() + 2, "%31s", name);
			ms.name = name;
			ms.verb = atoi(name);
			_steps.insert_at(_current, ms);
			play(1);
			continue;
		}
		if (line == "reload") {
			int cur = _current;
			if (ConfMan.hasKey("guide_file"))
				load(ConfMan.get("guide_file"));
			_current = CLIP<int>(cur, 0, _steps.size());
			continue;
		}
		TestEvent ev;
		char cmd[32] = "", arg[128] = "";
		ev.at = 0;
		ev.x = ev.y = 0;
		sscanf(line.c_str(), "%31s %127s", cmd, arg);
		ev.cmd = cmd;
		ev.arg = arg;
		sscanf(line.c_str(), "%*s %d %d", &ev.x, &ev.y);
		logf("control: %s", line.c_str());
		runCommand(ev);
	}
}

// Sentences and verb clicks from the control file, one at a time, each when
// the game is idle, as the walkthrough's own steps are.
void ScummGuide::runLive() {
	if (_live.empty())
		return;
	uint32 now = g_system->getMillis();
	if (!isIdle()) {
		_liveSince = 0;
		return;
	}
	if (!_liveSince) {
		_liveSince = now;
		return;
	}
	if (now - _liveSince < kIdleSettle)
		return;
	Step st = _live.front();
	_live.remove_at(0);
	_executing = true;
	if (st.kind == Step::kSentence) {
		logf("live: sentence %d %d %d (room %d, %s / %s)", st.verb, st.objectA, st.objectB, _vm->_currentRoom,
			objectName(st.objectA).c_str(), objectName(st.objectB).c_str());
		_vm->doSentence(st.verb, st.objectA, st.objectB);
	} else if (st.kind == Step::kScript) {
		logf("live: script %d %d %d %d (room %d)", st.verb, st.args[0], st.args[1], st.args[2], _vm->_currentRoom);
		runStepScript(st);
	} else if (st.kind == Step::kClick) {
		logf("live: click %d,%d (room %d)", st.objectA, st.objectB, _vm->_currentRoom);
		click(st.objectA, st.objectB);
	} else if (st.kind == Step::kGoTo) {
		logf("live: walk to %d (room %d)", st.verb, _vm->_currentRoom);
		goTo(st.verb);
	} else if (st.kind == Step::kInventory) {
		logf("live: inventory %d '%s' (room %d)", st.verb, objectName(st.verb).c_str(), _vm->_currentRoom);
		_vm->runInputScript(kInventoryClickArea, st.verb, 0);
	} else if (st.kind == Step::kTalk) {
		logf("live: talk to actor %d (room %d)", st.verb, _vm->_currentRoom);
		talkTo(st.verb);
	} else {
		logf("live: verb %d (room %d)", st.verb, _vm->_currentRoom);
		_vm->runInputScript(kVerbClickArea, st.verb, 1);
	}
	_executing = false;
	_liveSince = 0;
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
