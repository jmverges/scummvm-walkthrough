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
#include "common/debug.h"
#include "common/file.h"
#include "common/fs.h"
#include "common/stream.h"
#include "common/system.h"
#include "common/textconsole.h"

#include "sword1/guide.h"
#include "sword1/logic.h"
#include "sword1/menu.h"
#include "sword1/mouse.h"
#include "sword1/object.h"
#include "sword1/objectman.h"
#include "sword1/sword1.h"
#include "sword1/sworddefs.h"

namespace Sword1 {

static const uint32 kSettle = 1200;      // ms a step waits before it can be done (George starts walking or talking)
static const uint32 kIdleSettle = 500;   // ms the game must stay idle
static const uint32 kChoiceSettle = 700;    // ms a conversation must wait for a subject
static const uint32 kMissing = 30000;    // ms to wait for a step's object to show up

SwordGuide::SwordGuide(SwordEngine *vm) : _vm(vm), _current(0), _remaining(0), _state(kStopped), _stateSince(0),
	  _idleSince(0), _missingSince(0), _hideCursor(false), _logTalk(true), _saveSections(0), _sectionSaved(-1),
	  _controlOffset(0) {
	if (ConfMan.hasKey("guide_file"))
		load(ConfMan.get("guide_file"));
	if (ConfMan.hasKey("guide_record"))
		_recordPath = ConfMan.get("guide_record");
	if (ConfMan.hasKey("guide_control"))
		_controlPath = ConfMan.get("guide_control");
	if (ConfMan.hasKey("guide_hide_cursor"))
		_hideCursor = ConfMan.getBool("guide_hide_cursor");
	if (ConfMan.hasKey("guide_save_sections"))
		_saveSections = ConfMan.getInt("guide_save_sections");
}

void SwordGuide::logf(const char *fmt, ...) {
	va_list va;
	va_start(va, fmt);
	Common::String s = Common::String::vformat(fmt, va);
	va_end(va);
	debug("guide: %s", s.c_str());
}

bool SwordGuide::parseLine(const Common::String &text, Step &st) const {
	Common::String line = text, comment;
	size_t semi = line.findFirstOf(';');
	if (semi != Common::String::npos) {
		comment = line.substr(semi + 1);
		comment.trim();
		line = line.substr(0, semi);
	}
	line.trim();
	if (line.size() < 2 || line[1] != ' ')
		return false;
	st.a = st.b = 0;
	st.comment = comment;
	int n = sscanf(line.c_str() + 2, "%d %d", &st.a, &st.b);
	switch (line[0]) {
	case 'e': st.kind = Step::kClick; return n >= 1;
	case 'E': st.kind = Step::kClick; st.now = true; return n >= 1;
	case 'l': st.kind = Step::kLook; return n >= 1;
	case 'i': st.kind = Step::kItem; return n >= 1;
	case 'I': st.kind = Step::kItem; st.now = true; return n >= 1;
	case 'u': st.kind = Step::kUse; return n == 2;
	case 'U': st.kind = Step::kUse; st.now = true; return n == 2;
	case 't': st.kind = Step::kCombine; return n == 2;
	case 'k': st.kind = Step::kSubject; return n >= 1;
	case 's': st.kind = Step::kShow; return n >= 1;
	case 'S': st.kind = Step::kShow; st.now = true; return n >= 1;
	case 'g': st.kind = Step::kPoint; return n == 2;
	case 'G': st.kind = Step::kPoint; st.now = true; return n == 2;
	case 'a': st.kind = Step::kScreenWait; return n >= 1;
	case 'n': st.kind = Step::kVarWait; return n == 2;
	case 'y': st.kind = Step::kAnimWait; return n == 2;
	case 'z': st.kind = Step::kFaceWait; return n == 2;
	case 'w': st.kind = Step::kWait; return n >= 1;
	default: return false;
	}
}

bool SwordGuide::load(const Common::String &path) {
	Common::File f;
	if (!f.open(Common::FSNode(Common::Path(path, '/')))) {
		logf("cannot open %s", path.c_str());
		return false;
	}
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
		if (!parseLine(line, st)) {
			logf("bad line: %s", line.c_str());
			continue;
		}
		st.section = section;
		section.clear();
		_steps.push_back(st);
	}
	logf("%d steps in %s", _steps.size(), path.c_str());
	return true;
}

