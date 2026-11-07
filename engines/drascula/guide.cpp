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

#include "drascula/drascula.h"
#include "drascula/guide.h"

namespace Drascula {

static const uint32 kSettle = 500;       // ms a step waits after its last click
static const uint32 kIdleSettle = 300;   // ms the game must stay idle
static const uint32 kMissing = 30000;    // ms to wait for a step's object to show up
static const uint32 kNoConverse = 8000;  // ms without a conversation menu before a line step is skipped

DrasculaGuide::DrasculaGuide(DrasculaEngine *vm) : _vm(vm), _current(0), _remaining(0), _state(kStopped),
	  _stateSince(0), _idleSince(0), _missingSince(0), _converseSince(0), _inConverse(false), _pressed(false),
	  _bottom(nullptr), _hideCursor(false), _logTalk(true), _saveSections(0), _sectionSaved(-1), _controlOffset(0),
	  _mouseX(160), _mouseY(100) {
	if (ConfMan.hasKey("guide_file"))
		load(ConfMan.get("guide_file"));
	if (ConfMan.hasKey("guide_control"))
		_controlPath = ConfMan.get("guide_control");
	if (ConfMan.hasKey("guide_hide_cursor"))
		_hideCursor = ConfMan.getBool("guide_hide_cursor");
	if (ConfMan.hasKey("guide_save_sections"))
		_saveSections = ConfMan.getInt("guide_save_sections");
	if (ConfMan.hasKey("guide_play") && ConfMan.getBool("guide_play"))
		_state = kWaitIdle;
}

void DrasculaGuide::logf(const char *fmt, ...) {
	va_list va;
	va_start(va, fmt);
	Common::String s = Common::String::vformat(fmt, va);
	va_end(va);
	debug("guide: %s", s.c_str());
}

bool DrasculaGuide::parseLine(const Common::String &text, Step &st) const {
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
	case 'e': st.kind = Step::kObject; return n == 2;
	case 'u': st.kind = Step::kUse; return n == 2;
	case 'i': st.kind = Step::kInventory; return n == 2;
	case 'k': st.kind = Step::kOption; return n == 1 && st.a >= 1 && st.a <= 4;
	case 'g': st.kind = Step::kPoint; return n == 2;
	case 'r': st.kind = Step::kRoomWait; return n == 1;
	case 'n': st.kind = Step::kFlagWait; return n == 2;
	case 'w': st.kind = Step::kWait; return n == 1;
	default: return false;
	}
}

