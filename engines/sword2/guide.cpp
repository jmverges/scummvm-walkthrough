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
#include "common/system.h"
#include "common/textconsole.h"

#include "sword2/sword2.h"
#include "sword2/defs.h"
#include "sword2/guide.h"
#include "sword2/header.h"
#include "sword2/logic.h"
#include "sword2/mouse.h"
#include "sword2/resman.h"
#include "sword2/screen.h"

namespace Sword2 {

static const uint32 kSettle = 1200;       // ms a step waits before it can be done (George starts walking or talking)
static const uint32 kIdleSettle = 500;    // ms the game must stay idle
static const uint32 kChoiceSettle = 700;  // ms a conversation must wait for a subject
static const uint32 kStill = 400;         // ms George's feet must stay put to count as standing
static const uint32 kMissing = 30000;     // ms to wait for a step's object to show up
static const int kMenuY = 420;
static const int kTalkPointer = 787;     // the speech-bubble pointer people have            // the middle of the bottom menu, in game coordinates

Sword2Guide::Sword2Guide(Sword2Engine *vm) : _vm(vm) {
	if (ConfMan.hasKey("guide_file"))
		load(ConfMan.get("guide_file"));
	if (ConfMan.hasKey("guide_control"))
		_controlPath = ConfMan.get("guide_control");
	if (ConfMan.hasKey("guide_hide_cursor"))
		_hideCursor = ConfMan.getBool("guide_hide_cursor");
	if (ConfMan.hasKey("guide_save_sections"))
		_saveSections = ConfMan.getInt("guide_save_sections");
}

void Sword2Guide::logf(const char *fmt, ...) {
	va_list va;
	va_start(va, fmt);
	Common::String s = Common::String::vformat(fmt, va);
	va_end(va);
	debug("guide: %u %s", g_system->getMillis(), s.c_str());
}

bool Sword2Guide::parseLine(const Common::String &text, Step &st) const {
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
	st.a = st.b = st.c = 0;
	st.comment = comment;
	int n = sscanf(line.c_str() + 2, "%d %d %d", &st.a, &st.b, &st.c);
	switch (line[0]) {
	case 'e': st.kind = Step::kClick; return n >= 1;
	case 'E': st.kind = Step::kClick; st.now = true; return n >= 1;
	case 'l': st.kind = Step::kLook; return n >= 1;
	case 'i': st.kind = Step::kItem; return n >= 1;
	case 'u': st.kind = Step::kUse; return n == 2;
	case 't': st.kind = Step::kCombine; return n == 2;
	case 'k': st.kind = Step::kSubject; return n >= 1;
	case 'g': st.kind = Step::kPoint; return n == 2;
	case 'a': st.kind = Step::kScreenWait; return n >= 1;
	case 'n': st.kind = Step::kVarWait; return n == 2;
	case 'w': st.kind = Step::kWait; return n >= 1;
	case 'x': st.kind = Step::kRectWait; return n == 3;
	case 'h': st.kind = Step::kHold; return n >= 1;
	case 'v': st.kind = Step::kGoneWait; return n == 2;
	default: return false;
	}
}

bool Sword2Guide::load(const Common::String &path) {
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

void Sword2Guide::setCurrent(int step) {
	_current = CLIP<int>(step, 0, _steps.size());
	_retried = false;
	_exitTries = 0;
	_approaches = 0;
	_approaching = false;
}

void Sword2Guide::next() {
	setCurrent(_current + 1);
	if (_remaining > 0 && --_remaining == 0)
		_state = kStopped;
	else
		_state = kWaitIdle;
}

bool Sword2Guide::waitStep(const Step &st) const {
	return st.kind == Step::kWait || st.kind == Step::kScreenWait || st.kind == Step::kVarWait ||
		st.kind == Step::kRectWait || st.kind == Step::kGoneWait;
}

bool Sword2Guide::mousePos(int &x, int &y) const {
	if (!_active)
		return false;
	x = _mouse.x;
	y = _mouse.y;
	return true;
}

// A conversation waits for a subject: fnChoose has put the icons in the bottom menu.
bool Sword2Guide::choosing() const {
	return _vm->_mouse->_choosing && _vm->_logic->readVar(IN_SUBJECT) > 0 && !_vm->_logic->_speechRunning;
}

bool Sword2Guide::isIdle() const {
	Mouse *mouse = _vm->_mouse;
	Logic *logic = _vm->_logic;
	if (mouse->_mouseStatus || logic->readVar(DEAD))
		return false;   // no human control
	if (mouse->_choosing || logic->_speechRunning)
		return false;
	if (mouse->_mouseMode != MOUSE_normal && mouse->_mouseMode != MOUSE_menu)
		return false;
	for (int i = 0; i < MAX_events; i++)
		if (logic->_eventList[i].id == CUR_PLAYER_ID)
			return false;   // a click George has not taken up yet
	return g_system->getMillis() - _feetSince >= kStill;
}

bool Sword2Guide::available(const Step &st) const {
	Mouse *mouse = _vm->_mouse;
	switch (st.kind) {
	case Step::kClick:
	case Step::kLook:
		for (uint i = 0; i < mouse->_curMouse; i++)
			if (mouse->_mouseList[i].id == st.a)
				return true;
		return false;
	case Step::kItem:
	case Step::kHold:
	case Step::kUse:
	case Step::kCombine: {
		bool has = false, other = st.kind != Step::kCombine;
		for (uint i = 0; i < mouse->_totalMasters; i++) {
			has = has || mouse->_masterMenuList[i].icon_resource == st.a;
			other = other || mouse->_masterMenuList[i].icon_resource == st.b;
		}
		if (!has || !other)
			return false;
		if (st.kind == Step::kUse) {
			for (uint i = 0; i < mouse->_curMouse; i++)
				if (mouse->_mouseList[i].id == st.b)
					return true;
			return false;
		}
		return true;
	}
	case Step::kSubject:
		for (uint i = 0; i < _vm->_logic->readVar(IN_SUBJECT); i++)
			if ((int)mouse->_subjectList[i].res == st.a)
				return true;
		return false;
	default:
		return true;
	}
}

// The object the mouse picks at a game point, as Mouse::checkMouseList does (the lowest priority first).
int Sword2Guide::pickAt(int x, int y) const {
	Mouse *mouse = _vm->_mouse;
	ScreenInfo *si = _vm->_screen->getScreenInfo();
	const Common::Point p(x + si->scroll_offset_x, y + si->scroll_offset_y);
	for (int priority = 0; priority < 10; priority++)
		for (uint i = 0; i < mouse->_curMouse; i++)
			if (mouse->_mouseList[i].priority == priority && mouse->_mouseList[i].rect.contains(p))
				return mouse->_mouseList[i].id;
	return 0;
}

// Where a move points, in game coordinates (640x400 between the menus; the bottom menu is 400-439).
bool Sword2Guide::position(const Move &m, int &x, int &y) const {
	Mouse *mouse = _vm->_mouse;
	ScreenInfo *si = _vm->_screen->getScreenInfo();
	switch (m.target) {
	case Move::kObject:
		for (uint i = 0; i < mouse->_curMouse; i++) {
			const Common::Rect &r = mouse->_mouseList[i].rect;
			if (mouse->_mouseList[i].id != m.id)
				continue;
			// the middle of the part of its box that is on the screen
			const int x1 = MAX<int>(r.left - si->scroll_offset_x, 0), x2 = MIN<int>(r.right - 1 - si->scroll_offset_x, 639);
			const int y1 = MAX<int>(r.top - si->scroll_offset_y, 0), y2 = MIN<int>(r.bottom - 1 - si->scroll_offset_y, 399);
			x = x1 <= x2 ? (x1 + x2) / 2 : CLIP<int>((r.left + r.right) / 2 - si->scroll_offset_x, 0, 639);
			y = y1 <= y2 ? (y1 + y2) / 2 : CLIP<int>((r.top + r.bottom) / 2 - si->scroll_offset_y, 0, 399);
			if (x1 <= x2 && y1 <= y2 && pickAt(x, y) != m.id) {
				// something with a lower priority is in front of it there: the nearest point that picks it
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
	case Move::kMenuItem:
		for (uint i = 0; i < mouse->_totalMasters; i++)
			if (mouse->_masterMenuList[i].icon_resource == m.id) {
				x = RDMENU_ICONSTART + i * (RDMENU_ICONWIDE + RDMENU_ICONSPACING) + RDMENU_ICONWIDE / 2;
				y = kMenuY;
				return true;
			}
		return false;
	case Move::kSubject:
		for (uint i = 0; i < _vm->_logic->readVar(IN_SUBJECT); i++)
			if ((int)mouse->_subjectList[i].res == m.id) {
				x = RDMENU_ICONSTART + i * (RDMENU_ICONWIDE + RDMENU_ICONSPACING) + RDMENU_ICONWIDE / 2;
				y = kMenuY;
				return true;
			}
		return false;
	default:
		x = m.x;
		y = m.y;
		return true;
	}
}

// An object of a scrolling screen that is out of view: a click there misses it. Sets x, y to a floor point on
// that side, level with George's feet, to walk there first.
bool Sword2Guide::offScreen(int id, int &x, int &y) const {
	Mouse *mouse = _vm->_mouse;
	ScreenInfo *si = _vm->_screen->getScreenInfo();
	for (uint i = 0; i < mouse->_curMouse; i++) {
		const Common::Rect &r = mouse->_mouseList[i].rect;
		if (mouse->_mouseList[i].id != id)
			continue;
		const int vis = MIN<int>(r.right - si->scroll_offset_x, 640) - MAX<int>(r.left - si->scroll_offset_x, 0);
		// holding something, a walk would drop it: any part on the screen will do
		if (vis >= (_vm->_logic->readVar(OBJECT_HELD) ? 8 : MIN<int>(40, r.width())))
			return false;
		const int ox = (r.left + r.right) / 2 - si->scroll_offset_x;
		x = ox < 0 ? 60 : 580;
		const int gy = si->player_feet_y - si->scroll_offset_y;
		const int tries[] = {gy, gy + 30, gy - 30};
		y = CLIP<int>(tries[_approaches % 3], 0, 399);
		static const int dx[] = {0, -60, 60, -120, 120};
		static const int dy[] = {0, 15, -15, 30, -30};
		for (int a = 0; a < ARRAYSIZE(dx); a++)
			for (int b = 0; b < ARRAYSIZE(dy); b++) {
				const int px = CLIP(x + dx[a], 40, 600), py = CLIP(y + dy[b], 0, 399);
				const int pick = pickAt(px, py);
				// the floor is an object too; anything else (a door) would take George through it
				bool floor = !pick;
				for (uint j = 0; j < mouse->_curMouse && !floor; j++)
					if (mouse->_mouseList[j].id == pick && mouse->_mouseList[j].pointer == NORMAL_MOUSE_ID)
						floor = true;
				if (floor) {
					x = px;
					y = py;
					return true;
				}
			}
		return true;
	}
	return false;
}

// Point at a target for a few frames, then press: the mouse engines only act on the button-down event.
void Sword2Guide::clickOn(Move::Target target, int id, uint16 button) {
	Move m;
	m.target = target;
	m.id = id;
	m.frames = 6;   // long enough for the mouse to be "touching" it
	_moves.push_back(m);
	m.buttons = button;
	m.frames = 1;
	_moves.push_back(m);
	m.buttons = 0;
	m.frames = 3;
	_moves.push_back(m);
}

void Sword2Guide::execute(const Step &st) {
	_moves.clear();
	Move menu;   // into the bottom menu: it slides open
	menu.target = Move::kPoint;
	menu.x = 320;
	menu.y = kMenuY;
	menu.frames = 12;
	switch (st.kind) {
	case Step::kClick:
		clickOn(Move::kObject, st.a, RD_LEFTBUTTONDOWN);
		break;
	case Step::kLook:
		clickOn(Move::kObject, st.a, RD_RIGHTBUTTONDOWN);
		break;
	case Step::kItem:
		_moves.push_back(menu);
		clickOn(Move::kMenuItem, st.a, RD_RIGHTBUTTONDOWN);
		break;
	case Step::kUse:
		_moves.push_back(menu);
		clickOn(Move::kMenuItem, st.a, RD_LEFTBUTTONDOWN);
		clickOn(Move::kObject, st.b, RD_LEFTBUTTONDOWN);
		break;
	case Step::kHold: {
		// take the item and go back to the game area with it: a later click uses it at once
		_moves.push_back(menu);
		clickOn(Move::kMenuItem, st.a, RD_LEFTBUTTONDOWN);
		Move up;
		up.target = Move::kPoint;
		up.x = 320;
		up.y = 200;
		up.frames = 3;
		_moves.push_back(up);
		break;
	}
	case Step::kCombine:
		_moves.push_back(menu);
		clickOn(Move::kMenuItem, st.a, RD_LEFTBUTTONDOWN);
		clickOn(Move::kMenuItem, st.b, RD_LEFTBUTTONDOWN);
		break;
	case Step::kSubject:
		clickOn(Move::kSubject, st.a, RD_LEFTBUTTONDOWN);
		break;
	case Step::kPoint: {
		Move m;
		m.target = Move::kPoint;
		m.x = st.a;
		m.y = st.b;
		m.frames = 3;
		_moves.push_back(m);
		m.buttons = RD_LEFTBUTTONDOWN;
		m.frames = 1;
		_moves.push_back(m);
		m.buttons = 0;
		_moves.push_back(m);
		break;
	}
	default:
		break;
	}
}

Common::String Sword2Guide::name(uint32 res) const {
	if (!res || !_vm->_resman->checkValid(res))
		return "";
	byte buf[NAME_LEN];
	return Common::String((const char *)_vm->_resman->fetchName(res, buf));
}

Common::String Sword2Guide::describe(const Step &st) const {
	static const char *const names[] = {"click", "look", "item", "use", "combine", "subject", "point", "location",
		"var", "wait", "pose", "hold", "gone"};
	return Common::String::format("%s %d %d %d %s", names[st.kind], st.a, st.b, st.c, st.comment.c_str());
}

void Sword2Guide::tick() {
	const uint32 now = g_system->getMillis();
	pollControl();

	ScreenInfo *si = _vm->_screen->getScreenInfo();
	if (si->player_feet_x != _feetX || si->player_feet_y != _feetY) {
		_feetX = si->player_feet_x;
		_feetY = si->player_feet_y;
		_feetSince = now;
	}

	if (!_moves.empty()) {
		Move &m = _moves[0];
		int x, y;
		if (position(m, x, y))
			_mouse = Common::Point(x, y);
		_active = true;
		g_system->warpMouse(_mouse.x, _mouse.y + MENUDEEP);
		if (m.buttons) {
			_vm->_mouseEvent.pending = true;
			_vm->_mouseEvent.buttons = m.buttons;
		}
		if (--m.frames <= 0)
			_moves.remove_at(0);
		if (_moves.empty() && _state == kRunning) {
			_state = kSettle;
			_stateSince = now;
		}
		return;
	}

	if (_state == kStopped) {
		_active = false;
		return;
	}
	if (_vm->_logic->readVar(DEAD) && _saveSections && _current < (int)_steps.size() - 1) {   // not the end credits
		// caught or killed (a guard's round is random): back to the start of this section and try again; the
		// recording leaves the failed try out
		int start = _current, num = 0;
		while (start > 0 && _steps[start].section.empty())
			start--;
		for (int i = 0; i <= start; i++)
			if (!_steps[i].section.empty())
				num++;
		if (num > 0 && _retries < 10) {
			_retries++;
			logf("dead at step %d: retry %d from step %d, slot %d", _current + 1, _retries, start + 1, _saveSections + num - 1);
			_moves.clear();
			setCurrent(start);
			_sectionSaved = start;
			_vm->_gmmLoadSlot = _saveSections + num - 1;
			_state = kWaitIdle;
			_idleSince = 0;
			return;
		}
	}
	if (!_active || _mouse.y < 0 || _mouse.y > 399) {
		// park the mouse in the game area while playing: over a menu, the menu stays open
		_active = true;
		_mouse = Common::Point(320, 200);
		g_system->warpMouse(_mouse.x, _mouse.y + MENUDEEP);
	}

	const bool idle = isIdle();
	if (idle) {
		if (!_idleSince)
			_idleSince = now;
	} else {
		_idleSince = 0;
	}
	const bool choice = choosing();
	if ((!idle && !choice) || _vm->_logic->_speechRunning)
		_busy = true;
	if (choice) {
		if (!_choiceSince)
			_choiceSince = now;
	} else {
		_choiceSince = 0;
	}

	if (_state == kSettle) {
		const Step &cur = _steps[_current];
		if (now - _stateSince < (cur.now ? 300u : kSettle))
			return;
		if (cur.kind == Step::kSubject && !_busy && choice && now - _stateSince > 4000) {
			logf("step %d: no answer, clicking again", _current + 1);
			execute(cur);
			_state = kRunning;
			_stateSince = now;
			return;
		}
		// done once the game reacted (George walked, someone talked, a conversation went on) and waits again,
		// or when nothing happened for a while
		// a quick step (E) is done as soon as the click is in: the next one follows right away
		const bool waited = waitStep(cur) || cur.now;
		const bool reacted = waited || _busy || now - _stateSince > 3000;
		// a conversation can open straight on its subjects, with nothing said first
		const bool ready = waited || (_idleSince && now - _idleSince >= kIdleSettle) ||
			(_choiceSince && now - _choiceSince >= kChoiceSettle && (_busy || now - _stateSince > 3000));
		if (!ready || !reacted)
			return;
		if (_approaching) {
			_approaching = false;
			_state = kWaitIdle;
			return;
		}
		if (cur.kind == Step::kClick && !cur.now && !_spoke && !_retried && !choice && pointerOf(cur.a) == kTalkPointer) {
			// a click on someone far away can end with George just walking up to them: once more (the engine
			// ignores a click on what was clicked last, so that is forgotten first)
			logf("step %d: nobody talked, clicking again", _current + 1);
			_retried = true;
			_busy = false;
			_vm->_logic->writeVar(CLICKED_ID, 0);
			execute(cur);
			_state = kRunning;
			_stateSince = now;
			return;
		}
		const int ptr = pointerOf(cur.a);
		ScreenInfo *si = _vm->_screen->getScreenInfo();
		if (cur.kind == Step::kClick && ptr >= 788 && ptr <= 799 && _exitTries < 3 &&
			(int)_vm->_logic->readVar(LOCATION) == _stepLocation &&
			ABS(si->player_feet_x - _stepFeetX) + ABS(si->player_feet_y - _stepFeetY) < 40) {
			// an exit that did not take George anywhere (something interrupted him: a bird, a scene): again
			_exitTries++;
			logf("step %d: still here, clicking the exit again", _current + 1);
			_busy = false;
			_vm->_logic->writeVar(CLICKED_ID, 0);
			execute(cur);
			_state = kRunning;
			_stateSince = now;
			return;
		}
		logf("step %d done (location %d)", _current + 1, _vm->_logic->readVar(LOCATION));
		next();
		return;
	}

	if (_state == kRunning) {
		// a wait step
		const Step &st = _steps[_current];
		bool done = false;
		if (st.kind == Step::kWait)
			done = now - _stateSince >= (uint32)st.a * 1000;
		else if (st.kind == Step::kScreenWait)
			done = (int)_vm->_logic->readVar(LOCATION) == st.a && idle;
		else if (st.kind == Step::kVarWait)
			done = (int)_vm->_logic->readVar(st.a) == st.b;
		else if (st.kind == Step::kRectWait) {
			// a pose of someone: the left edge of their mouse box (their sprite) in that range
			Mouse *mouse = _vm->_mouse;
			for (uint i = 0; i < mouse->_curMouse; i++)
				if (mouse->_mouseList[i].id == st.a && mouse->_mouseList[i].rect.left >= st.b &&
					mouse->_mouseList[i].rect.left <= st.c)
					done = true;
		}
		else if (st.kind == Step::kGoneWait) {
			// someone out of sight for a while: they left the screen, not just walked behind something
			Mouse *mouse = _vm->_mouse;
			bool here = false;
			for (uint i = 0; i < mouse->_curMouse; i++)
				here = here || mouse->_mouseList[i].id == st.a;
			if (here)
				_goneSince = 0;
			else if (!_goneSince)
				_goneSince = now;
			done = _goneSince && now - _goneSince >= (uint32)st.b;
		}
		if (done) {
			_goneSince = 0;
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
	const bool choiceStep = st.kind == Step::kSubject;
	if (choiceStep) {
		if (!_choiceSince && _idleSince && now - _idleSince > 8000) {
			logf("step %d: no conversation, skipped", _current + 1);
			next();
			return;
		}
		if (!_choiceSince || now - _choiceSince < kChoiceSettle)
			return;
	} else if (st.now) {
		if (_vm->_mouse->_mouseStatus)
			return;   // as soon as the player has control, even while someone walks
	} else if (!waitStep(st) && (!_idleSince || now - _idleSince < kIdleSettle)) {
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
			_vm->saveGame(slot, (const byte *)st.section.c_str());
		_retries = 0;
		logf("section %d step %d slot %d: %s", num, _current + 1, slot, st.section.c_str());
	}
	if ((st.kind == Step::kItem || st.kind == Step::kHold || st.kind == Step::kUse || st.kind == Step::kCombine) &&
		_vm->_mouse->_mouseMode == MOUSE_normal)
		refreshItems();
	if (!available(st) && st.kind == Step::kClick && _current > 0 && _steps[_current - 1].kind == Step::kClick &&
		_steps[_current - 1].a == st.a && (int)_vm->_logic->readVar(LOCATION) != _stepLocation) {
		// a click repeated in case the first one only walked there: the first one was enough
		logf("step %d: already through", _current + 1);
		next();
		return;
	}
	if (!available(st)) {
		if (!_missingSince)
			_missingSince = now;
		if (choiceStep && _choiceSince && now - _choiceSince > 5000) {
			logf("step %d: no such subject, skipped", _current + 1);
			_missingSince = 0;
			next();
			return;
		}
		if (now - _missingSince > kMissing) {
			logf("step %d: its object is not here (location %d); stopping", _current + 1,
				_vm->_logic->readVar(LOCATION));
			_state = kStopped;
			_missingSince = 0;
		}
		return;
	}
	_missingSince = 0;
	int wx, wy;
	const int target = st.kind == Step::kUse ? st.b : st.a;
	if ((st.kind == Step::kClick || st.kind == Step::kLook || st.kind == Step::kUse) && _approaches < 6 &&
		offScreen(target, wx, wy)) {
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
	_stepLocation = _vm->_logic->readVar(LOCATION);
	_stepFeetX = _vm->_screen->getScreenInfo()->player_feet_x;
	_stepFeetY = _vm->_screen->getScreenInfo()->player_feet_y;
	// the mouse engine ignores a second click on what was clicked last ("re-click"): forget it, a walkthrough
	// that clicks the same thing twice means it (a wheel turned twice)
	_vm->_logic->writeVar(CLICKED_ID, 0);
	_spoke = false;
	_busy = false;
	_state = kRunning;
	_stateSince = now;
	execute(st);
}

// The inventory list is only brought up to date when the menu opens: build it without showing the menu.
void Sword2Guide::refreshItems() {
	Mouse *mouse = _vm->_mouse;
	const uint8 status = mouse->_menuStatus[RDMENU_BOTTOM];
	mouse->buildMenu();
	mouse->_menuStatus[RDMENU_BOTTOM] = status;
}

int Sword2Guide::pointerOf(int id) const {
	Mouse *mouse = _vm->_mouse;
	for (uint i = 0; i < mouse->_curMouse; i++)
		if (mouse->_mouseList[i].id == id)
			return mouse->_mouseList[i].pointer;
	return 0;
}

void Sword2Guide::onSpeak(uint32 id, const char *text) {
	_spoke = true;
	logf("talk %u: %s", id, text);
}

void Sword2Guide::listObjects() {
	Mouse *mouse = _vm->_mouse;
	Logic *logic = _vm->_logic;
	ScreenInfo *si = _vm->_screen->getScreenInfo();
	logf("location %d, idle %d, human %d, mode %d, held %d, subjects %d, choosing %d, speech %d, feet %d,%d, "
		"scroll %d,%d, guide state %d step %d", logic->readVar(LOCATION), isIdle(), !mouse->_mouseStatus,
		mouse->_mouseMode, logic->readVar(OBJECT_HELD), logic->readVar(IN_SUBJECT), choosing(), logic->_speechRunning,
		si->player_feet_x, si->player_feet_y, si->scroll_offset_x, si->scroll_offset_y, _state, _current + 1);
	for (uint i = 0; i < mouse->_curMouse; i++) {
		const MouseUnit &u = mouse->_mouseList[i];
		logf("  object %d '%s' at %d,%d (%d,%d-%d,%d) priority %d pointer %d", u.id, name(u.id).c_str(),
			(u.rect.left + u.rect.right) / 2 - si->scroll_offset_x, (u.rect.top + u.rect.bottom) / 2 - si->scroll_offset_y,
			u.rect.left, u.rect.top, u.rect.right, u.rect.bottom, u.priority, u.pointer);
	}
	if (mouse->_mouseMode != MOUSE_menu && mouse->_mouseMode != MOUSE_drag && !mouse->_choosing)
		refreshItems();
	for (uint i = 0; i < mouse->_totalMasters; i++)
		logf("  item %d '%s'", mouse->_masterMenuList[i].icon_resource, name(mouse->_masterMenuList[i].icon_resource).c_str());
	for (uint i = 0; i < logic->readVar(IN_SUBJECT); i++)
		logf("  subject %d '%s'", mouse->_subjectList[i].res, name(mouse->_subjectList[i].res).c_str());
}

void Sword2Guide::runCommand(const Common::String &text) {
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
	} else if (c == "objects") {
		listObjects();
	} else if (c == "var") {
		logf("var %d = %d", x, _vm->_logic->readVar(x));
	} else if (c == "debuglevel") {
		gDebugLevel = x;   // e.g. 9 traces every script opcode, to read a puzzle's checks
		logf("debug level %d", x);
	} else if (c == "diff") {
		// the script variables that changed since the last "diff" (to find a puzzle's state)
		const int count = 2000;
		for (int i = 0; i < count; i++) {
			const uint32 v = _vm->_logic->readVar(i);
			if (i < (int)_varSnap.size() && _varSnap[i] != v)
				logf("var %d: %u -> %u", i, _varSnap[i], v);
		}
		_varSnap.resize(count);
		for (int i = 0; i < count; i++)
			_varSnap[i] = _vm->_logic->readVar(i);
	} else if (c == "setvar") {
		_vm->_logic->writeVar(x, y);
		logf("var %d = %d", x, y);
	} else if (c == "save") {
		_vm->saveGame(x, (const byte *)Common::String::format("guide %d", x).c_str());
		logf("saved slot %d", x);
	} else if (c == "load") {
		_vm->_gmmLoadSlot = x;   // the main loop loads it
		logf("loading slot %d", x);
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

void Sword2Guide::pollControl() {
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

} // End of namespace Sword2