void SwordGuide::setCurrent(int step) {
	_current = CLIP<int>(step, 0, _steps.size());
	_retried = false;
	_approaches = 0;
	_approaching = false;
}

// A conversation waits for a subject: the bottom bar is open (the player has no other control then).
bool SwordGuide::choosing() const {
	// fnChooser puts George's script on hold (LOGIC_choose) until a subject is picked: a pause between two lines
	// of the conversation leaves the bar open but not this
	return _vm->_menu->_subjectBarStatus == 3 && Logic::_scriptVars[IN_SUBJECT] > 0 &&   // MENU_OPEN
		!SwordEngine::_systemVars.speechRunning && !SwordEngine::_systemVars.textRunning &&
		(_vm->_objectMan->fetchObject(GEORGE)->o_logic == LOGIC_choose || g_system->getMillis() - _barSince > 5000);
}

bool SwordGuide::isIdle() const {
	if (!(Logic::_scriptVars[MOUSE_STATUS] & 1))
		return false;   // no human control
	if (Logic::_scriptVars[GEORGE_WALKING])
		return false;
	if (SwordEngine::_systemVars.speechRunning || SwordEngine::_systemVars.textRunning)
		return false;
	if (Logic::_scriptVars[SCREEN] != Logic::_scriptVars[NEW_SCREEN])
		return false;
	return true;
}

// The inventory bar, in its order: Menu::buildMenu lists the pockets (script variables) that are set.
static Common::Array<int> pockets() {
	Common::Array<int> items;
	for (int i = 0; i < TOTAL_pockets; i++)
		if (Logic::_scriptVars[POCKET_1 + i])
			items.push_back(i + 1);
	return items;
}

bool SwordGuide::available(const Step &st) const {
	Mouse *mouse = _vm->_mouse;
	Menu *menu = _vm->_menu;
	switch (st.kind) {
	case Step::kClick:
	case Step::kLook:
		for (int i = 0; i < mouse->_numObjs; i++)
			if (mouse->_objList[i].id == st.a)
				return true;
		return false;
	case Step::kUse:
	case Step::kItem:
	case Step::kCombine:
	case Step::kShow: {
		const Common::Array<int> items = pockets();
		bool has = false;
		for (uint i = 0; i < items.size(); i++)
			has = has || items[i] == st.a;
		if (!has)
			return false;
		if (st.kind == Step::kUse) {
			for (int i = 0; i < mouse->_numObjs; i++)
				if (mouse->_objList[i].id == st.b)
					return true;
			return false;
		}
		if (st.kind == Step::kCombine) {
			for (uint i = 0; i < items.size(); i++)
				if (items[i] == st.b)
					return true;
			return false;
		}
		return true;
	}
	case Step::kSubject:
		for (uint i = 0; i < Logic::_scriptVars[IN_SUBJECT]; i++)
			if ((int)menu->_subjectBar[i] == st.a)
				return true;
		return false;
	default:
		return true;
	}
}