bool DrasculaGuide::load(const Common::String &path) {
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

void DrasculaGuide::setCurrent(int step) {
	_current = CLIP<int>(step, 0, _steps.size());
}

void DrasculaGuide::next() {
	setCurrent(_current + 1);
	_state = kWaitIdle;
	if (_remaining > 0 && --_remaining == 0)
		_state = kStopped;
}

// The index of a room object by its number, or -1.
int DrasculaGuide::objectIndex(int num) const {
	for (int l = 0; l < _vm->numRoomObjs; l++)
		if (_vm->objectNum[l] == num)
			return l;
	return -1;
}

// The inventory slot holding a verb (1-6) or an item, or 0.
int DrasculaGuide::slotOf(int thing) const {
	for (int n = 1; n < ARRAYSIZE(_vm->inventoryObjects); n++)
		if (_vm->inventoryObjects[n] == thing)
			return n;
	return 0;
}

bool DrasculaGuide::available(const Step &st) const {
	const int held = _vm->takeObject ? _vm->pickedObject : 0;
	switch (st.kind) {
	case Step::kObject:
	case Step::kUse: {
		if (st.kind == Step::kUse && held != st.a && !slotOf(st.a))
			return false;
		if (st.b == 50)
			return true;   // John himself
		const int l = objectIndex(st.b);
		return l >= 0 && (st.kind == Step::kObject && st.a == kVerbNone ? true : _vm->visible[l] == 1);
	}
	case Step::kInventory:
		// what John holds goes back to the inventory first
		return (slotOf(st.b) || held == st.b) && (held == st.a || slotOf(st.a));
	case Step::kOption:
		return _bottom != nullptr;
	default:
		return true;
	}
}

// Where a move points, in game coordinates (320x200).
bool DrasculaGuide::position(const Move &m, int &x, int &y) const {
	switch (m.target) {
	case Move::kObject: {
		const int l = objectIndex(m.id);
		if (l < 0)
			return false;
		const Common::Rect &r = _vm->_objectRect[l];
		const int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
		// the point nearest the middle that is on no other object (a click acts on every object under it) and,
		// in chapter 2, not on John (he comes first there)
		int best = -1;
		x = cx;
		y = cy;
		for (int py = r.top + 1; py < r.bottom; py += 2)
			for (int px = r.left + 1; px < r.right; px += 2) {
				bool clear = !(_vm->currentChapter == 2 && px > _vm->curX && py > _vm->curY &&
					px < _vm->curX + _vm->curWidth && py < _vm->curY + _vm->curHeight);
				for (int o = 0; o < _vm->numRoomObjs && clear; o++)
					if (o != l && _vm->_objectRect[o].contains(Common::Point(px, py)) &&
						(_vm->visible[o] == 1 || _vm->isDoor[o]))
						clear = false;
				if (!clear)
					continue;
				const int d = ABS(px - cx) + ABS(py - cy);
				if (best < 0 || d < best) {
					best = d;
					x = px;
					y = py;
				}
			}
		return true;
	}
	case Move::kJohn:
		x = _vm->curX + _vm->curWidth / 2;
		y = _vm->curY + _vm->curHeight / 2;
		return true;
	case Move::kBar:
		x = (_vm->_verbBarX[m.id] + _vm->_verbBarX[m.id + 1]) / 2;
		y = 10;
		return true;
	case Move::kSlot: {
		const int n = slotOf(m.id);
		if (!n)
			return false;
		x = _vm->_itemLocations[n].x + OBJWIDTH / 2;
		y = _vm->_itemLocations[n].y + OBJHEIGHT / 2;
		return true;
	}
	case Move::kOption:
		if (!_bottom)
			return false;
		x = 100;
		y = ((m.id > 1 ? _bottom[m.id - 2] : 0) + _bottom[m.id - 1]) / 2;
		return true;
	default:
		x = m.x;
		y = m.y;
		return true;
	}
}

// Point at a target for a few passes, then a click (1 left, 2 right) and a short rest.
void DrasculaGuide::push(Move::Target target, int id, int button, int x, int y) {
	Move m;
	m.target = target;
	m.id = id;
	m.x = x;
	m.y = y;
	m.button = 0;
	m.frames = button == 2 ? 1 : 3;
	_moves.push_back(m);
	m.button = button;
	m.frames = 1;
	_moves.push_back(m);
	m.button = 0;
	m.frames = 2;
	_moves.push_back(m);
}

// Put back whatever verb or item John holds: the empty end of the verb bar (only with the inventory closed).
void DrasculaGuide::dropHeld() {
	push(Move::kBar, kVerbNone, 1);
}

void DrasculaGuide::execute(const Step &st) {
	_moves.clear();
	const int held = _vm->takeObject ? _vm->pickedObject : 0;
	const bool menu = _vm->_menuScreen;
	switch (st.kind) {
	case Step::kObject:
	case Step::kPoint: {
		if (menu)
			push(Move::kPoint, 0, 2, _mouseX, _mouseY);   // close the inventory
		const int verb = st.kind == Step::kObject ? st.a : kVerbNone;
		if (held != verb)
			push(Move::kBar, verb, 1);
		if (st.kind == Step::kPoint)
			push(Move::kPoint, 0, 1, st.a, st.b);
		else
			push(st.b == 50 && objectIndex(50) < 0 ? Move::kJohn : Move::kObject, st.b, 1);
		break;
	}
	case Step::kUse:
		if (held != st.a || menu) {
			if (menu)
				push(Move::kPoint, 0, 2, _mouseX, _mouseY);
			if (held && held != st.a)
				dropHeld();
			if (held != st.a) {
				push(Move::kPoint, 0, 2, 160, 100);   // open the inventory
				push(Move::kSlot, st.a, 1);
				push(Move::kPoint, 0, 2, 160, 100);
			}
		}
		push(st.b == 50 && objectIndex(50) < 0 ? Move::kJohn : Move::kObject, st.b, 1);
		break;
	case Step::kInventory:
		if (!menu || (held && held != st.a)) {
			if (menu)
				push(Move::kPoint, 0, 2, _mouseX, _mouseY);
			if (held && held != st.a)
				dropHeld();
			push(Move::kPoint, 0, 2, 160, 100);
		}
		if (held != st.a)
			push(Move::kSlot, st.a, 1);
		push(Move::kSlot, st.b, 1);
		break;
	case Step::kOption:
		push(Move::kOption, st.a, 1);
		break;
	default:
		break;
	}
}

void DrasculaGuide::runMoves(bool converse) {
	Move &m = _moves[0];
	int x, y;
	if (position(m, x, y)) {
		_mouseX = x;
		_mouseY = y;
	}
	_vm->_mouseX = _mouseX;
	_vm->_mouseY = _mouseY;
	if (m.button == 1) {
		_vm->_leftMouseButton = 1;
		_pressed = true;
	} else if (m.button == 2) {
		_vm->_rightMouseButton = 1;
	}
	if (--m.frames <= 0)
		_moves.remove_at(0);
	if (_moves.empty() && _state == kRunning) {
		_state = kSettle;
		_stateSince = g_system->getMillis();
	}
}

void DrasculaGuide::afterEvents() {
	if (_pressed) {
		_pressed = false;
		_vm->_leftMouseButton = 0;
	}
}

Common::String DrasculaGuide::describe(const Step &st) const {
	static const char *const names[] = {"object", "use", "inventory", "line", "point", "room", "flag", "wait"};
	return Common::String::format("%s %d %d %s", names[st.kind], st.a, st.b, st.comment.c_str());
}

void DrasculaGuide::tick() {
	pollControl();
	if (_inConverse) {
		_inConverse = false;
		_bottom = nullptr;
	}
	if (_pendingLoad >= 0) {
		_moves.clear();
		_vm->loadGameState(_pendingLoad);
		logf("loaded slot %d", _pendingLoad);
		_pendingLoad = -1;
		return;
	}
	if (!_moves.empty()) {
		runMoves(false);
		return;
	}
	step(_steps.empty() ? Step() : _steps[MIN<int>(_current, _steps.size() - 1)], g_system->getMillis(), false);
}

void DrasculaGuide::tickConverse(const int bottom[4]) {
	pollControl();
	_bottom = bottom;
	if (!_inConverse) {
		_inConverse = true;
		_converseSince = g_system->getMillis();
	}
	if (!_moves.empty()) {
		runMoves(true);
		return;
	}
	step(_steps.empty() ? Step() : _steps[MIN<int>(_current, _steps.size() - 1)], g_system->getMillis(), true);
}

// One pass of the guide outside its clicks; converse: the conversation menu waits for a line.
void DrasculaGuide::step(const Step &cur, uint32 now, bool converse) {
	if (_state == kStopped)
		return;

	const bool idle = converse || !_vm->_characterMoved;
	if (idle) {
		if (!_idleSince)
			_idleSince = now;
	} else {
		_idleSince = 0;
	}

	if (_state == kSettle) {
		if (now - _stateSince < kSettle || !_idleSince || now - _idleSince < kIdleSettle)
			return;
		const int l = cur.kind == Step::kObject && cur.a == kVerbNone ? objectIndex(cur.b) : -1;
		if (l >= 0 && _vm->isDoor[l] && _vm->_roomNumber == _startRoom && !_retried) {
			// a click on a door that left John in the room (it sometimes takes a second one): again
			logf("step %d: still in room %d, clicking again", _current + 1, _startRoom);
			_retried = true;
			_state = kRunning;
			execute(cur);
			return;
		}
		logf("step %d done (room %d)", _current + 1, _vm->_roomNumber);
		next();
		return;
	}

	if (_state == kRunning) {
		// a wait step
		bool done = false;
		if (cur.kind == Step::kWait)
			done = now - _stateSince >= (uint32)cur.a * 1000;
		else if (cur.kind == Step::kRoomWait)
			done = _vm->_roomNumber == cur.a && !converse && idle;
		else if (cur.kind == Step::kFlagWait)
			done = _vm->flags[cur.a] == cur.b;
		if (done) {
			logf("step %d done (room %d)", _current + 1, _vm->_roomNumber);
			next();
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
	const bool waitStep = st.kind == Step::kWait || st.kind == Step::kRoomWait || st.kind == Step::kFlagWait;
	if (st.kind == Step::kOption) {
		if (!converse) {
			if (_idleSince && now - _idleSince > kNoConverse) {
				// the conversation ended on its own (a goodbye that was not needed): go on
				logf("step %d: no conversation, skipped", _current + 1);
				next();
			}
			return;
		}
		if (now - _converseSince < kIdleSettle)
			return;
	} else if (!waitStep && (converse || !_idleSince || now - _idleSince < kIdleSettle)) {
		return;
	}
	if (!st.section.empty() && _sectionSaved != _current && !converse) {
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
		if (now - _missingSince > kMissing) {
			logf("step %d: its object is not here (room %d); stopping", _current + 1, _vm->_roomNumber);
			_state = kStopped;
			_missingSince = 0;
		}
		return;
	}
	_missingSince = 0;
	logf("step %d: %s", _current + 1, describe(st).c_str());
	_state = kRunning;
	_stateSince = now;
	_idleSince = 0;
	_startRoom = _vm->_roomNumber;
	_retried = false;
	// a cutscene that takes John through a door leaves doBreak set, and the next click on the room is lost
	// (the player's first click after such a scene does nothing in the original game too)
	_vm->doBreak = 0;
	if (!waitStep)
		execute(st);
}

void DrasculaGuide::onTalk(const char *file, const char *said) {
	if (_logTalk && said)
		logf("talk %s: %s", file, said);
}

void DrasculaGuide::onConverse(const char *const lines[4]) {
	for (int i = 0; i < 4; i++)
		logf("line %d: %s", i + 1, lines[i]);
}

void DrasculaGuide::listObjects() {
	logf("chapter %d room %d, moving %d, menu %d, held %d, converse %d, john at %d,%d, guide state %d step %d",
		_vm->currentChapter, _vm->_roomNumber, _vm->_characterMoved, _vm->_menuScreen,
		_vm->takeObject ? _vm->pickedObject : 0, _inConverse, _vm->curX, _vm->curY, _state, _current + 1);
	for (int l = 0; l < _vm->numRoomObjs; l++) {
		const Common::Rect &r = _vm->_objectRect[l];
		logf("  object %d '%s' at %d,%d (%d,%d-%d,%d) visible %d door %d%s", _vm->objectNum[l], _vm->objName[l],
			(r.left + r.right) / 2, (r.top + r.bottom) / 2, r.left, r.top, r.right, r.bottom, _vm->visible[l],
			_vm->isDoor[l], _vm->isDoor[l] ? Common::String::format(" to room %d", _vm->_doorDestRoom[l]).c_str() : "");
	}
	Common::String items;
	for (int n = 1; n < ARRAYSIZE(_vm->inventoryObjects); n++)
		if (_vm->inventoryObjects[n])
			items += Common::String::format(" %d", _vm->inventoryObjects[n]);
	logf("  inventory:%s", items.c_str());
}

void DrasculaGuide::runCommand(const Common::String &text) {
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
	} else if (c == "flag") {
		logf("flag %d = %d", x, _vm->flags[x]);
	} else if (c == "flags") {
		Common::String s;
		for (int i = 0; i < NUM_FLAGS; i++)
			if (_vm->flags[i])
				s += Common::String::format(" %d=%d", i, _vm->flags[i]);
		logf("flags:%s", s.c_str());
	} else if (c == "setflag") {
		_vm->flags[x] = y;
		logf("flag %d = %d", x, y);
	} else if (c == "save") {
		_vm->saveGameState(x, Common::String::format("guide %d", x));
		logf("saved slot %d", x);
	} else if (c == "load") {
		_pendingLoad = x;   // from the main loop only, not from a conversation
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

void DrasculaGuide::pollControl() {
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

} // End of namespace Drascula
