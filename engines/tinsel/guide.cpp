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

#include "tinsel/guide.h"

#include "common/algorithm.h"
#include "common/config-manager.h"
#include "common/coroutines.h"
#include "common/events.h"
#include "common/file.h"
#include "common/fs.h"
#include "common/system.h"
#include "graphics/palette.h"
#include "graphics/paletteman.h"
#include "graphics/surface.h"
#include "image/png.h"

#include "tinsel/actors.h"
#include "tinsel/cursor.h"
#include "tinsel/dialogs.h"
#include "tinsel/events.h"
#include "tinsel/movers.h"
#include "tinsel/pid.h"
#include "tinsel/pcode.h"
#include "tinsel/polygons.h"
#include "tinsel/scroll.h"
#include "tinsel/sound.h"
#include "tinsel/strres.h"
#include "tinsel/timers.h"
#include "tinsel/tinsel.h"

namespace Tinsel {

void WalkTo(int x, int y);	// events.cpp

extern SCNHANDLE GetSceneHandle();

static const uint32 kIdleSettle = 600;      // ms the game must stay idle before the next step
static const uint32 kStepTimeout = 90000;
static const uint32 kQuietDone = 10000;    // ms with control on and nobody talking that end a step whose code goes on
static const uint32 kQuietLong = 15000;    // the same while a sample plays (a long line, or a looping effect)   // ms a step's target may be missing before giving up

static const char *eventName(int event) {
	switch (event) {
	case WALKTO: return "walk";
	case ACTION: return "act";
	case LOOK: return "look";
	default: return "?";
	}
}

static int eventByName(const Common::String &s) {
	if (s == "walk") return WALKTO;
	if (s == "act") return ACTION;
	if (s == "look") return LOOK;
	return -1;
}

static PLR_EVENT buttonFor(int event) {
	return event == ACTION ? PLR_DLEFT : event == LOOK ? PLR_SRIGHT : PLR_SLEFT;
}

static int sceneNumber() {
	return GetSceneHandle() >> SCNHANDLE_SHIFT;
}

TinselGuide::TinselGuide(TinselEngine *vm) : _vm(vm), _current(0), _remaining(0), _state(kWaitIdle),
		_stateSince(0), _idleSince(0), _liveSince(0), _hideCursor(false), _logTalk(false), _executing(false),
		_saveSections(0), _sectionSaved(-1), _controlOffset(0) {
	_hideCursor = ConfMan.getBool("guide_hide_cursor");
	_logTalk = ConfMan.getBool("guide_log_talk");
	if (ConfMan.hasKey("guide_save_sections"))
		_saveSections = ConfMan.getInt("guide_save_sections");
	if (ConfMan.hasKey("guide_record"))
		_recordPath = ConfMan.get("guide_record");
	_shotDir = ConfMan.hasKey("guide_shots") ? ConfMan.get("guide_shots") : ".";
	if (ConfMan.hasKey("guide_control")) {
		_controlPath = ConfMan.get("guide_control");
		Common::FSNode node(Common::Path(_controlPath, '/'));
		Common::SeekableReadStream *in = node.createReadStream();
		if (in) {
			_controlOffset = in->size();   // only commands written from now on
			delete in;
		}
	}
	if (ConfMan.hasKey("guide_file"))
		load(ConfMan.get("guide_file"));
	if (ConfMan.hasKey("guide_step"))
		_current = CLIP<int>(ConfMan.getInt("guide_step"), 0, _steps.size());
}

void TinselGuide::logf(const char *fmt, ...) {
	va_list va;
	va_start(va, fmt);
	Common::String s = Common::String::vformat(fmt, va);
	va_end(va);
	g_system->logMessage(LogMessageType::kInfo, ("guide: " + s + "\n").c_str());
}

// ------------------------------------------------------------ the file

bool TinselGuide::parseLine(const Common::String &text, Step &st) {
	Common::String line = text;
	st.comment.clear();
	int semi = line.findFirstOf(';');
	if (semi >= 0) {
		st.comment = line.substr(semi + 1);
		st.comment.trim();
		line = line.substr(0, semi);
	}
	line.trim();
	st.optional = st.quick = false;
	while (line.hasPrefix("?") || line.hasPrefix("!")) {
		(line[0] == '?' ? st.optional : st.quick) = true;
		line = line.substr(1);
		line.trim();
	}
	if (line.size() < 2)
		return false;
	char kind = line[0];
	Common::String rest = line.substr(2);
	char a[16] = "", b[16] = "";
	int n1 = 0, n2 = 0;
	st.event = WALKTO;
	st.actor = false;
	st.target = st.item = st.x = st.y = 0;
	switch (kind) {
	case 'e':
		if (sscanf(rest.c_str(), "%15s %15s %d", a, b, &n1) < 2 || (b[0] != 'P' && b[0] != 'A'))
			return false;
		st.kind = Step::kEvent;
		st.event = eventByName(a);
		st.actor = b[0] == 'A';
		st.target = atoi(b + 1);
		st.item = n1;
		return st.event >= 0;
	case 'i':
		if (sscanf(rest.c_str(), "%15s %d %d", a, &n1, &n2) < 2)
			return false;
		st.kind = Step::kInventory;
		st.event = eventByName(a);
		st.target = n1;
		st.item = n2;
		return st.event >= 0;
	case 'k':
		st.kind = Step::kConverse;
		st.target = atoi(rest.c_str());
		return true;
	case 'h':
		st.kind = Step::kHold;
		st.item = atoi(rest.c_str());
		return true;
	case 'a':
		st.kind = Step::kSceneWait;
		st.target = atoi(rest.c_str());
		return st.target > 0;
	case 'n':
		if (sscanf(rest.c_str(), "%d %d", &n1, &n2) != 2)
			return false;
		st.kind = Step::kVarWait;
		st.target = n1;
		st.x = n2;
		return true;
	case 'p':
		st.kind = Step::kPutDown;
		st.target = atoi(rest.c_str());
		return st.target == INV_1 || st.target == INV_2;
	case 'c':
	case 'r':
		if (sscanf(rest.c_str(), "%d %d", &n1, &n2) != 2)
			return false;
		st.kind = kind == 'c' ? Step::kClick : Step::kRightClick;
		st.x = n1;
		st.y = n2;
		return true;
	case 'g':
		if (sscanf(rest.c_str(), "%d %d", &n1, &n2) != 2)
			return false;
		st.kind = Step::kWalk;
		st.x = n1;
		st.y = n2;
		return true;
	case 'w':
		st.kind = Step::kWait;
		st.target = atoi(rest.c_str());
		return true;
	default:
		return false;
	}
}

bool TinselGuide::load(const Common::String &path) {
	Common::FSNode node(Common::Path(path, '/'));
	Common::SeekableReadStream *in = node.createReadStream();
	if (!in) {
		logf("%s not found", path.c_str());
		return false;
	}
	_steps.clear();
	Common::String section;
	while (!in->eos()) {
		Common::String line = in->readLine();
		line.trim();
		if (line.hasPrefix("==")) {
			section = line.substr(2);
			section.trim();
			continue;
		}
		if (line.empty() || line.hasPrefix("#"))
			continue;
		Step st;
		if (parseLine(line, st)) {
			st.section = section;
			_steps.push_back(st);
		} else {
			logf("can't read: %s", line.c_str());
		}
	}
	delete in;
	logf("%d steps in %s", _steps.size(), path.c_str());
	return true;
}

void TinselGuide::play(int count) {
	_remaining = count;
	_state = kWaitIdle;
	_idleSince = 0;
	logf("play from step %d (%s)", _current + 1, count < 0 ? "to the end" : "some steps");
}

void TinselGuide::stop() {
	_remaining = 0;
}

// ------------------------------------------------------------ state

bool TinselGuide::isIdle() const {
	if (!ControlIsOn() || _pendingInvNo)
		return false;
	MOVER *lead = GetMover(_vm->_actor->GetLeadId());
	if (lead && MoverMoving(lead))
		return false;
	// Scenes keep some Tinsel code processes running for good (background
	// animations), so wait only for the ones the last step started.
	// A new scene ended them all (and their slots may hold the new scene's processes).
	if (sceneNumber() != _stepScene)
		_stepProcs.clear();
	if (_stepProcs.empty())
		return true;
	Common::List<Common::PROCESS *> now;
	CoroScheduler.listProcesses(PID_TCODE, now);
	for (auto it = _stepProcs.begin(); it != _stepProcs.end();) {
		if (Common::find(now.begin(), now.end(), *it) == now.end())
			it = _stepProcs.erase(it);
		else
			++it;
	}
	if (_stepProcs.empty())
		return true;
	// Some code keeps running after it has given the player control back (a worm wriggling in a tomato, an imp
	// on the loose): once nobody has said anything for a while with control on, the step is done. The sample
	// channel also carries looping sound effects, so it only stretches that to the longest line.
	uint32 now32 = g_system->getMillis();
	if (_vm->_scroll->IsScrolling())
		_quietSince = now32;
	uint32 since = MIN(now32 - _quietSince, now32 - _lastTalk);
	return since >= (_vm->_sound->sampleIsPlaying() ? kQuietLong : kQuietDone);
}

bool TinselGuide::available(const Step &st) const {
	switch (st.kind) {
	case Step::kEvent:
		if (st.actor)
			return _vm->_actor->actorAlive(st.target);
		// A live tag or exit of this scene (a step queued for another scene must not pick up whatever has that number here).
		return st.target >= 0 && st.target < PolyCount() && GetPolyHandle(st.target) != NOPOLY &&
			(PolyType(st.target) == TAG || PolyType(st.target) == EXIT);
	case Step::kInventory:
		return _vm->_dialogs->isInInventory(st.target, INV_1) || _vm->_dialogs->isInInventory(st.target, INV_2);
	case Step::kSceneWait:
		return sceneNumber() == st.target;
	case Step::kVarWait:
		return st.target >= 0 && st.target < GetGlobalCount() && GetGlobal(st.target) == st.x;
	case Step::kConverse:
		// As a click would: only while the topics are on screen (the window hides while people talk).
		return _vm->_dialogs->whichInventoryOpen() == INV_CONV && _vm->_dialogs->inventoryActive() &&
			!_vm->_dialogs->convIsHidden();
	default:
		return true;
	}
}

Common::String TinselGuide::polyName(int hPoly) const {
	if (hPoly < 0 || hPoly >= PolyCount() || GetPolyHandle(hPoly) == NOPOLY)
		return "";
	PTYPE t = PolyType(hPoly);
	if (t != TAG && t != EX_TAG)
		return t == EXIT || t == EX_EXIT ? "exit" : "";
	SCNHANDLE hText;
	int x, y;
	GetTagTag(hPoly, &hText, &x, &y);
	if (!hText)
		return "";
	char buf[100];
	LoadStringRes(hText, buf, sizeof(buf));
	return buf;
}

Common::String TinselGuide::actorName(int actor) const {
	SCNHANDLE h = _vm->_actor->GetActorTag(actor);
	if (!h)
		return "";
	char buf[100];
	LoadStringRes(h, buf, sizeof(buf));
	return buf;
}

Common::String TinselGuide::describe(const Step &st) const {
	switch (st.kind) {
	case Step::kEvent:
		return Common::String::format("%s %s%d%s %s", eventName(st.event), st.actor ? "A" : "P", st.target,
			st.item ? Common::String::format(" with %d", st.item).c_str() : "", st.comment.c_str());
	case Step::kInventory:
		return Common::String::format("%s item %d%s %s", eventName(st.event), st.target,
			st.item ? Common::String::format(" with %d", st.item).c_str() : "", st.comment.c_str());
	case Step::kConverse:
		return Common::String::format("topic %d %s", st.target, st.comment.c_str());
	case Step::kHold:
		return Common::String::format("hold %d %s", st.item, st.comment.c_str());
	case Step::kPutDown:
		return Common::String::format("put it into inventory %d %s", st.target, st.comment.c_str());
	case Step::kSceneWait:
		return Common::String::format("wait for scene %d %s", st.target, st.comment.c_str());
	case Step::kVarWait:
		return Common::String::format("wait for global %d = %d %s", st.target, st.x, st.comment.c_str());
	case Step::kClick:
	case Step::kRightClick:
		return Common::String::format("%s %d,%d %s", st.kind == Step::kClick ? "click" : "right click", st.x, st.y, st.comment.c_str());
	case Step::kWait:
		return Common::String::format("wait %d s %s", st.target, st.comment.c_str());
	case Step::kWalk:
		return Common::String::format("walk to %d,%d %s", st.x, st.y, st.comment.c_str());
	}
	return "";
}

// ------------------------------------------------------------ playing

/** A point inside the polygon, near its middle. */
bool TinselGuide::pointIn(int hPoly, int &x, int &y) const {
	int minX = 10000, maxX = -10000, minY = 10000, maxY = -10000;
	for (int i = 0; i < 4; i++) {
		minX = MIN(minX, PolyCornerX(hPoly, i));
		maxX = MAX(maxX, PolyCornerX(hPoly, i));
		minY = MIN(minY, PolyCornerY(hPoly, i));
		maxY = MAX(maxY, PolyCornerY(hPoly, i));
	}
	x = (minX + maxX) / 2;
	y = (minY + maxY) / 2;
	if (IsInPolygon(x, y, hPoly))
		return true;
	for (int r = 1; r < 200; r++)
		for (int dy = -r; dy <= r; dy += r)
			for (int dx = -r; dx <= r; dx += r)
				if (IsInPolygon(x + dx, y + dy, hPoly)) {
					x += dx;
					y += dy;
					return true;
				}
	return false;
}

void TinselGuide::click(int x, int y, bool right) {
	Common::EventManager *em = g_system->getEventManager();
	Common::Event ev;
	ev.mouse = Common::Point(x, y);
	ev.type = Common::EVENT_MOUSEMOVE;
	em->pushEvent(ev);
	g_system->warpMouse(x, y);
	ev.type = right ? Common::EVENT_RBUTTONDOWN : Common::EVENT_LBUTTONDOWN;
	em->pushEvent(ev);
	ev.type = right ? Common::EVENT_RBUTTONUP : Common::EVENT_LBUTTONUP;
	em->pushEvent(ev);
}

void TinselGuide::execute(const Step &st) {
	_executing = true;
	Common::List<Common::PROCESS *> before;
	CoroScheduler.listProcesses(PID_TCODE, before);
	switch (st.kind) {
	case Step::kEvent:
		// Close the inventory first, as a player clicking the scene would.
		if (_vm->_dialogs->inventoryActive() && (_vm->_dialogs->whichInventoryOpen() == INV_1 ||
				_vm->_dialogs->whichInventoryOpen() == INV_2))
			_vm->_dialogs->killInventory();
		if (st.item)
			_vm->_dialogs->holdItem(st.item);
		// Code often walks to where the click was (exits always do): the
		// cursor goes on the target first.
		if (st.actor) {
			int ax, ay;
			_vm->_actor->GetActorPos(st.target, &ax, &ay);
			_vm->_cursor->SetCursorXY(ax, ay - 10);
		} else {
			int px, py;
			if (pointIn(st.target, px, py))
				_vm->_cursor->SetCursorXY(px, py);
		}
		if (st.actor)
			ActorEvent(st.target, (TINSEL_EVENT)st.event, buttonFor(st.event));
		else
			RunPolyTinselCode(st.target, (TINSEL_EVENT)st.event, buttonFor(st.event), false);
		break;
	case Step::kInventory: {
		// Items act on each other only in an open inventory window, as for a player: open it first and
		// send the event once it is up (tick()).
		int invno = _vm->_dialogs->isInInventory(st.target, INV_1) ? INV_1 : INV_2;
		_pendingInv = st;
		_pendingInvNo = invno;
		break;
	}
	case Step::kConverse:
		_vm->_dialogs->guideConverse(st.target);
		break;
	case Step::kHold:
		_vm->_dialogs->holdItem(st.item ? st.item : INV_NOICON);
		break;
	case Step::kPutDown:
		_vm->_dialogs->guidePutDown(st.target);
		break;
	case Step::kVarWait:
	case Step::kSceneWait:
		break;
	case Step::kClick:
	case Step::kRightClick:
		click(st.x, st.y, st.kind == Step::kRightClick);
		break;
	case Step::kWalk:
		// As a click on the floor there (scene coordinates): exits are crossed by walking.
		WalkTo(st.x, st.y);
		break;
	case Step::kWait:
		break;
	}
	_executing = false;
	// The code processes this step started: it is done when they are.
	Common::List<Common::PROCESS *> after;
	CoroScheduler.listProcesses(PID_TCODE, after);
	_stepProcs.clear();
	_stepScene = sceneNumber();
	_quietSince = g_system->getMillis();
	for (auto *p : after)
		if (Common::find(before.begin(), before.end(), p) == before.end())
			_stepProcs.push_back(p);
}

void TinselGuide::tick() {
	pollControl();
	uint32 now = g_system->getMillis();
	if (_pendingInvNo && !(_vm->_dialogs->inventoryActive() && _vm->_dialogs->whichInventoryOpen() == _pendingInvNo)) {
		// Close whatever window is up (a conversation left open), then open this one.
		if (_vm->_dialogs->inventoryActive())
			_vm->_dialogs->killInventory();
		else
			_vm->_dialogs->popUpInventory(_pendingInvNo);
	} else if (_pendingInvNo) {
		_pendingInvNo = 0;
		_executing = true;
		Common::List<Common::PROCESS *> before;
		CoroScheduler.listProcesses(PID_TCODE, before);
		if (_pendingInv.item)
			_vm->_dialogs->holdItem(_pendingInv.item);
		_vm->_dialogs->guideInvEvent(_pendingInv.target, _pendingInv.event);
		_executing = false;
		Common::List<Common::PROCESS *> after;
		CoroScheduler.listProcesses(PID_TCODE, after);
		for (auto *p : after)
			if (Common::find(before.begin(), before.end(), p) == before.end())
				_stepProcs.push_back(p);
	}
	bool idle = isIdle();

	// Live steps from the control file: one at a time, when idle.
	if (!_live.empty()) {
		if (!idle) {
			_liveSince = 0;
		} else if (!_liveSince) {
			_liveSince = now;
		} else if (now - _liveSince >= kIdleSettle) {
			Step st = _live.front();
			if ((st.kind == Step::kVarWait || st.kind == Step::kSceneWait) && !available(st) && now - _liveSince < kStepTimeout)
				return;	// keep waiting for it
			_live.remove_at(0);
			if (!available(st)) {
				logf("live: %s: not here (scene %d), skipped", describe(st).c_str(), sceneNumber());
			} else {
				logf("live: %s (scene %d)", describe(st).c_str(), sceneNumber());
				execute(st);
			}
			_liveSince = 0;
		}
	}

	if (_remaining == 0 || _current >= (int)_steps.size()) {
		if (_remaining != 0) {
			logf("walkthrough finished");
			_remaining = 0;
		}
		return;
	}
	if (!idle)
		_idleSince = 0;
	else if (!_idleSince)
		_idleSince = now;

	const Step &st = _steps[_current];
	if (_state == kRunning) {
		bool done;
		if (st.kind == Step::kWait)
			done = now - _stateSince >= (uint32)st.target * 1000;
		else if (st.quick && !_pendingInvNo && ControlIsOn() && now - _stateSince >= kIdleSettle &&
				!(GetMover(_vm->_actor->GetLeadId()) && MoverMoving(GetMover(_vm->_actor->GetLeadId())))) {
			// "!": done as soon as the player has control again, while its code still runs
			done = true;
			_stepProcs.clear();
		} else
			done = idle && now - _idleSince >= kIdleSettle && now - _stateSince >= kIdleSettle;
		if (done) {
			logf("step %d done (scene %d)", _current + 1, sceneNumber());
			_current++;
			ConfMan.setInt("guide_step", _current);
			if (_remaining > 0)
				_remaining--;
			_state = kWaitIdle;
		} else if (now - _stateSince > kStepTimeout * 20) {
			logf("step %d never finished; stopping", _current + 1);
			_remaining = 0;
		}
		return;
	}

	if (!idle || now - _idleSince < kIdleSettle)
		return;
	if (!available(st) && st.optional) {
		logf("step %d: %s: not needed (scene %d)", _current + 1, describe(st).c_str(), sceneNumber());
		_current++;
		ConfMan.setInt("guide_step", _current);
		if (_remaining > 0)
			_remaining--;
		return;
	}
	if (!available(st)) {
		if (!_stateSince)
			_stateSince = now;
		if (now - _stateSince > (st.kind == Step::kSceneWait ? kStepTimeout * 20 : kStepTimeout)) {
			logf("step %d: its target is not here (scene %d); stopping", _current + 1, sceneNumber());
			_remaining = 0;
			_stateSince = 0;
		}
		return;
	}
	if (_saveSections && _sectionSaved != _current &&
			(_current == 0 || st.section != _steps[_current - 1].section)) {
		int n = 0;
		for (int i = 1; i <= _current; i++)
			if (_steps[i].section != _steps[i - 1].section)
				n++;
		_sectionSaved = _current;
		_vm->saveGameFromGuide(_saveSections + n, st.section);
		logf("section %d at %u ms, step %d, slot %d: %s", n + 1, now, _current + 1, _saveSections + n,
			st.section.c_str());
	}
	logf("step %d: %s", _current + 1, describe(st).c_str());
	execute(st);
	_state = kRunning;
	_stateSince = now;
	_idleSince = 0;
}

// ------------------------------------------------------------ recording

void TinselGuide::record(const Common::String &line) {
	logf("%s", line.c_str());
	if (_recordPath.empty() || _executing)
		return;
	Common::String all;
	Common::FSNode node(Common::Path(_recordPath, '/'));
	if (Common::SeekableReadStream *in = node.createReadStream()) {
		all = in->readString(0, in->size());
		delete in;
	}
	Common::WriteStream *out = node.createWriteStream();
	if (!out)
		return;
	all += line + "\n";
	out->writeString(all);
	out->finalize();
	delete out;
}

void TinselGuide::onUserEvent(int actor, int hPoly, int event) {
	if (event != WALKTO && event != ACTION && event != LOOK)
		return;
	if (!actor && event == WALKTO && (hPoly < 0 || PolyType(hPoly) == EXIT || PolyType(hPoly) == EX_EXIT)) {
		int x, y;
		_vm->_cursor->GetCursorXY(&x, &y, true);
		record(Common::String::format("g %d %d ; scene %d%s", x, y, sceneNumber(), hPoly >= 0 ? ": exit" : ""));
		return;
	}
	int held = _vm->_dialogs->whichItemHeld();
	Common::String withItem = held != INV_NOICON ? Common::String::format(" %d", held) : "";
	if (actor)
		record(Common::String::format("e %s A%d%s ; scene %d: %s", eventName(event), actor, withItem.c_str(),
			sceneNumber(), actorName(actor).c_str()));
	else if (hPoly >= 0)
		record(Common::String::format("e %s P%d%s ; scene %d: %s", eventName(event), hPoly, withItem.c_str(),
			sceneNumber(), polyName(hPoly).c_str()));
}

void TinselGuide::onInventoryEvent(int item, int event) {
	int held = _vm->_dialogs->whichItemHeld();
	Common::String withItem = held != INV_NOICON && held != item ? Common::String::format(" %d", held) : "";
	record(Common::String::format("i %s %d%s ; scene %d", eventName(event), item, withItem.c_str(), sceneNumber()));
}

void TinselGuide::onConverse(int icon) {
	record(Common::String::format("k %d ; scene %d", icon, sceneNumber()));
}

void TinselGuide::onTalk(int actor, const char *text) {
	_lastTalk = g_system->getMillis();
	if (_logTalk)
		logf("talk %d: %s", actor, text);
}

// ------------------------------------------------------------ live control

void TinselGuide::listObjects() {
	int x = 0, y = 0;
	int lead = _vm->_actor->GetLeadId();
	_vm->_actor->GetActorPos(lead, &x, &y);
	logf("scene %d, lead %d at %d,%d, control %d, step processes %d (scrolling %d, sample %d), idle %d, held %d, inventory open %d",
		sceneNumber(), lead, x, y, ControlIsOn(), (int)_stepProcs.size(), _vm->_scroll->IsScrolling(),
		_vm->_sound->sampleIsPlaying(), isIdle(),
		_vm->_dialogs->whichItemHeld(), _vm->_dialogs->whichInventoryOpen());
	{
		Common::List<Common::PROCESS *> movers, tcode;
		CoroScheduler.listProcesses(PID_MOVER, movers);
		CoroScheduler.listProcesses(PID_TCODE, tcode);
		logf("  lead in path P%d, mover processes %d, code processes %d", InPolygon(x, y, PATH), (int)movers.size(),
			(int)tcode.size());
	}
	for (int i = 0; i < PolyCount(); i++) {
		if (GetPolyHandle(i) == NOPOLY)
			continue;
		PTYPE t = PolyType(i);
		if (t == PATH || t == BLOCK) {
			logf("  P%d %s at %d,%d %d,%d %d,%d %d,%d", i, t == PATH ? "path" : "block", PolyCornerX(i, 0), PolyCornerY(i, 0),
				PolyCornerX(i, 1), PolyCornerY(i, 1), PolyCornerX(i, 2), PolyCornerY(i, 2), PolyCornerX(i, 3), PolyCornerY(i, 3));
			continue;
		}
		if (t == EFFECT) {
			logf("  P%d effect at %d,%d %d,%d %d,%d %d,%d", i, PolyCornerX(i, 0), PolyCornerY(i, 0), PolyCornerX(i, 1),
				PolyCornerY(i, 1), PolyCornerX(i, 2), PolyCornerY(i, 2), PolyCornerX(i, 3), PolyCornerY(i, 3));
			continue;
		}
		if (t != TAG && t != EX_TAG && t != EXIT && t != EX_EXIT)
			continue;
		logf("  P%d %s%s '%s' at %d,%d (code %x)", i, t == TAG ? "tag" : t == EXIT ? "exit" : t == EX_TAG ? "dead tag" : "dead exit",
			GetPolyScript(i) ? "" : " (no code)", polyName(i).c_str(), PolyCornerX(i, 0), PolyCornerY(i, 0), GetPolyScript(i));
	}
	for (int a = 1; a <= _vm->_actor->GetCount(); a++) {
		if (!_vm->_actor->actorAlive(a) || !_vm->_actor->GetActorCode(a))
			continue;
		int ax = 0, ay = 0;
		_vm->_actor->GetActorPos(a, &ax, &ay);
		logf("  A%d '%s' at %d,%d%s (code %x)", a, actorName(a).c_str(), ax, ay, _vm->_actor->ActorHidden(a) ? " hidden" : "",
			_vm->_actor->GetActorCode(a));
		if (MOVER *m = GetMover(a)) {
			if (m->bSpecReel)
				logf("    special reel");
			if (MoverMoving(m))
				logf("    moving to %d,%d (I %d,%d U %d,%d) path P%d diff %d obj %d,%d scale %d active %d walkreel %d",
					m->targetX, m->targetY, m->ItargetX, m->ItargetY, m->UtargetX, m->UtargetY, m->hCpath,
					(int)m->InDifficulty, m->objX, m->objY, m->scale, m->bActive, m->bWalkReel * 10 + m->bSpecReel);
				int dx = m->targetX > m->objX ? 1 : m->targetX < m->objX ? -1 : 0;
				int dy = m->targetY > m->objY ? 1 : m->targetY < m->objY ? -1 : 0;
				MOVER *mb = m->actorObj ? InMoverBlock(m, m->objX + dx, m->objY + dy) : nullptr;
				logf("    next step %d,%d: path P%d block P%d mover block %d", m->objX + dx, m->objY + dy,
					InPolygon(m->objX + dx, m->objY + dy, PATH), InPolygon(m->objX + dx, m->objY + dy, BLOCK),
					mb ? mb->actorID : 0);
		}
	}
	Common::String inv;
	for (int n = 0; n < _vm->_dialogs->guideInvCount(INV_1); n++)
		inv += Common::String::format(" %d", _vm->_dialogs->guideInvItem(INV_1, n));
	logf("  inventory:%s", inv.c_str());
	if (_vm->_dialogs->guideInvCount(INV_2)) {
		Common::String inv2;
		for (int n = 0; n < _vm->_dialogs->guideInvCount(INV_2); n++)
			inv2 += Common::String::format(" %d", _vm->_dialogs->guideInvItem(INV_2, n));
		logf("  inventory 2:%s", inv2.c_str());
	}
	if (_vm->_dialogs->whichInventoryOpen() == INV_CONV) {
		Common::String topics;
		for (int n = 0; n < _vm->_dialogs->guideInvCount(INV_CONV); n++)
			topics += Common::String::format(" %d", _vm->_dialogs->guideInvItem(INV_CONV, n));
		logf("  topics:%s", topics.c_str());
	}
}

void TinselGuide::saveShot(const Common::String &name) {
	Graphics::Surface *s = g_system->lockScreen();
	if (!s)
		return;
	Common::FSNode dir(Common::Path(_shotDir, '/'));
	Common::FSNode file = dir.getChild(name + ".png");
	Common::WriteStream *out = file.createWriteStream();
	if (out) {
		byte pal[768];
		g_system->getPaletteManager()->grabPalette(pal, 0, 256);
		Image::writePNG(*out, *s, pal);
		out->finalize();
		delete out;
		logf("shot %s", name.c_str());
	}
	g_system->unlockScreen();
}

void TinselGuide::runCommand(const Common::String &text) {
	Common::String cmd = text, arg;
	int sp = text.findFirstOf(' ');
	if (sp >= 0) {
		cmd = text.substr(0, sp);
		arg = text.substr(sp + 1);
		arg.trim();
	}
	logf("control: %s", text.c_str());
	int x = 0, y = 0;
	if (cmd == "objects") {
		listObjects();
	} else if (cmd == "click" || cmd == "rclick") {
		if (sscanf(arg.c_str(), "%d %d", &x, &y) == 2)
			click(x, y, cmd == "rclick");
	} else if (cmd == "move") {
		if (sscanf(arg.c_str(), "%d %d", &x, &y) == 2) {
			Common::Event ev;
			ev.type = Common::EVENT_MOUSEMOVE;
			ev.mouse = Common::Point(x, y);
			g_system->getEventManager()->pushEvent(ev);
			g_system->warpMouse(x, y);
		}
	} else if (cmd == "key") {
		Common::Event ev;
		ev.type = Common::EVENT_KEYDOWN;
		if (arg == "escape") {
			ev.kbd = Common::KeyState(Common::KEYCODE_ESCAPE, Common::ASCII_ESCAPE);
		} else if (arg == "space") {
			ev.kbd = Common::KeyState(Common::KEYCODE_SPACE, ' ');
		} else if (arg.size() == 1) {
			ev.kbd = Common::KeyState((Common::KeyCode)arg[0], arg[0]);
		} else if (arg.hasPrefix("f")) {
			int n = atoi(arg.c_str() + 1);
			ev.kbd = Common::KeyState((Common::KeyCode)(Common::KEYCODE_F1 + n - 1), Common::ASCII_F1 + n - 1);
		}
		g_system->getEventManager()->pushEvent(ev);
		ev.type = Common::EVENT_KEYUP;
		g_system->getEventManager()->pushEvent(ev);
	} else if (cmd == "shot") {
		saveShot(arg);
	} else if (cmd == "save") {
		int slot = atoi(arg.c_str());
		int sp2 = arg.findFirstOf(' ');
		_vm->saveGameFromGuide(slot, sp2 >= 0 ? arg.substr(sp2 + 1) : Common::String("guide"));
		logf("save slot %d", slot);
	} else if (cmd == "load") {
		Common::Error e = _vm->loadGameStateFromGuide(atoi(arg.c_str()));
		logf("load slot %d: %s", atoi(arg.c_str()), e.getDesc().c_str());
	} else if (cmd == "global") {
		int g = atoi(arg.c_str());
		if (g >= 0 && g < GetGlobalCount())
			logf("global %d = %d", g, GetGlobal(g));
	} else if (cmd == "name") {
		// The item's label, as when the pointer is over it in the inventory (logged as talk 0)
		_vm->_dialogs->guideInvEvent(atoi(arg.c_str()), POINTED);
		logf("item %d code %x", atoi(arg.c_str()), _vm->_dialogs->guideInvScript(atoi(arg.c_str())));
	} else if (cmd == "timer") {
		logf("timer %d = %d", atoi(arg.c_str()), Timer(atoi(arg.c_str())));
	} else if (cmd == "play") {
		play(arg.empty() ? -1 : atoi(arg.c_str()));
	} else if (cmd == "stop") {
		stop();
	} else if (cmd == "goto") {
		_current = CLIP<int>(atoi(arg.c_str()) - 1, 0, _steps.size());
		ConfMan.setInt("guide_step", _current);
		logf("current step %d", _current + 1);
	} else if (cmd == "reload") {
		if (ConfMan.hasKey("guide_file"))
			load(ConfMan.get("guide_file"));
	} else if (cmd == "quit") {
		_vm->quitGame();
	} else {
		Step st;
		if (parseLine(text, st))
			_live.push_back(st);
		else
			logf("unknown command: %s", text.c_str());
	}
}

void TinselGuide::pollControl() {
	if (_controlPath.empty())
		return;
	Common::FSNode node(Common::Path(_controlPath, '/'));
	Common::SeekableReadStream *in = node.createReadStream();
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
	delete in;
	// Only whole lines; a half-written one waits for the next poll.
	size_t lastNl = text.findLastOf('\n');
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
		if (!line.empty() && !line.hasPrefix("#"))
			runCommand(line);
	}
}

} // End of namespace Tinsel