// Where a move points, in screen coordinates (640x480, the top bar is y 0-39, the bottom bar 440-479).
bool SwordGuide::position(const Move &m, int &x, int &y) const {
	Mouse *mouse = _vm->_mouse;
	Menu *menu = _vm->_menu;
	switch (m.target) {
	case Move::kObject:
		for (int i = 0; i < mouse->_numObjs; i++) {
			Object *c = mouse->_objList[i].compact;
			if (mouse->_objList[i].id != m.id)
				continue;
			// the middle of the part of its mouse box that is on the screen
			const int sx = (int)Logic::_scriptVars[SCROLL_OFFSET_X] + 128, sy = (int)Logic::_scriptVars[SCROLL_OFFSET_Y] + 128 - 40;
			const int x1 = MAX<int>(c->o_mouse_x1 - sx, 0), x2 = MIN<int>(c->o_mouse_x2 - sx, 639);
			const int y1 = MAX<int>(c->o_mouse_y1 - sy, 41), y2 = MIN<int>(c->o_mouse_y2 - sy, 439);
			x = x1 <= x2 ? (x1 + x2) / 2 : CLIP((c->o_mouse_x1 + c->o_mouse_x2) / 2 - sx, 0, 639);
			y = y1 <= y2 ? (y1 + y2) / 2 : CLIP((c->o_mouse_y1 + c->o_mouse_y2) / 2 - sy, 41, 439);
			if (x1 <= x2 && y1 <= y2 && pickAt(x, y) != m.id) {
				// someone stands in front of it there: the point nearest the middle that picks this one
				int best = -1;
				for (int py = y1; py <= y2; py += 4)
					for (int px = x1; px <= x2; px += 4)
						if (pickAt(px, py) == m.id) {
							const int d = ABS(px - x) + ABS(py - y);
							if (best < 0 || d < best) {
								best = d;
								x = px;
								y = py;
							}
						}
			}
			return true;
		}
		return false;
	case Move::kTopItem: {
		const Common::Array<int> items = pockets();
		for (uint i = 0; i < items.size(); i++)
			if (items[i] == m.id) {
				x = i * 40 + 20;
				y = 20;
				return true;
			}
		return false;
	}
	case Move::kBottomSubject:
		for (uint i = 0; i < Logic::_scriptVars[IN_SUBJECT]; i++)
			if ((int)menu->_subjectBar[i] == m.id) {
				x = i * 40 + 20;
				y = 460;
				return true;
			}
		return false;
	case Move::kTopBar:
		x = 320;
		y = 20;
		return true;
	default:
		x = m.x;
		y = m.y;
		return true;
	}
}

// The object the mouse picks at a screen point, as Mouse::engine does (the lowest priority first).
int SwordGuide::pickAt(int x, int y) const {
	Mouse *mouse = _vm->_mouse;
	const int mx = (int)Logic::_scriptVars[SCROLL_OFFSET_X] + x + 128;
	const int my = (int)Logic::_scriptVars[SCROLL_OFFSET_Y] + y + 128 - 40;
	for (int priority = 0; priority < 10; priority++)
		for (int i = 0; i < mouse->_numObjs; i++) {
			Object *c = mouse->_objList[i].compact;
			if (c->o_priority == priority && mx >= c->o_mouse_x1 && mx <= c->o_mouse_x2 &&
				my >= c->o_mouse_y1 && my <= c->o_mouse_y2)
				return mouse->_objList[i].id;
		}
	return 0;
}

// Whether a screen point is on an object the mouse would pick (not the background).
bool SwordGuide::onHotspot(int x, int y) const {
	Mouse *mouse = _vm->_mouse;
	const int mx = (int)Logic::_scriptVars[SCROLL_OFFSET_X] + x + 128;
	const int my = (int)Logic::_scriptVars[SCROLL_OFFSET_Y] + y + 128 - 40;
	for (int i = 0; i < mouse->_numObjs; i++) {
		Object *c = mouse->_objList[i].compact;
		if (c->o_type == 1)
			continue;   // the screen itself
		if (mx >= c->o_mouse_x1 && mx <= c->o_mouse_x2 && my >= c->o_mouse_y1 && my <= c->o_mouse_y2)
			return true;
	}
	return false;
}

// An object of a scrolling screen that is out of view: a click there misses it (the mouse stays on the screen).
// Sets x, y to a floor point on that side, level with George's feet, to walk there first.
bool SwordGuide::offScreen(int id, int &x, int &y) const {
	Mouse *mouse = _vm->_mouse;
	for (int i = 0; i < mouse->_numObjs; i++) {
		Object *c = mouse->_objList[i].compact;
		if (mouse->_objList[i].id != id)
			continue;
		// out of view only when no part of its mouse box is on the screen
		const int sx = (int)Logic::_scriptVars[SCROLL_OFFSET_X] + 128;
		const int vis = MIN<int>(c->o_mouse_x2 - sx, 639) - MAX<int>(c->o_mouse_x1 - sx, 0);
		if (vis >= MIN<int>(40, c->o_mouse_x2 - c->o_mouse_x1))
			return false;   // enough of it is on the screen to click
		const int ox = (c->o_mouse_x1 + c->o_mouse_x2) / 2 - sx;
		Object *george = _vm->_objectMan->fetchObject(GEORGE);
		// a stretch of the way there from where George stands: the screen scrolls as he walks
		x = ox < 8 ? 60 : 580;
		// level with George's feet, or every other try level with the object (some screens draw George
		// elsewhere than his coordinates say)
		const int oy = (c->o_mouse_y1 + c->o_mouse_y2) / 2 - (int)Logic::_scriptVars[SCROLL_OFFSET_Y] - 128 + 40;
		const int gy = george->o_ycoord - (int)Logic::_scriptVars[SCROLL_OFFSET_Y] - 128 + 40;
		const int tries[] = {gy, gy + 40, oy, gy - 30};
		y = CLIP(tries[_approaches % 4], 41, 439);
		// floor, not a hotspot (a door there would take George through it): try nearby points
		static const int dy[] = {0, 15, -15, 30, -30, 45, -45};
		static const int dx[] = {0, -60, 60, -120, 120};
		for (int a = 0; a < ARRAYSIZE(dx); a++)
			for (int b = 0; b < ARRAYSIZE(dy); b++) {
				const int px = CLIP(x + dx[a], 40, 600), py = CLIP(y + dy[b], 41, 439);
				if (!onHotspot(px, py)) {
					x = px;
					y = py;
					return true;
				}
			}
		return true;
	}
	return false;
}

// Point at a target for a few frames, press, release: the mouse engine only acts on a click a frame after it.
void SwordGuide::clickOn(Move::Target target, int id, bool right) {
	Move m;
	m.target = target;
	m.id = id;
	m.x = m.y = 0;
	m.flags = 0;
	m.frames = 6;   // long enough for the object's mouse-on script after a scroll
	_moves.push_back(m);
	m.flags = right ? BS1R_BUTTON_DOWN : BS1L_BUTTON_DOWN;
	m.frames = 1;
	_moves.push_back(m);
	m.flags = 0;
	_moves.push_back(m);
	m.flags = right ? BS1R_BUTTON_UP : BS1L_BUTTON_UP;
	_moves.push_back(m);
	m.flags = 0;
	m.frames = 2;
	_moves.push_back(m);
}

void SwordGuide::execute(const Step &st) {
	_moves.clear();
	Move bar;
	bar.target = Move::kTopBar;
	bar.id = bar.x = bar.y = 0;
	bar.flags = 0;
	bar.frames = st.now ? 4 : 12;   // the inventory bar slides in (no time to wait for it on a quick step)
	switch (st.kind) {
	case Step::kClick:
		clickOn(Move::kObject, st.a);
		break;
	case Step::kLook:
		clickOn(Move::kObject, st.a, true);
		break;
	case Step::kItem:
		_moves.push_back(bar);
		clickOn(Move::kTopItem, st.a);
		break;
	case Step::kUse:
		_moves.push_back(bar);
		clickOn(Move::kTopItem, st.a);
		clickOn(Move::kObject, st.b);
		break;
	case Step::kCombine:
		_moves.push_back(bar);
		clickOn(Move::kTopItem, st.a);
		clickOn(Move::kTopItem, st.b);
		break;
	case Step::kSubject:
		clickOn(Move::kBottomSubject, st.a);
		break;
	case Step::kShow:
		// in a conversation the inventory bar is open: a click on the item shows it
		clickOn(Move::kTopItem, st.a);
		break;
	case Step::kPoint: {
		Move m;
		m.target = Move::kPoint;
		m.id = 0;
		m.x = st.a;
		m.y = st.b;
		m.flags = 0;
		m.frames = 3;
		_moves.push_back(m);
		m.flags = BS1L_BUTTON_DOWN;
		m.frames = 1;
		_moves.push_back(m);
		m.flags = 0;
		_moves.push_back(m);
		m.flags = BS1L_BUTTON_UP;
		_moves.push_back(m);
		break;
	}
	default:
		break;
	}
}

Common::String SwordGuide::itemName(int item) const {
	if (item <= 0 || item > TOTAL_pockets)
		return "";
	int32 desc = Menu::_objectDefs[item].textDesc;
	if (!desc)
		return "";
	char *text = _vm->_objectMan->lockText(desc);
	Common::String s = text ? text : "";
	_vm->_objectMan->unlockText(desc);
	return s;
}

Common::String SwordGuide::objectName(int id) const {
	return Common::String::format("%d", id);
}

Common::String SwordGuide::describe(const Step &st) const {
	static const char *const names[] = {"click", "look", "item", "use", "combine", "subject", "show", "point",
		"screen", "var", "wait", "anim", "face"};
	return Common::String::format("%s %d %d %s", names[st.kind], st.a, st.b, st.comment.c_str());
}

void SwordGuide::tick(Common::Point &mouse, uint16 &state) {
	const uint32 now = g_system->getMillis();
	pollControl();

	// The player's own clicks are recorded; while the guide plays they are ignored.
	if (state & (MOUSE_DOWN_MASK)) {
		if (_state == kStopped && _moves.empty())
			record(state, mouse);
	}

	if (!_moves.empty()) {
		Move &m = _moves[0];
		int x, y;
		if (position(m, x, y)) {
			_guideMouse = Common::Point(x, y);
		}
		mouse = _guideMouse;
		state = m.flags;
		g_system->warpMouse(mouse.x, mouse.y);
		if (--m.frames <= 0)
			_moves.remove_at(0);
		if (_moves.empty() && _state == kRunning) {
			_state = kSettle;
			_stateSince = now;
		}
		return;
	}

	if (_watchId && (int)Logic::_scriptVars[SCREEN] != _watchScreen) {
		logf("watch %d: off, the screen changed", _watchId);
		_watchId = 0;   // its section may be gone
	}
	if (_watchId) {
		Object *w = _vm->_objectMan->fetchObject(_watchId);
		const int key = w->o_anim_resource * 16 + w->o_dir * 2 + (w->o_logic == LOGIC_AR_animate);
		if (key != _watchAnim) {
			_watchAnim = key;
			logf("watch %d: anim %d dir %d logic %d at %d,%d at %u ms", _watchId, w->o_anim_resource, w->o_dir,
				w->o_logic, w->o_xcoord, w->o_ycoord, now);
		}
		const int ctl = (Logic::_scriptVars[MOUSE_STATUS] & 1) * 100 + _vm->_menu->_subjectBarStatus * 10 +
			(Logic::_scriptVars[IN_SUBJECT] ? 1 : 0);
		if (ctl != _watchCtl) {
			_watchCtl = ctl;
			logf("watch: control %d bars %d subjects %d at %u ms", ctl / 100, ctl / 10 % 10, ctl % 10, now);
		}
		if (_vm->_mouse->_numObjs != _watchObjs) {
			_watchObjs = _vm->_mouse->_numObjs;
			logf("watch: %d mouse objects at %u ms", _watchObjs, now);
		}
	}

	if (_state == kStopped)
		return;

	const bool idle = isIdle();
	if (idle) {
		if (!_idleSince)
			_idleSince = now;
	} else {
		_idleSince = 0;
	}

	// the bar open with nobody talking, for how long (in case someone else than George runs the chooser)
	if (_vm->_menu->_subjectBarStatus == 3 && Logic::_scriptVars[IN_SUBJECT] > 0 &&
		!SwordEngine::_systemVars.speechRunning && !SwordEngine::_systemVars.textRunning) {
		if (!_barOpen) {
			_barOpen = true;
			_barSince = now;
		}
	} else {
		_barOpen = false;
		_barSince = now;
	}
	const bool choice = choosing();
	if ((!idle && !choice) || SwordEngine::_systemVars.speechRunning || SwordEngine::_systemVars.textRunning ||
		_vm->_objectMan->fetchObject(GEORGE)->o_logic == LOGIC_AR_animate)
		_busy = true;
	if (choice) {
		if (!_choiceSince)
			_choiceSince = now;
	} else {
		_choiceSince = 0;
	}

	if (_state == kSettle) {
		if (now - _stateSince < kSettle)
			return;
		const Step &cur = _steps[_current];
		if ((cur.kind == Step::kSubject || cur.kind == Step::kShow) && !_busy && choice && now - _stateSince > 4000) {
			// the click on the bar came too early and the game never saw it: again
			logf("step %d: no answer, clicking again", _current + 1);
			execute(cur);
			_state = kRunning;
			_stateSince = now;
			return;
		}
		// done once the game reacted (George walked, someone talked, a conversation went on) and waits again,
		// or when nothing happened for a while
		// a wait step is done as soon as its condition is met: the next step may have to act right then
		const bool waited = cur.kind == Step::kWait || cur.kind == Step::kScreenWait || cur.kind == Step::kVarWait ||
			cur.kind == Step::kAnimWait || cur.kind == Step::kFaceWait;
		const bool reacted = waited || _busy || now - _stateSince > 3000;
		const bool ready = waited || (_idleSince && now - _idleSince >= kIdleSettle) ||
			(_busy && _choiceSince && now - _choiceSince >= kChoiceSettle);
		if (!ready || !reacted)
			return;
		if (_approaching) {
			_approaching = false;
			_state = kWaitIdle;
			return;
		}
		if (cur.kind == Step::kClick && !_spoke && !_retried && isPerson(cur.a)) {
			// a click on someone far away can end with George just walking up to them: once more
			logf("step %d: nobody talked, clicking again", _current + 1);
			_retried = true;
			_busy = false;
			execute(cur);
			_state = kRunning;
			_stateSince = now;
			return;
		}
		logf("step %d done (screen %d)", _current + 1, Logic::_scriptVars[SCREEN]);
		setCurrent(_current + 1);
		if (_remaining > 0 && --_remaining == 0) {
			_state = kStopped;
			return;
		}
		_state = kWaitIdle;
	}

	if (_state == kRunning) {
		// a wait step
		const Step &st = _steps[_current];
		bool done = false;
		if (st.kind == Step::kWait)
			done = now - _stateSince >= (uint32)st.a * 1000;
		else if (st.kind == Step::kScreenWait)
			done = (int)Logic::_scriptVars[SCREEN] == st.a && idle;
		else if (st.kind == Step::kVarWait)
			done = (int)Logic::_scriptVars[st.a] == st.b;
		else if (st.kind == Step::kAnimWait)
			done = _vm->_objectMan->fetchObject(st.a)->o_anim_resource == st.b;
		else if (st.kind == Step::kFaceWait) {
			Object *o = _vm->_objectMan->fetchObject(st.a);
			done = o->o_logic != LOGIC_AR_animate && ((st.b >> o->o_dir) & 1);   // standing, not walking or turning
		}
		if (done) {
			_state = kSettle;
			_stateSince = now - kSettle;
		}
		return;
	}

	// kWaitIdle: the next step, once the game waits for the player
	if (_current >= (int)_steps.size()) {
		logf("walkthrough finished");
		_state = kStopped;
		return;
	}
	const Step &st = _steps[_current];
	const bool waitStep = st.kind == Step::kWait || st.kind == Step::kScreenWait || st.kind == Step::kVarWait ||
		st.kind == Step::kAnimWait || st.kind == Step::kFaceWait;
	const bool choiceStep = st.kind == Step::kSubject || st.kind == Step::kShow;
	if (choiceStep && st.now) {
		if (_vm->_menu->_subjectBarStatus != 3 || !Logic::_scriptVars[IN_SUBJECT])
			return;   // the bars are open, even while someone still talks
	} else if (choiceStep) {
		if (!_choiceSince && _idleSince && now - _idleSince > 8000) {
			// the conversation ended on its own (a goodbye that was not needed): go on
			logf("step %d: no conversation, skipped", _current + 1);
			setCurrent(_current + 1);
			if (_remaining > 0 && --_remaining == 0)
				_state = kStopped;
			return;
		}
		if (!_choiceSince || now - _choiceSince < kChoiceSettle)
			return;
	} else if (st.now) {
		if (!(Logic::_scriptVars[MOUSE_STATUS] & 1))
			return;   // as soon as the player has control, even while someone talks
	} else if (!waitStep && (!_idleSince || now - _idleSince < kIdleSettle)) {
		return;
	}
	if (!st.section.empty() && _sectionSaved != _current) {
		_sectionSaved = _current;
		int num = 0;
		for (int i = 0; i <= _current; i++)
			if (!_steps[i].section.empty())
				num++;
		const int slot = _saveSections ? _saveSections + num - 1 : 0;
		if (_saveSections)
			_vm->saveGameState(slot, st.section);
		logf("section %d at %u ms, step %d, slot %d: %s", num, now, _current + 1, slot, st.section.c_str());
	}
	if (!available(st)) {
		if (!_missingSince)
			_missingSince = now;
		if (choiceStep && _choiceSince && now - _choiceSince > 5000) {
			// the conversation is waiting but has no such subject (it went another way): go on without it
			logf("step %d: no such subject, skipped", _current + 1);
			_missingSince = 0;
			setCurrent(_current + 1);
			if (_remaining > 0 && --_remaining == 0)
				_state = kStopped;
			return;
		}
		if (now - _missingSince > kMissing) {
			logf("step %d: its object is not here (screen %d); stopping", _current + 1, Logic::_scriptVars[SCREEN]);
			_state = kStopped;
			_missingSince = 0;
		}
		return;
	}
	_missingSince = 0;
	int wx, wy;
	const int target = st.kind == Step::kUse ? st.b : st.a;
	if ((st.kind == Step::kClick || st.kind == Step::kLook || st.kind == Step::kUse) && _approaches < 8 &&
		offScreen(target, wx, wy)) {
		// walk toward it first, the screen scrolls; then this step again
		_approaches++;
		logf("step %d: %d is out of view, walking to %d,%d", _current + 1, target, wx, wy);
		Step walk;
		walk.kind = Step::kPoint;
		walk.a = wx;
		walk.b = wy;
		_busy = false;
		_approaching = true;
		_state = kRunning;
		_stateSince = now;
		execute(walk);
		return;
	}
	logf("step %d: %s", _current + 1, describe(st).c_str());
	_spoke = false;
	_busy = false;
	_state = kRunning;
	_stateSince = now;
	execute(st);
}

bool SwordGuide::isPerson(int id) const {
	Mouse *mouse = _vm->_mouse;
	for (int i = 0; i < mouse->_numObjs; i++)
		if (mouse->_objList[i].id == id)
			return mouse->_objList[i].compact->o_type == 4 || mouse->_objList[i].compact->o_type == 5;
	return false;
}

void SwordGuide::onSpeak(int32 id, uint32 textNo) {
	_spoke = true;
	if (!_logTalk)
		return;
	char *text = _vm->_objectMan->lockText(textNo);
	if (text)
		logf("talk %d: %s", id, text);
	_vm->_objectMan->unlockText(textNo);
}

void SwordGuide::record(uint16 state, const Common::Point &mouse) {
	const bool right = state & BS1R_BUTTON_DOWN;
	Common::String line;
	if (mouse.y < 40) {
		const int slot = mouse.x / 40;
		if (slot < (int)_vm->_menu->_inMenu)
			line = Common::String::format("i %d ; %s%s", _vm->_menu->_menuList[slot], right ? "(look) " : "",
				itemName(_vm->_menu->_menuList[slot]).c_str());
	} else if (mouse.y >= 440 && Logic::_scriptVars[IN_SUBJECT]) {
		const int slot = mouse.x / 40;
		if (slot < (int)Logic::_scriptVars[IN_SUBJECT])
			line = Common::String::format("k %d", _vm->_menu->_subjectBar[slot]);
	} else if (Logic::_scriptVars[SPECIAL_ITEM]) {
		const int id = Logic::_scriptVars[SPECIAL_ITEM];
		if (Logic::_scriptVars[OBJECT_HELD] && !right)
			line = Common::String::format("u %d %d ; %s", Logic::_scriptVars[OBJECT_HELD], id,
				itemName(Logic::_scriptVars[OBJECT_HELD]).c_str());
		else
			line = Common::String::format("%c %d", right ? 'l' : 'e', id);
	} else {
		line = Common::String::format("g %d %d", mouse.x, mouse.y);
	}
	if (line.empty())
		return;
	line += Common::String::format(" ; screen %d", Logic::_scriptVars[SCREEN]);
	logf("record: %s", line.c_str());
	if (_recordPath.empty())
		return;
	Common::String text;
	Common::File in;
	if (in.open(Common::FSNode(Common::Path(_recordPath, '/')))) {
		text = in.readString(0, in.size());
		in.close();
	}
	Common::DumpFile out;
	if (out.open(Common::Path(_recordPath, '/'), true)) {
		out.writeString(text);
		out.writeString(line);
		out.writeString("\n");
		out.close();
	}
}

void SwordGuide::listObjects() {
	logf("screen %d, idle %d, human %d, walking %d, held %d, subjects %d, choosing %d, guide state %d step %d",
		Logic::_scriptVars[SCREEN], isIdle(), Logic::_scriptVars[MOUSE_STATUS] & 1, Logic::_scriptVars[GEORGE_WALKING],
		Logic::_scriptVars[OBJECT_HELD], Logic::_scriptVars[IN_SUBJECT], choosing(), _state, _current + 1);
	Object *george = _vm->_objectMan->fetchObject(GEORGE);
	logf("george logic %d at %d,%d", george->o_logic, george->o_xcoord, george->o_ycoord);
	Mouse *mouse = _vm->_mouse;
	for (int i = 0; i < mouse->_numObjs; i++) {
		Object *c = mouse->_objList[i].compact;
		int x = (c->o_mouse_x1 + c->o_mouse_x2) / 2 - (int)Logic::_scriptVars[SCROLL_OFFSET_X] - 128;
		int y = (c->o_mouse_y1 + c->o_mouse_y2) / 2 - (int)Logic::_scriptVars[SCROLL_OFFSET_Y] - 128 + 40;
		logf("  object %d at %d,%d type %d priority %d", mouse->_objList[i].id, x, y, c->o_type, c->o_priority);
	}
	const Common::Array<int> items = pockets();
	for (uint i = 0; i < items.size(); i++)
		logf("  item %d '%s'", items[i], itemName(items[i]).c_str());
	for (uint i = 0; i < Logic::_scriptVars[IN_SUBJECT]; i++)
		logf("  subject %d", _vm->_menu->_subjectBar[i]);
}

void SwordGuide::runCommand(const Common::String &text) {
	Common::String line = text;
	line.trim();
	if (line.empty())
		return;
	logf("control: %s", line.c_str());
	char cmd[32] = "";
	int x = 0, y = 0;
	sscanf(line.c_str(), "%31s %d %d", cmd, &x, &y);
	Common::String c(cmd);
	Step st;
	if (c == "play") {
		_remaining = 0;
		_state = kWaitIdle;
		logf("play from step %d", _current + 1);
	} else if (c == "stop") {
		_state = kStopped;
		_moves.clear();
	} else if (c == "goto") {
		setCurrent(x > 0 ? x - 1 : 0);
		_sectionSaved = _current;   // no save for the section it starts in
		logf("current step %d", _current + 1);
	} else if (c == "reload") {
		if (ConfMan.hasKey("guide_file"))
			load(ConfMan.get("guide_file"));
	} else if (c == "watch") {
		_watchId = x;   // log that object's animation changes (0: off), on this screen
		_watchScreen = Logic::_scriptVars[SCREEN];
		_watchAnim = -1;
		logf("watching %d", x);
	} else if (c == "objects") {
		listObjects();
	} else if (c == "var") {
		logf("var %d = %d", x, Logic::_scriptVars[x]);
	} else if (c == "setvar") {
		Logic::_scriptVars[x] = y;
		logf("var %d = %d", x, y);
	} else if (c == "save") {
		_vm->saveGameState(x, Common::String::format("guide %d", x));
		logf("saved slot %d", x);
	} else if (c == "load") {
		_vm->loadGameState(x);
		logf("loaded slot %d", x);
	} else if (c == "quit") {
		_vm->quitGame();
	} else if (parseLine(line, st)) {
		// one step now (after those already queued), as a walkthrough of its own
		const bool queued = _state != kStopped && _remaining > 0;
		_steps.insert_at(MIN<int>(_current + (queued ? _remaining : 0), _steps.size()), st);
		_remaining = queued ? _remaining + 1 : 1;
		if (!queued)
			_state = kWaitIdle;
	}
}

void SwordGuide::pollControl() {
	if (_controlPath.empty())
		return;
	Common::File f;
	if (!f.open(Common::FSNode(Common::Path(_controlPath, '/'))))
		return;
	if (f.size() < (int64)_controlOffset)
		_controlOffset = 0;   // the file was truncated: start again
	if (f.size() == (int64)_controlOffset)
		return;
	f.seek(_controlOffset);
	Common::String text = f.readString(0, f.size() - _controlOffset);
	// only whole lines
	size_t end = text.findLastOf('\n');
	if (end == Common::String::npos)
		return;
	_controlOffset += end + 1;
	text = text.substr(0, end);
	Common::String line;
	for (uint i = 0; i <= text.size(); i++) {
		if (i == text.size() || text[i] == '\n') {
			runCommand(line);
			line.clear();
		} else {
			line += text[i];
		}
	}
}

} // End of namespace Sword1
