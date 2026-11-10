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

#include "agos/agos.h"
#include "agos/guide.h"
#include "agos/intern.h"
#include "agos/sound.h"

namespace AGOS {

static const uint32 kSettle = 1000;      // ms a step waits before it can be done (Simon starts walking or talking)
static const uint32 kIdleSettle = 500;   // ms the game must stay idle
static const uint32 kRoomSettle = 800;   // ms a new room must be up before The Feeble Files takes a click
static const uint32 kMissing = 30000;    // ms to wait for a step's item to show up

AGOSGuide::AGOSGuide(AGOSEngine *vm) : _vm(vm), _current(0), _remaining(0), _state(kStopped), _stateSince(0),
	  _idleSince(0), _missingSince(0), _hideCursor(false), _logTalk(true), _saveSections(0), _sectionSaved(-1),
	  _controlOffset(0) {
	if (ConfMan.hasKey("guide_file"))
		load(ConfMan.get("guide_file"));
	if (ConfMan.hasKey("guide_control"))
		_controlPath = ConfMan.get("guide_control");
	if (ConfMan.hasKey("guide_hide_cursor"))
		_hideCursor = ConfMan.getBool("guide_hide_cursor");
	if (ConfMan.hasKey("guide_save_sections"))
		_saveSections = ConfMan.getInt("guide_save_sections");
}

void AGOSGuide::logf(const char *fmt, ...) {
	va_list va;
	va_start(va, fmt);
	Common::String s = Common::String::vformat(fmt, va);
	va_end(va);
	debug("guide: %s", s.c_str());
}

bool AGOSGuide::parseLine(const Common::String &text, Step &st) const {
	Common::String line = text, comment;
	size_t semi = line.findFirstOf(';');
	if (semi != Common::String::npos) {
		comment = line.substr(semi + 1);
		comment.trim();
		line = line.substr(0, semi);
	}
	line.trim();
	st.onlyRoom = 0;
	if (line.hasPrefix("@")) {
		// "@<room> <step>": only when Simon is in that room, else skipped
		st.onlyRoom = atoi(line.c_str() + 1);
		size_t sp = line.findFirstOf(' ');
		if (sp == Common::String::npos)
			return false;
		line = line.substr(sp + 1);
		line.trim();
	}
	if (line == "m" || line == "L")
		line += ' ';   // the steps without numbers
	if (line.size() < 2 || line[1] != ' ')
		return false;
	if (line.contains('$')) {
		// "$<var>[+<n>]": a number the game decides as it plays (a script variable, plus n), read when the step
		// comes up
		st.raw = line;
		line = withVariables(line);
	}
	st.a = st.b = st.c = 0;
	st.comment = comment;
	int n = sscanf(line.c_str() + 2, "%d %d %d", &st.a, &st.b, &st.c);
	switch (line[0]) {
	case 'e': st.kind = Step::kVerb; return n >= 2;
	case 'E': st.kind = Step::kVerb; st.now = true; return n >= 2;
	case 'h': st.kind = Step::kBox; return n >= 1;
	case 'B': st.kind = Step::kBox; st.now = true; return n >= 1;
	case 'k': st.kind = Step::kChoice; return n >= 1;
	case 'C':
		st.optional = true;
		// fall through
	case 'c':
		st.kind = Step::kChoiceText;
		st.text = line.substr(2);
		st.text.trim();
		return !st.text.empty();
	case 'g': st.kind = Step::kPoint; return n == 2;
	case 'p': st.kind = Step::kPathPoint; return n == 2;
	case 'x': st.kind = Step::kPathPoint; st.exit = true; return n == 2;
	case 'r': st.kind = Step::kRoomWait; return n >= 1;
	case 'n': st.kind = Step::kVarWait; return n == 2;
	case 'N': st.kind = Step::kVarWait; st.check = true; return n == 3 && st.c < 0;
	case 'b': st.kind = Step::kBitWait; return n == 2;
	case 'Q': st.kind = Step::kBitWait; st.check = true; return n == 3 && st.c < 0;
	case 'w': st.kind = Step::kWait; return n >= 1;
	case 's': st.kind = Step::kSwitch; return feeble() && n == 1;
	case 'v': st.kind = Step::kVar2Wait; return feeble() && n >= 2;
	case 'm': st.kind = Step::kRoomLeave; return true;
	case 'L': st.kind = Step::kCloseUp; return feeble();
	case 'P': st.kind = Step::kPairs; return n == 3 && st.b > 0;
	case 'H': st.kind = Step::kHold; return n == 3;
	case 'K': st.kind = Step::kClickTo; return n == 3;
	case 'O': st.kind = Step::kLights; return n == 3 && st.b > 0 && st.b <= 8;
	case 'l': st.kind = Step::kLoop; return n == 2 && st.a > 0;
	case 't':
		st.kind = Step::kTalkWait;
		st.text = line.substr(2);
		st.text.trim();
		return !st.text.empty();
	default: return false;
	}
}

Common::String AGOSGuide::withVariables(const Common::String &line) const {
	Common::String out;
	for (uint i = 0; i < line.size(); i++) {
		if (line[i] != '$') {
			out += line[i];
			continue;
		}
		int var = 0, add = 0;
		while (i + 1 < line.size() && Common::isDigit(line[i + 1]))
			var = var * 10 + (line[++i] - '0');
		if (i + 1 < line.size() && line[i + 1] == '+') {
			i++;
			while (i + 1 < line.size() && Common::isDigit(line[i + 1]))
				add = add * 10 + (line[++i] - '0');
		}
		const int value = var < _vm->_numVars ? _vm->_variableArray[var] : 0;
		out += Common::String::format("%d", value + add);
	}
	return out;
}

bool AGOSGuide::load(const Common::String &path) {
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

void AGOSGuide::setCurrent(int step) {
	_current = CLIP<int>(step, 0, _steps.size());
}

// An item's number, or -2 for "me" and -3 for "the room" (the boxes that stand for them), -1 for none.
int AGOSGuide::itemId(Item *item) const {
	if (!item)
		return -1;
	if (item == _vm->_dummyItem2)
		return -2;
	if (item == _vm->_dummyItem3)
		return -3;
	for (uint i = 0; i != _vm->_itemArraySize; i++)
		if (_vm->_itemArrayPtr[i] == item)
			return i;
	return -1;
}

int AGOSGuide::room() const {
	Item *me = _vm->me();
	return me ? me->parent : 0;
}

// What Feeble carries, in one number.
int AGOSGuide::inventoryCount() const {
	uint32 h = 0;
	Item *me = _vm->me();
	for (Item *it = me ? _vm->derefItem(me->child) : nullptr; it; it = _vm->derefItem(it->next))
		h = h * 31 + (uint32)itemId(it);
	return (int)h;
}

// What the room offers to click, in one number (to tell whether a step changed anything).
uint32 AGOSGuide::boxSignature() const {
	uint32 h = 0;
	for (uint i = 0; i < ARRAYSIZE(_vm->_hitAreas); i++) {
		const HitArea *ha = &_vm->_hitAreas[i];
		if (!(ha->flags & kBFBoxInUse) || (ha->flags & kBFBoxDead))
			continue;
		h = h * 31 + ha->id;
		h = h * 31 + (uint32)itemId(ha->itemPtr);
		h = h * 31 + (ha->flags >> 8);
	}
	return h;
}

// The game's bit flags, in one number, but for the first and fourth words of the first array, which the scripts
// flip all the time (walking, the verb under way).
uint32 AGOSGuide::flagSignature() const {
	uint32 h = 0;
	for (uint i = 0; i < ARRAYSIZE(_vm->_bitArray); i++)
		if (i != 0 && i != 3)
			h = h * 31 + _vm->_bitArray[i];
	for (uint i = 0; i < ARRAYSIZE(_vm->_bitArrayTwo); i++)
		h = h * 31 + _vm->_bitArrayTwo[i];
	for (uint i = 0; i < ARRAYSIZE(_vm->_bitArrayThree); i++)
		h = h * 31 + _vm->_bitArrayThree[i];
	return h;
}

// A bit flag; from 256 on, the second array's (SET_BIT2 n is 256 + n).
bool AGOSGuide::bit(int n) const {
	if (n >= 512)
		return (_vm->_bitArrayThree[(n - 512) / 16] & (1 << ((n - 512) & 15))) != 0;
	if (n >= 256)
		return (_vm->_bitArrayTwo[(n - 256) / 16] & (1 << ((n - 256) & 15))) != 0;
	return _vm->getBitFlag(n);
}

// A verb step followed by a choice, with choices already up: the conversation it should start already started.
bool AGOSGuide::conversationAhead() const {
	if (_current + 1 >= (int)_steps.size() || _steps[_current].kind != Step::kVerb)
		return false;
	const Step::Kind next = _steps[_current + 1].kind;
	return (next == Step::kChoice || next == Step::kChoiceText) && !choices().empty();
}

bool AGOSGuide::feeble() const {
	return _vm->getGameType() == GType_FF;
}

// How far the room is scrolled at a screen point: Simon 2 scrolls in steps of 8 pixels and only above its panel
// (and on the map); The Feeble Files scrolls the whole screen, both ways.
int AGOSGuide::scrollXAt(int y) const {
	if (feeble())
		return _vm->_scrollX;
	return (_vm->getBitFlag(79) || y < 134) ? _vm->_scrollX * 8 : 0;
}

int AGOSGuide::scrollY() const {
	return feeble() ? _vm->_scrollY : 0;
}

int AGOSGuide::screenW() const {
	return _vm->_screenWidth;
}

int AGOSGuide::screenH() const {
	return _vm->_screenHeight;
}

// The bottom of the room on the screen (Simon 2's panel is below).
int AGOSGuide::roomBottom() const {
	return feeble() ? screenH() - 1 : 133;
}

// The game waits for the player: the main input loop or a conversation's choices, the pointer on, nobody
// talking, Simon standing.
bool AGOSGuide::isIdle() const {
	if (_vm->_mouseHideCount)
		return false;
	// a conversation waits for a line only once it printed its lines after the last click (the old ones stay
	// up while the game answers)
	// (The Feeble Files' menus wait the way "use with" does, but with a pointer of their own)
	const bool menu = feeble() && _vm->_guideWaiting == 2 && _vm->_mouseCursor != 5;
	if (_vm->_guideWaiting != 1 && !menu && !(_vm->_guideWaiting == 2 && !choices().empty() && _choiceGen != _clickGen))
		return false;
	// (people talking among themselves in The Feeble Files, the pointer on: the game takes clicks)
	if (_vm->_sound->isVoiceActive() && !(feeble() && _vm->_guideWaiting == 1))
		return false;
	if (_vm->getBitFlag(11)) {
		// Simon walks; the flag sometimes stays on with Simon standing (after a load, after a blocked walk):
		// then only for a while
		if (!_walkSince)
			_walkSince = g_system->getMillis();
		if (g_system->getMillis() - _walkSince < (feeble() ? 30000u : 6000u))   // Feeble walks across whole rooms
			return false;
	} else {
		_walkSince = 0;
	}
	return true;
}

Common::String AGOSGuide::itemName(Item *item) const {
	if (!item || itemId(item) < 0)
		return "";
	SubObject *sub = (SubObject *)_vm->findChildOfType(item, kObjectType);
	if (!sub)
		return "";
	const byte *s = _vm->getStringPtrByID(sub->objectName);
	if ((!s || !s[0]) && (sub->objectFlags & kOFText))
		s = _vm->getStringPtrByID(sub->objectFlagValue[0]);
	return s ? Common::String((const char *)s) : "";
}

HitArea *AGOSGuide::findItemBox(int item) const {
	HitArea *best = nullptr;
	for (uint i = 0; i < ARRAYSIZE(_vm->_hitAreas); i++) {
		HitArea *ha = &_vm->_hitAreas[i];
		if (!(ha->flags & kBFBoxInUse) || (ha->flags & kBFBoxDead) || !ha->itemPtr)
			continue;
		if (ha->id == 0x7FFB || ha->id == 0x7FFC)
			continue;   // the inventory's arrows carry an item too
		if (itemId(ha->itemPtr) != item)
			continue;
		if (!best || ha->priority > best->priority)
			best = ha;
	}
	return best;
}

HitArea *AGOSGuide::findBoxId(int id) const {
	// The Feeble Files' Oracle takes boxes 20 and 80 to 82 while it is up: a room's boxes with those ids come
	// back once it closes
	if (feeble() && _vm->getBitFlag(99) && (id == 20 || (id >= 80 && id <= 82)))
		return nullptr;
	for (uint i = 0; i < ARRAYSIZE(_vm->_hitAreas); i++) {
		HitArea *ha = &_vm->_hitAreas[i];
		if ((ha->flags & kBFBoxInUse) && !(ha->flags & kBFBoxDead) && ha->id == id)
			return ha;
	}
	return nullptr;
}

// A conversation's choices: its lines in the panel (item boxes a few text lines high), top to bottom.
Common::Array<HitArea *> AGOSGuide::choices() const {
	Common::Array<HitArea *> list;
	for (uint i = 0; i < ARRAYSIZE(_vm->_hitAreas); i++) {
		HitArea *ha = &_vm->_hitAreas[i];
		if (!(ha->flags & kBFBoxInUse) || (ha->flags & kBFBoxDead))
			continue;
		if (feeble()) {
			// The Feeble Files: lines at the bottom of the screen, lit under the pointer
			if (!ha->itemPtr || !(ha->flags & kBFInvertTouch) || itemId(ha->itemPtr) <= 0 || ha->id < 50 || ha->id >= 100)
				continue;
		} else if ((ha->flags & (kBFTextBox | kBFBoxItem)) || !ha->itemPtr || ha->y < 134 || ha->height > 40 ||
				itemId(ha->itemPtr) <= 0 || ha->id < 50 || ha->id >= 100) {
			continue;   // the verbs and the inventory's arrows sit in the panel too
		}
		uint j = 0;
		while (j < list.size() && list[j]->y <= ha->y)
			j++;
		list.insert_at(j, ha);
	}
	return list;
}

// The number of the choice whose text has these words (the latest choices printed), 0 for none.
int AGOSGuide::choiceWith(const Common::String &text) const {
	if (choices().empty())
		return 0;
	for (uint i = 0; i < _choiceText.size(); i++)
		if (_choiceText[i].contains(text))
			return i + 1;
	return 0;
}

// How many walking paths a room can have, and the mark at the end of one (as GETPATHPOSN reads them).
int AGOSGuide::maxPaths() const {
	return feeble() ? 100 : 20;
}

uint AGOSGuide::pathEnd() const {
	return feeble() ? 9999 : 999;
}

// A point of one of the room's walking paths (as GETPATHPOSN numbers them: path 1.., point 0..), in room
// coordinates; where the scripts put the exits.
bool AGOSGuide::pathPoint(int path, int point, int &x, int &y) const {
	if (path < 1 || path > maxPaths())
		return false;
	const uint16 *p = (const uint16 *)_vm->_pathFindArray[path - 1];
	if (!p)
		return false;
	for (int j = 0; _vm->readUint16Wrapper(&p[0]) != pathEnd(); j++, p += 2)
		if (j == point) {
			x = (int16)_vm->readUint16Wrapper(&p[0]);
			y = _vm->readUint16Wrapper(&p[1]) - 12;
			return true;
		}
	return false;
}

// The path point a click on the room at x, y walks to, as GETPATHPOSN finds it (room coordinates).
void AGOSGuide::nearestPathPoint(int x, int y, int &path, int &point) const {
	uint best = 0xFFFFFFFF;
	path = point = 0;
	for (int i = 1; i <= maxPaths(); i++) {
		const uint16 *p = (const uint16 *)_vm->_pathFindArray[i - 1];
		if (!p)
			continue;
		for (int j = 0; _vm->readUint16Wrapper(&p[0]) != pathEnd(); j++, p += 2) {
			uint dx = ABS((int16)(_vm->readUint16Wrapper(&p[0]) - x));
			uint dy = ABS((int16)(_vm->readUint16Wrapper(&p[1]) - 12 - y));
			if (dx < dy) {
				dx /= 4;
				dy *= 4;
			}
			dx += dy /= 4;
			if (dx < best) {
				best = dx;
				path = i;
				point = j;
			}
		}
	}
}

// The box a click at a screen point picks (as boxController does), if any.
const HitArea *AGOSGuide::pickAt(int x, int y) const {
	const int rx = x + scrollXAt(y);
	y += scrollY();
	const HitArea *pick = nullptr;
	uint16 priority = 0;
	for (uint i = 0; i < ARRAYSIZE(_vm->_hitAreas); i++) {
		const HitArea *h = &_vm->_hitAreas[i];
		if (!(h->flags & kBFBoxInUse) || (h->flags & kBFBoxDead))
			continue;
		if (rx >= h->x && y >= h->y && rx - h->x < h->width && y - h->y < h->height && priority <= h->priority) {
			priority = h->priority;
			pick = h;
		}
	}
	return pick;
}

// A room point that walks to that path point with a click on the floor (not on someone or something standing
// there, which would walk to them instead): the path point itself if it is free, else the nearest free one.
bool AGOSGuide::floorPoint(int path, int point, int &x, int &y) const {
	int px, py;
	if (!pathPoint(path, point, px, py))
		return false;
	const int sx = scrollXAt(0), sy = scrollY();
	int best = -1;
	for (int dy = -24; dy <= 24; dy += 2)
		for (int dx = -24; dx <= 24; dx += 2) {
			const int cx = px + dx, cy = feeble() ? py + dy : CLIP(py + dy, sy, sy + roomBottom());
			if (cx - sx < 0 || cx - sx >= screenW() || cy - sy < 0 || cy - sy > roomBottom())
				continue;
			const HitArea *h = pickAt(cx - sx, cy - sy);
			if (!h || h->id != 0)
				continue;   // not the floor
			int p2, j2;
			nearestPathPoint(cx, cy, p2, j2);
			if (p2 != path || j2 != point)
				continue;
			const int d = ABS(dx) + ABS(dy);
			if (best < 0 || d < best) {
				best = d;
				x = cx;
				y = cy;
			}
		}
	if (best >= 0)
		return true;
	x = px;
	y = feeble() ? py : CLIP(py, sy, sy + roomBottom());   // The Feeble Files scrolls up and down after Feeble
	return false;
}

bool AGOSGuide::inInventory(int item) const {
	if (item <= 0 || item >= (int)_vm->_itemArraySize)
		return false;
	Item *it = _vm->derefItem(item);
	return it && _vm->derefItem(it->parent) == _vm->me();
}

// A screen point that picks this box (as boxController picks: the highest priority there), if any is in view.
bool AGOSGuide::boxOnScreen(const HitArea *ha, int &x, int &y) const {
	const bool room = feeble() || _vm->getBitFlag(79) || ha->y < 134;
	const bool oracle = feeble() && _vm->getBitFlag(99);   // the Oracle's boxes are on the screen, not the room
	const int sx = room && !oracle ? scrollXAt(0) : 0, sy = oracle ? 0 : scrollY();
	const int cx = ha->x + ha->width / 2 - sx, cy = ha->y + ha->height / 2 - sy;
	int best = -1;
	for (int py = ha->y - sy; py < ha->y - sy + ha->height; py += 2)
		for (int px = ha->x - sx; px < ha->x - sx + ha->width; px += 2) {
			if (px < 0 || px >= screenW() || py < 0 || py >= screenH())
				continue;
			if (feeble() && !oracle && _vm->_scrollXMax && (px < 48 || px >= screenW() - 48))
				continue;   // the edges of a room that scrolls scroll it
			if (!feeble() && ((room && py >= 134 && !_vm->getBitFlag(79)) || (!room && py < 134)))
				continue;
			if (pickAt(px, py) != ha)   // the box that a click there picks
				continue;
			const int d = ABS(px - cx) + ABS(py - cy);
			if (best < 0 || d < best) {
				best = d;
				x = px;
				y = py;
			}
		}
	return best >= 0;
}

bool AGOSGuide::available(const Step &st) const {
	if (feeble() && _vm->getBitFlag(99) && (st.kind == Step::kVerb || st.kind == Step::kBox))
		return true;   // the Oracle is up and hides the room: the step closes it first
	switch (st.kind) {
	case Step::kVerb:
		if (st.a && !feeble() && !findBoxId(st.a))
			return false;
		if (st.b < 0) {
			HitArea *ha = findBoxId(-st.b);
			return ha && ha->y < screenH() + scrollY();   // a box out of the picture: someone hiding, not there yet
		}
		return findItemBox(st.b) || inInventory(st.b);
	case Step::kBox:
	case Step::kClickTo:
		return findBoxId(st.a) != nullptr;
	case Step::kChoice:
		return (int)choices().size() >= st.a;
	case Step::kChoiceText:
		return choiceWith(st.text) > 0;
	case Step::kPathPoint: {
		int x, y;
		return pathPoint(st.a, st.b, x, y);
	}
	default:
		return true;
	}
}

// Where a move points, in screen coordinates (320x200). An item out of view: the inventory's arrow, or a
// point toward it (the pointer at the edge of a scrolling map, Simon walking that way in a room).
AGOSGuide::Aim AGOSGuide::aim(const Move &m, int &x, int &y) const {
	HitArea *ha = nullptr;
	switch (m.target) {
	case Move::kBox:
		ha = findBoxId(m.id);
		break;
	case Move::kItem:
		ha = findItemBox(m.id);
		if (!ha && inInventory(m.id) && feeble() && !_vm->getBitFlag(99))
			return kOracle;   // the inventory is in the Oracle
		if (!ha && inInventory(m.id)) {
			// in the inventory but not in its window
			HitArea *arrow = findBoxId(0x7FFC);
			if (!arrow)
				arrow = findBoxId(0x7FFB);
			return arrow ? kArrow : kGone;
		}
		break;
	case Move::kIdle:
		return kGone;
	case Move::kStay:
		x = _guideMouse.x;
		y = _guideMouse.y;
		return kHere;
	case Move::kChoice: {
		Common::Array<HitArea *> list = choices();
		if (m.id < 1 || m.id > (int)list.size())
			return kGone;
		return boxOnScreen(list[m.id - 1], x, y) ? kHere : kGone;
	}
	default: {
		if (feeble() && _vm->getBitFlag(99) && m.target == Move::kPoint && m.id != -1)
			return kLeaveOracle;   // a place in the room, under the Oracle
		x = m.x - scrollXAt(m.y);
		y = m.y - scrollY();
		const bool inY = y >= 0 && y < screenH();
		if (x >= 0 && x < screenW() && inY)
			return kHere;
		if (inY && ((x >= screenW() && _vm->_scrollX >= _vm->_scrollXMax) || (x < 0 && _vm->_scrollX <= 0))) {
			x = CLIP(x, 0, screenW() - 1);   // a path point past the edge: the edge picks it
			return kHere;
		}
		return toward(m.x, m.y, x, y);
	}
	}
	if (feeble() && (_vm->_scrollCount || _vm->_scrollFlag)) {
		// the room scrolls (after Feeble): the box isn't where it will be
		x = _guideMouse.x;
		y = _guideMouse.y;
		return kScrolling;
	}
	const bool oracle = feeble() && _vm->getBitFlag(99);
	if (!ha)
		return oracle ? kLeaveOracle : kGone;
	if (boxOnScreen(ha, x, y))
		return kHere;
	if (oracle && m.target == Move::kItem && inInventory(m.id) && (findBoxId(0x7FFC) || findBoxId(0x7FFB)))
		return kArrow;   // an item in the Oracle's list, behind its frame: the list on
	if (oracle)
		return kLeaveOracle;   // something in the room, under the Oracle
	if (!feeble() && ha->y >= 134 && !_vm->getBitFlag(79))
		return kGone;
	return toward(ha->x + ha->width / 2, ha->y + ha->height / 2, x, y);
}

// Toward a room point out of view: the pointer at the edge of a scrolling map, or Simon walking that way.
AGOSGuide::Aim AGOSGuide::toward(int rx, int ry, int &x, int &y) const {
	const bool right = rx - scrollXAt(0) > screenW() / 2;
	const bool across = rx - scrollXAt(0) < 0 || rx - scrollXAt(0) >= screenW();   // not just above or below
	if (across && ((right && _vm->_scrollX >= _vm->_scrollXMax) || (!right && _vm->_scrollX <= 0)))
		return kGone;
	if (_vm->getBitFlag(79)) {
		// the map scrolls with the pointer at its edge, once it came from inside
		x = !_vm->_vgaVar9 ? 160 : (right ? 318 : 1);
		y = 100;
		return kScrolling;
	}
	// the point of the room's paths in view nearest to it, clicked on the floor there: Simon walks that way and
	// the room scrolls after him
	const int sx = scrollXAt(0), sy = scrollY();
	int best = -1;
	for (int path = 1; path <= maxPaths(); path++) {
		int px, py;
		for (int j = 0; pathPoint(path, j, px, py); j++) {
			if (px - sx < 1 || px - sx > screenW() - 2 || py - sy < 1 || py - sy > roomBottom() - 1)
				continue;
			const int d = ABS(px - rx) + ABS(py - ry);
			int fx, fy;
			if ((best < 0 || d < best) && floorPoint(path, j, fx, fy)) {
				best = d;
				x = fx - sx;
				y = fy - scrollY();
			}
		}
	}
	if (best >= 0)
		return kWalk;
	x = right ? screenW() - 4 : 4;
	y = CLIP<int>(ry - scrollY(), 60, roomBottom() - 3);
	return kWalk;
}

void AGOSGuide::pushClick(Common::Array<Move> &list, uint at, Move::Target target, int id, int x, int y, int gate) {
	Move m;
	m.target = target;
	m.id = id;
	m.x = x;
	m.y = y;
	m.gate = gate;
	m.click = false;
	m.frames = 3;
	list.insert_at(at++, m);
	m.gate = 0;
	m.click = true;
	m.frames = 1;
	list.insert_at(at++, m);
	m.target = Move::kStay;
	m.click = false;
	m.frames = 4;   // the input loop looks at a click once every 100 ms
	list.insert_at(at++, m);
}

void AGOSGuide::execute(const Step &st) {
	_moves.clear();
	switch (st.kind) {
	case Step::kVerb:
		if (st.a && feeble()) {
			// The Feeble Files: the verb is the pointer's, which a right click turns to the next one (for something
			// in the inventory, once the Oracle is open)
			if (!(st.b > 0 && inInventory(st.b) && !findItemBox(st.b))) {
				Move m;
				m.target = Move::kCursor;
				m.id = st.a;
				_moves.push_back(m);
			}
		} else if (st.a) {   // none: walk, the verb the game starts with
			pushClick(_moves, _moves.size(), Move::kBox, st.a, 0, 0, 0);
			// an item out of view: walk toward it before the verb (a walk with the verb on would use it); for
			// "use with" from the inventory, toward the second one
			const int ahead = (st.c && st.b > 0 && inInventory(st.b)) ? st.c : st.b;
			_moves[_moves.size() - 3].aheadTarget = ahead < 0 ? Move::kBox : Move::kItem;
			_moves[_moves.size() - 3].aheadId = ABS(ahead);
		}
		pushClick(_moves, _moves.size(), st.b < 0 ? Move::kBox : Move::kItem, ABS(st.b), 0, 0, 0);
		if (st.c)   // "use with", "give to": the game asks for it after the first
			pushClick(_moves, _moves.size(), st.c < 0 ? Move::kBox : Move::kItem, ABS(st.c), 0, 0, 2);
		break;
	case Step::kBox:
		pushClick(_moves, 0, Move::kBox, st.a, 0, 0, 0);
		break;
	case Step::kClickTo:
		// one click, then the game's turn; the settling clicks again until the variable has the value
		if (_vm->_variableArray[st.b] != st.c) {
			pushClick(_moves, 0, Move::kBox, st.a, 0, 0, 0);
			Move w;
			w.target = Move::kStay;
			w.frames = 10;
			_moves.push_back(w);
			w.target = Move::kIdle;
			_moves.push_back(w);
		}
		break;
	case Step::kChoice:
		pushClick(_moves, 0, Move::kChoice, st.a, 0, 0, 0);
		break;
	case Step::kChoiceText:
		pushClick(_moves, 0, Move::kChoice, choiceWith(st.text), 0, 0, 0);
		break;
	case Step::kPoint:
		pushClick(_moves, 0, Move::kPoint, 0, st.a, st.b, 0);
		break;
	case Step::kPathPoint: {
		int x = 0, y = 0;
		floorPoint(st.a, st.b, x, y);
		pushClick(_moves, 0, Move::kPoint, 0, x, y, 0);
		break;
	}
	case Step::kPairs: {
		// each card, then the one with the same face; after each click the game turns it over
		Common::Array<bool> done(st.b, false);
		for (int i = 0; i < st.b; i++) {
			if (done[i])
				continue;
			for (int j = i + 1; j < st.b; j++) {
				if (done[j] || _vm->_variableArray[st.a + j] != _vm->_variableArray[st.a + i])
					continue;
				done[i] = done[j] = true;
				const int pair[2] = {i, j};
				for (int k = 0; k < 2; k++) {
					pushClick(_moves, _moves.size(), Move::kBox, st.c + pair[k], 0, 0, 0);
					Move w;
					w.target = Move::kStay;
					w.frames = 10;
					_moves.push_back(w);
					w.target = Move::kIdle;
					_moves.push_back(w);
				}
				break;
			}
		}
		break;
	}
	case Step::kHold: {
		Move m;
		m.target = Move::kHold;
		m.id = st.a;
		m.x = st.b;
		m.y = st.c;
		m.gate = _vm->_variableArray[st.b] > st.c ? -1 : 1;   // the way the variable goes
		_moves.push_back(m);
		Move w;
		w.target = Move::kStay;
		w.frames = 10;
		_moves.push_back(w);
		break;
	}
	case Step::kLights: {
		// Gaussian elimination over GF(2): which cells to click so that every cell ends as the target (all
		// ones, or else all zeros)
		const int n = st.b * st.b;
		for (int target = 1; target >= 0; target--) {
			Common::Array<uint64> rows(n);
			for (int i = 0; i < n; i++) {
				uint64 row = 0;
				for (int j = 0; j < n; j++) {
					const int ri = i / st.b, ci = i % st.b, rj = j / st.b, cj = j % st.b;
					if (ABS(ri - rj) + ABS(ci - cj) <= 1)
						row |= (uint64)1 << j;
				}
				if ((_vm->_variableArray[st.a + i] != 0) != (target != 0))
					row |= (uint64)1 << n;
				rows[i] = row;
			}
			Common::Array<int> pivots;
			int r = 0;
			for (int col = 0; col < n && r < n; col++) {
				int p = r;
				while (p < n && !(rows[p] >> col & 1))
					p++;
				if (p == n)
					continue;
				SWAP(rows[r], rows[p]);
				for (int k = 0; k < n; k++)
					if (k != r && (rows[k] >> col & 1))
						rows[k] ^= rows[r];
				pivots.push_back(col);
				r++;
			}
			bool solvable = true;
			for (int k = r; k < n; k++)
				if (rows[k] >> n & 1)
					solvable = false;
			if (!solvable)
				continue;
			for (int k = 0; k < r; k++) {
				if (!(rows[k] >> n & 1))
					continue;
				pushClick(_moves, _moves.size(), Move::kBox, st.c + pivots[k], 0, 0, 0);
				Move w;
				w.target = Move::kStay;
				w.frames = 10;
				_moves.push_back(w);
				w.target = Move::kIdle;
				_moves.push_back(w);
			}
			break;
		}
		break;
	}
	case Step::kCloseUp: {
		Move m;
		m.target = Move::kLeave;
		_moves.push_back(m);
		break;
	}
	case Step::kSwitch: {
		// the button right of the Oracle's (the scripts check the position)
		if (_vm->_variableArray[99] == st.a)
			break;
		Move m;
		m.target = Move::kPoint;
		m.x = 85 + scrollXAt(38);
		m.y = 38 + scrollY();
		m.frames = 3;
		_moves.push_back(m);
		m.click = true;
		m.frames = 1;
		_moves.push_back(m);
		m.click = false;
		m.frames = 3;
		_moves.push_back(m);
		break;
	}
	default:
		break;
	}
}

Common::String AGOSGuide::describe(const Step &st) const {
	static const char *const names[] = {"verb", "box", "choice", "point", "room", "var", "bit", "wait", "choice", "path", "talk", "switch", "var2", "leave", "loop", "pairs", "lights", "hold", "close-up", "click to"};
	Common::String s = Common::String::format("%s %d %d %d", names[st.kind], st.a, st.b, st.c);
	if (st.kind == Step::kChoiceText)
		s = Common::String::format("choice '%s' (%d)", st.text.c_str(), choiceWith(st.text));
	else if (st.kind == Step::kTalkWait)
		s = Common::String::format("talk '%s'", st.text.c_str());
	if (!st.comment.empty())
		s += " ; " + st.comment;
	return s;
}

bool AGOSGuide::frame(Common::Point &mouse, bool &click) {
	if (_moves.empty())
		return false;
	_lastFrame = g_system->getMillis();
	click = false;
	Move &m = _moves[0];
	if (m.gate == 2 && _vm->_guideWaiting != 2 && ++m.waited < 100) {
		mouse = _guideMouse;   // until the game asks for the second item (5 s at most)
		return true;
	}
	if (m.target == Move::kOpen) {
		// open the Oracle: a click on its button at the top left (the scripts check the position), again now and
		// then until it is up
		_guideMouse = Common::Point(42, 44);
		mouse = _guideMouse;
		if (_vm->getBitFlag(99) && _vm->_guideWaiting == 1 && !_vm->_mouseHideCount) {   // even while it greets
			_moves.remove_at(0);
		} else if (++m.waited > 600) {
			logf("the Oracle never opened");
			_moves.remove_at(0);
		} else if (!_vm->getBitFlag(99) && m.waited % 60 == 5) {
			click = true;
			logf("click 42,44: the Oracle");
		}
		return true;
	}
	if (m.target == Move::kHold) {
		// the button down on the box until the variable gets there, then let go
		Move box;
		box.target = Move::kBox;
		box.id = m.id;
		int x = 0, y = 0;
		if (aim(box, x, y) == kHere)
			_guideMouse = Common::Point(x, y);
		mouse = _guideMouse;
		const int v = _vm->_variableArray[m.x];
		if ((m.gate < 0 ? v <= m.y : v >= m.y) || ++m.waited > 1500) {
			_hold = 0;
			logf("let go at %d", v);
			_moves.remove_at(0);
		} else {
			_hold = 1;
		}
		return true;
	}
	if (m.target == Move::kLeave) {
		// out of the Oracle or a close-up (88): the pointer out of its frame
		if (feeble() && (_vm->getBitFlag(99) || _vm->getBitFlag(88)) && ++m.waited < 200)
			_guideMouse = Common::Point(screenW() - 1, screenH() / 2);
		else
			_moves.remove_at(0);
		mouse = _guideMouse;
		if (_moves.empty() && _state == kRunning) {
			_state = kSettle;
			_stateSince = g_system->getMillis();
		}
		return true;
	}
	if (m.target == Move::kCursor) {
		// a right click every few frames until the pointer has that verb
		if (feeble() && _vm->getBitFlag(99))
			_guideMouse = Common::Point(320, 160);   // the Oracle's text screen: on nothing (not its arrows)
		else if (feeble() && _guideMouse.x < 112 && _guideMouse.y < 64)
			_guideMouse = Common::Point(screenW() / 2, screenH() / 2);   // not on the Oracle's or the switch's button
		mouse = _guideMouse;
		if (feeble() && _vm->_mouseCursor != m.id && conversationAhead()) {
			// a retry of the step before started the conversation this step should start, while it waited
			logf("step %d: its conversation is already up, skipped", _current + 1);
			_moves.clear();
			setCurrent(_current + 1);
			_state = kWaitIdle;
			return true;
		}
		if (_vm->_mouseCursor == m.id || ++m.waited > 60) {
			if (_vm->_mouseCursor != m.id)
				logf("step %d: the pointer never turned to verb %d (it is %d)", _current + 1, m.id, _vm->_mouseCursor);
			_moves.remove_at(0);
		} else if (m.waited % 5 == 1) {
			_vm->_rightButtonDown = true;
		}
		return true;
	}
	int x = _guideMouse.x, y = _guideMouse.y;
	if (m.aheadId) {
		Move ahead;
		ahead.target = m.aheadTarget;
		ahead.id = m.aheadId;
		if (aim(ahead, x, y) == kWalk && ++m.walks <= 6) {
			pushClick(_moves, 0, Move::kPoint, 0, x + scrollXAt(y), y + scrollY(), 0);
			Move w;
			w.target = Move::kIdle;
			_moves.insert_at(3, w);
			mouse = _guideMouse;
			return true;
		}
		m.aheadId = 0;
		x = _guideMouse.x;
		y = _guideMouse.y;
	}
	switch (aim(m, x, y)) {
	case kHere:
		_guideMouse = Common::Point(x, y);
		click = m.click;
		if (click) {
			// what follows answers this click (talk, choices, what changed)
			_clickGen = _choiceGen;
			if (feeble()) {
				_effect = false;
				_stepInv = inventoryCount();
				_stepBoxes = boxSignature();
				_stepFlags = flagSignature();
			}
		}
		if (click && _logClicks) {
			const HitArea *h = pickAt(x, y);
			logf("click %d,%d on box %d item %d", x, y, h ? h->id : -1, h ? itemId(h->itemPtr) : 0);
		}
		if (--m.frames <= 0)
			_moves.remove_at(0);
		break;
	case kScrolling:
		_guideMouse = Common::Point(x, y);
		if (++m.waited > 600)
			_moves.remove_at(0);
		break;
	case kArrow:
		if (++m.arrows > (feeble() ? 60 : 12)) {   // the Oracle's list moves a few pixels a click
			_moves.remove_at(0);
		} else {
			HitArea *arrow = findBoxId(0x7FFC);
			if (feeble() && m.target == Move::kItem) {
				// The Oracle's list: up if the item comes before the first one in view, else down (its rows take
				// the boxes from 201 on in turn as it moves, so the first in view is the one first in the list)
				Item *me = _vm->me();
				int at = 0, wanted = -1, shown = -1;
				for (Item *it = me ? _vm->derefItem(me->child) : nullptr; it; it = _vm->derefItem(it->next), at++) {
					if (itemId(it) == m.id)
						wanted = at;
					for (int id = 201; shown < 0 && id < 300; id++) {
						HitArea *ha = findBoxId(id);
						if (ha && ha->itemPtr == it)
							shown = at;
					}
				}
				if (wanted >= 0 && shown >= 0 && wanted < shown)
					arrow = nullptr;
			}
			pushClick(_moves, 0, Move::kBox, arrow ? 0x7FFC : 0x7FFB, 0, 0, 0);
		}
		break;
	case kOracle:
		// open the Oracle (its button at the top left, which the scripts check by position) and wait for it
		if (++m.arrows > 3) {
			_moves.remove_at(0);
		} else {
			Move o;
			o.target = Move::kOpen;
			_moves.insert_at(0, o);
			// the Oracle has a pointer of its own: the step's verb again
			const Step &st = _steps[_current];
			if (st.kind == Step::kVerb && st.a) {
				Move v;
				v.target = Move::kCursor;
				v.id = st.a;
				_moves.insert_at(1, v);
			}
		}
		break;
	case kLeaveOracle:
		// the pointer out of the Oracle's frame closes it
		_guideMouse = Common::Point(screenW() - 1, screenH() / 2);
		if (++m.waited > 200)
			_moves.remove_at(0);
		break;
	case kWalk:
		if (++m.walks > 6) {
			_moves.remove_at(0);
		} else {
			// walk there, wait until Simon stops, then the move again
			pushClick(_moves, 0, Move::kPoint, 0, x + scrollXAt(y), y + scrollY(), 0);
			Move w;
			w.target = Move::kIdle;
			w.frames = 0;
			_moves.insert_at(3, w);
		}
		break;
	default:
		if (m.target == Move::kIdle) {
			if ((isIdle() && (!m.id || _vm->getBitFlag(m.id))) || ++m.waited > 400)
				_moves.remove_at(0);
			break;
		}
		if ((m.target == Move::kBox || m.target == Move::kItem) && ++m.waited < 300) {
			// someone who comes and goes (behind the crates): wait for them, 13 s at most
			break;
		}
		logf("step %d: its target %d is not on the screen (room %d); stopping", _current + 1, m.id, room());
		_moves.clear();
		_state = kStopped;
		break;
	}
	mouse = _guideMouse;
	if (_moves.empty() && _state == kRunning) {
		_state = kSettle;
		_stateSince = g_system->getMillis();
	}
	return true;
}

void AGOSGuide::tick() {
	const uint32 now = g_system->getMillis();
	pollControl();
	if (room() != _seenRoom) {
		_seenRoom = room();
		_roomSince = now;
	}

	// The Feeble Files looks at the pointer only when it draws a frame, and a still room draws none: one now
	// and then while the guide moves the pointer
	if (feeble() && !_moves.empty() && now - _lastFrame > 60)
		_vm->_displayFlag = 1;

	// the pointer went off right after a click (the game reads keys now): what was left of the click
	if (!_moves.empty() && _moves[0].target == Move::kStay && _vm->_mouseHideCount) {
		while (!_moves.empty() && _moves[0].target == Move::kStay)
			_moves.remove_at(0);
		if (_moves.empty() && _state == kRunning) {
			_state = kSettle;
			_stateSince = now;
		}
	}
	if (_pendingLoad >= 0) {
		const int slot = _pendingLoad;
		_pendingLoad = -1;
		restore(slot);
	}
	if (_pendingSave >= 0 && _vm->_guideWaiting == 1) {
		const int slot = _pendingSave;
		_pendingSave = -1;
		save(slot, Common::String::format("guide %d", slot));
	}

	// how long the game has been taking clicks, without a break
	if (_vm->_guideWaiting == 1 && !_vm->_mouseHideCount) {
		if (!_clickableSince)
			_clickableSince = now;
	} else {
		_clickableSince = 0;
	}
	if (_state == kRunning && !_moves.empty() && _clickableSince && now - _clickableSince > 15000 &&
		now - _lastFrame > 15000) {
		// the clicks stopped going out while the game waits for one: the step again
		logf("step %d: the clicks stalled, again", _current + 1);
		_moves.clear();
		_state = kWaitIdle;
		_idleSince = now;
		return;
	}
	if (_state == kStopped || !_moves.empty())
		return;

	const bool idle = isIdle();
	if (idle) {
		if (!_idleSince)
			_idleSince = now;
	} else {
		_idleSince = 0;
		_busy = true;
		if (feeble() && _vm->_mouseHideCount && _state == kSettle)
			_effect = true;   // a script took over (walking alone leaves the pointer on)
	}

	if (_state == kSettle) {
		const Step &cur = _steps[_current];
		const bool waited = cur.kind == Step::kWait || cur.kind == Step::kRoomWait || cur.kind == Step::kVarWait ||
			cur.kind == Step::kBitWait || cur.kind == Step::kTalkWait || cur.kind == Step::kVar2Wait ||
			cur.kind == Step::kRoomLeave || cur.kind == Step::kLoop || cur.kind == Step::kClickTo ||
			(cur.kind == Step::kBox && cur.now);   // B: the next one may come right away
		if (!waited && now - _stateSince < kSettle)
			return;
		// done once the game reacted and waits again, or when nothing happened for a while
		const bool reacted = waited || _busy || now - _stateSince > 3000;
		const bool ready = waited || (_idleSince && now - _idleSince >= kIdleSettle);
		if (!ready || !reacted)
			return;
		if ((cur.exit || (cur.kind == Step::kBox && _stepMap)) && room() == _stepRoom) {
			// an exit (or a place on the map) that left Simon in the same room: again, then give up
			if (++_retries <= (feeble() ? 8 : 3)) {   // The Feeble Files: long walks stop now and then
				logf("step %d: still in room %d, again", _current + 1, room());
				_busy = false;
				_state = kRunning;
				_stateSince = now;
				execute(cur);
				return;
			}
			logf("step %d: Simon never left room %d; stopping", _current + 1, room());
			_state = kStopped;
			return;
		}
		if (cur.kind == Step::kClickTo && _vm->_variableArray[cur.b] != cur.c) {
			if (++_retries <= 30) {
				_state = kRunning;
				_stateSince = now;
				execute(cur);
				if (_moves.empty())
					_state = kSettle;
				return;
			}
			logf("step %d: variable %d never got to %d (it is %d)", _current + 1, cur.b, cur.c, _vm->_variableArray[cur.b]);
		}
		if (cur.kind == Step::kSwitch && _vm->_variableArray[99] != cur.a && ++_retries <= 5) {
			logf("step %d: the other one is not playing yet, again", _current + 1);
			_busy = false;
			_state = kRunning;
			_stateSince = now;
			execute(cur);
			return;
		}
		if (feeble() && cur.kind == Step::kVerb && !_effect && room() == _stepRoom && inventoryCount() == _stepInv &&
				boxSignature() == _stepBoxes && flagSignature() == _stepFlags &&
				++_retries <= 1) {
			// The Feeble Files sometimes drops the first click after a room comes up: nothing happened, again
			logf("step %d: nothing happened, again", _current + 1);
			_busy = false;
			_state = kRunning;
			_stateSince = now;
			execute(cur);
			return;
		}
		logf("step %d done (room %d)", _current + 1, room());
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
		else if (st.kind == Step::kRoomWait) {
			done = room() == st.a;
			if (!done && st.b > 0 && st.c < 0 && now - _stateSince >= (uint32)st.b * 1000 && isIdle()) {
				// not there in time: some steps back, to try them again
				logf("step %d: not in room %d after %d s, back to step %d", _current + 1, st.a, st.b, _current + 1 + st.c);
				setCurrent(_current + st.c);
				_state = kWaitIdle;
				return;
			}
		}
		else if (st.kind == Step::kVarWait) {
			done = _vm->_variableArray[st.a] == st.b;
			if (!done && st.check && isIdle()) {
				logf("step %d: variable %d is %d, not %d: back to step %d", _current + 1, st.a,
					_vm->_variableArray[st.a], st.b, _current + 1 + st.c);
				setCurrent(_current + st.c);
				_state = kWaitIdle;
				return;
			}
		}
		else if (st.kind == Step::kBitWait) {
			done = bit(st.a) == (st.b != 0);
			if (!done && st.check && isIdle()) {
				logf("step %d: bit %d is not %d: back to step %d", _current + 1, st.a, st.b, _current + 1 + st.c);
				setCurrent(_current + st.c);
				_state = kWaitIdle;
				return;
			}
		}
		else if (st.kind == Step::kTalkWait)
			done = _heard.contains(st.text);
		else if (st.kind == Step::kRoomLeave)
			done = room() != (st.a ? st.a : _prevStepRoom);
		else if (st.kind == Step::kLoop) {
			Step &loop = _steps[_current];
			if (loop.loops < st.b) {
				loop.loops++;
				logf("step %d: back %d steps (%d of %d)", _current + 1, st.a, loop.loops, st.b);
				setCurrent(_current - st.a);
				_state = kWaitIdle;
				return;
			}
			loop.loops = 0;
			done = true;
		} else if (st.kind == Step::kVar2Wait && _vm->_variableArray2)
			done = _vm->_variableArray2[st.a] == st.b || (st.c && _vm->_variableArray2[st.a] == st.c);
		if (done) {
			_state = kSettle;
			_stateSince = now;
		}
		return;
	}

	// kWaitIdle: the next step, once the game waits for the player
	if (_until > 0 && _current + 1 == _until) {
		logf("stopped at step %d", _until);
		_until = 0;
		_state = kStopped;
		return;
	}
	if (_current >= (int)_steps.size()) {
		logf("walkthrough finished");
		_state = kStopped;
		return;
	}
	if (!_steps[_current].raw.empty()) {
		// read the variables it names now
		Step fresh;
		fresh.section = _steps[_current].section;
		if (parseLine(_steps[_current].raw + (_steps[_current].comment.empty() ? "" : " ; " + _steps[_current].comment), fresh)) {
			fresh.onlyRoom = _steps[_current].onlyRoom;
			_steps[_current] = fresh;
		}
	}
	const Step &st = _steps[_current];
	const bool waitStep = st.kind == Step::kWait || st.kind == Step::kRoomWait || st.kind == Step::kVarWait ||
		st.kind == Step::kBitWait || st.kind == Step::kTalkWait || st.kind == Step::kVar2Wait ||
		st.kind == Step::kRoomLeave || st.kind == Step::kLoop;
	if (st.now) {
		if (!(_vm->_guideWaiting == 1 || (st.kind == Step::kBox && _vm->_guideWaiting == 2)) || _vm->_mouseHideCount)
			return;   // as soon as the game takes clicks, even while someone talks
	} else if (!waitStep && (!_idleSince || now - _idleSince < kIdleSettle)) {
		return;
	}
	if (!waitStep && feeble() && now - _roomSince < kRoomSettle)
		return;   // it drops the first click when it comes too soon after a room came up
	if (!st.section.empty() && _sectionSaved != _current && (waitStep ? _vm->_guideWaiting == 1 : true)) {
		_sectionSaved = _current;
		int num = 0;
		for (int i = 0; i <= _current; i++)
			if (!_steps[i].section.empty())
				num++;
		const int slot = _saveSections ? _saveSections + num - 1 : 0;
		if (_saveSections && _vm->getBitFlag(79))
			logf("section %d: on the map, which can't be saved", num);   // a section should start in a room
		else if (_saveSections && _vm->_guideWaiting != 1)
			logf("section %d: in a conversation, which can't be saved", num);
		else if (_saveSections)
			save(slot, st.section);
		logf("section %d at %u ms, step %d, slot %d: %s", num, now, _current + 1, slot, st.section.c_str());
	}
	if (st.onlyRoom && room() != st.onlyRoom) {
		logf("step %d: not in room %d, skipped", _current + 1, st.onlyRoom);
		setCurrent(_current + 1);
		if (_remaining > 0 && --_remaining == 0)
			_state = kStopped;
		return;
	}
	if (feeble() && conversationAhead() && _idleSince && now - _idleSince > 1500) {
		// a retry of the step before already started the conversation this step should start
		logf("step %d: its conversation is already up, skipped", _current + 1);
		setCurrent(_current + 1);
		if (_remaining > 0 && --_remaining == 0)
			_state = kStopped;
		return;
	}
	if (st.optional && choices().empty() && _vm->_guideWaiting == 1 && _idleSince && now - _idleSince > 3000) {
		logf("step %d: no conversation, skipped", _current + 1);
		setCurrent(_current + 1);
		if (_remaining > 0 && --_remaining == 0)
			_state = kStopped;
		return;
	}
	if (!available(st)) {
		if (!_missingSince)
			_missingSince = now;
		if (st.optional && !choices().empty() && now - _missingSince > 1500) {
			logf("step %d: no such line, skipped", _current + 1);
			_missingSince = 0;
			setCurrent(_current + 1);
			if (_remaining > 0 && --_remaining == 0)
				_state = kStopped;
			return;
		}
		if (now - _missingSince > kMissing && feeble() && st.kind == Step::kBox && _current > 0 &&
				_steps[_current - 1].kind == Step::kVerb) {
			// The Feeble Files drops a click now and then: the bike (or the like) that should have brought
			// up the map, again (three times at most)
			if (_missingBackStep != _current) {
				_missingBackStep = _current;
				_missingBacks = 0;
			}
			if (++_missingBacks <= 3) {
				logf("step %d: its item is not here (room %d): back to step %d", _current + 1, room(), _current);
				_missingSince = 0;
				setCurrent(_current - 1);
				_state = kWaitIdle;
				return;
			}
		}
		if (now - _missingSince > kMissing) {
			logf("step %d: its item is not here (room %d); stopping", _current + 1, room());
			_state = kStopped;
			_missingSince = 0;
		}
		return;
	}
	_missingSince = 0;
	logf("step %d: %s", _current + 1, describe(st).c_str());
	_prevStepRoom = _stepRoom;
	_stepRoom = room();
	_stepMap = _vm->getBitFlag(79);
	_lastFrame = now;
	_retries = 0;
	_busy = false;
	_effect = false;
	if (!waitStep)
		_heard.clear();   // a line a wait step waits for may have come since the last action
	_stepInv = inventoryCount();
	_stepBoxes = boxSignature();
	_stepFlags = flagSignature();
	_state = kRunning;
	_stateSince = now;
	execute(st);
	if (_moves.empty() && !waitStep) {
		_state = kSettle;
		_stateSince = now;
	}
}

// Text the scripts print in the panel (a conversation's choices): logged line by line.
void AGOSGuide::onMessage(const char *text) {
	for (const char *p = text; *p; p++) {
		if (*p == '\n') {
			_msgLine.trim();
			if (!_msgLine.empty()) {
				logf("line: %s", _msgLine.c_str());
				// the choices come numbered: "1", its text, "2", ...
				const int num = atoi(_msgLine.c_str());
				if (num > 0 && Common::String::format("%d", num) == _msgLine) {
					if (num == 1) {
						_choiceText.clear();
						_choiceGen++;
					}
					_choiceText.resize(num);
				} else if (!_choiceText.empty()) {
					_choiceText.back() += _choiceText.back().empty() ? _msgLine : " " + _msgLine;
				}
			}
			_msgLine.clear();
		} else {
			_msgLine += *p;
		}
	}
}

// The Feeble Files' conversation choices, one at a time from the top: "1. text", "2. text"...
void AGOSGuide::onInteract(uint num, int y, const char *text) {
	logf("line: %s (at %d)", text, y);
	_effect = true;
	if (num == 1) {
		_choiceText.clear();
		_choiceGen++;
	}
	if (num >= 1 && num <= 20) {
		_choiceText.resize(num);
		const char *dot = strstr(text, ". ");
		_choiceText[num - 1] = dot ? dot + 2 : text;
	}
}

void AGOSGuide::onTalk(uint sprite, const byte *text) {
	if (_vm->getBitFlag(79))
		return;   // the map's names
	if (text && text[0] && text[0] != ' ') {
		_effect = true;
		_heard += (const char *)text;
		_heard += "\n";
		if (_heard.size() > 4000)
			_heard = _heard.substr(_heard.size() - 2000);
	}
	if (_logTalk && text && text[0])
		logf("talk %d: %s", sprite, (const char *)text);
}

void AGOSGuide::listObjects(bool all) {
	if (feeble())
		logf("pointer %d, moves %d (first %d, waited %d), last frame %u ms ago", _vm->_mouseCursor, _moves.size(),
			_moves.empty() ? -1 : (int)_moves[0].target, _moves.empty() ? 0 : _moves[0].waited, g_system->getMillis() - _lastFrame);
	logf("room %d, idle %d, waiting %d, hidden %d, voice %d, walking %d, scroll %d of %d bit79 %d, guide state %d step %d",
		room(), isIdle(), _vm->_guideWaiting, _vm->_mouseHideCount, _vm->_sound->isVoiceActive(), _vm->getBitFlag(11),
		_vm->_scrollX, _vm->_scrollXMax, _vm->getBitFlag(79), _state, _current + 1);
	for (uint i = 0; i < ARRAYSIZE(_vm->_hitAreas); i++) {
		HitArea *ha = &_vm->_hitAreas[i];
		if (!(ha->flags & kBFBoxInUse) || ((ha->flags & kBFBoxDead) && !all))
			continue;
		Common::String text;
		if (ha->flags & kBFTextBox) {
			const uint t = ha->flags / 256;
			if (t < _vm->_numTextBoxes)
				text = Common::String((const char *)_vm->getStringPtrByID(_vm->_shortText[t]));
		}
		const int item = itemId(ha->itemPtr);
		const bool inv = item > 0 && inInventory(item);
		logf("  box %d item %d%s at %d,%d %dx%d verb %d flags %x prio %d '%s'", ha->id, item, inv ? " (inv)" : "",
			ha->x, ha->y, ha->width, ha->height, ha->verb, ha->flags, ha->priority,
			(ha->flags & kBFTextBox) ? text.c_str() : itemName(ha->itemPtr).c_str());
	}
	// the inventory, all of it
	Item *me = _vm->me();
	for (Item *it = me ? _vm->derefItem(me->child) : nullptr; it; it = _vm->derefItem(it->next))
		logf("  inv %d '%s'", itemId(it), itemName(it).c_str());
}

bool AGOSGuide::save(int slot, const Common::String &desc) {
	if (_vm->_mouseHideCount || _vm->_showPreposition) {
		logf("cannot save now (slot %d)", slot);
		return false;
	}
	const bool ok = _vm->saveGame(slot, desc.c_str());
	logf("saved slot %d: %d", slot, ok);
	return ok;
}

bool AGOSGuide::restore(int slot) {
	const bool ok = _vm->loadGame(_vm->genSaveName(slot));
	if (ok && feeble()) {
		// the voices of the CD the game was on (a save made on a later CD loads with the first one's)
		const uint disc = _vm->_variableArray[97];
		if (!strcmp(_vm->getExtra(), "4CD") && disc >= 1 && disc <= 4)
			_vm->_sound->switchVoiceFile(_vm->gss, disc);
		else if (!strcmp(_vm->getExtra(), "2CD") && disc >= 1 && disc <= 4)
			_vm->_sound->switchVoiceFile(_vm->gss, disc <= 2 ? 1 : 2);
		// The Feeble Files puts a room back up when its Oracle closes (its own saves are made with it open):
		// open and close it
		Move m;
		m.target = Move::kOpen;
		_moves.push_back(m);
		m.target = Move::kLeave;
		_moves.push_back(m);
	} else if (ok) {
		_vm->drawIconArray(2, _vm->me(), 0, 0);
		_vm->setBitFlag(97, true);
		Subroutine *sub = _vm->getSubroutineByID(100);
		if (sub)
			_vm->startSubroutine(sub);
	}
	logf("loaded slot %d: %d", slot, ok);
	return ok;
}

void AGOSGuide::runCommand(const Common::String &text) {
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
	} else if (c == "until") {
		// play on, but stop as step x comes up (to load a save at a quiet moment)
		_until = x;
		logf("until step %d", x);
	} else if (c == "goto") {
		setCurrent(x > 0 ? x - 1 : 0);
		_sectionSaved = _current;   // no save for the section it starts in
		logf("current step %d", _current + 1);
	} else if (c == "reload") {
		if (ConfMan.hasKey("guide_file"))
			load(ConfMan.get("guide_file"));
		if (_current >= (int)_steps.size()) {   // a shorter file: past its end
			setCurrent(_steps.size());
			_moves.clear();
			_state = kStopped;
		}
	} else if (c == "clicks") {
		_logClicks = x != 0;
	} else if (c == "objects") {
		listObjects();
	} else if (c == "boxes") {
		listObjects(true);   // the dead ones too
	} else if (c == "var") {
		logf("var %d = %d", x, _vm->_variableArray[x]);
	} else if (c == "vars") {
		Common::String s;
		for (uint i = 0; i < _vm->_numVars; i++)
			if (_vm->_variableArray[i])
				s += Common::String::format(" %u=%d", i, _vm->_variableArray[i]);
		logf("vars%s", s.c_str());
	} else if (c == "vars2") {
		// the second set (The Feeble Files: someone else's walking, while bit 83 is on)
		Common::String s;
		for (uint i = 0; _vm->_variableArray2 && i < _vm->_numVars; i++)
			if (_vm->_variableArray2[i])
				s += Common::String::format(" %u=%d", i, _vm->_variableArray2[i]);
		logf("vars2%s", s.c_str());
	} else if (c == "setvar") {
		_vm->_variableArray[x] = y;
		logf("var %d = %d", x, y);
	} else if (c == "bit") {
		logf("bit %d = %d", x, bit(x));
	} else if (c == "setbit") {
		_vm->setBitFlag(x, y != 0);
		logf("bit %d = %d", x, y != 0);
	} else if (c == "save") {
		_pendingSave = x;
	} else if (c == "load") {
		_pendingLoad = x;
	} else if (c == "mouse") {
		// point the mouse there and click (screen coordinates)
		Move m;
		m.target = Move::kPoint;
		m.id = -1;   // as given, even on the Oracle
		m.x = x + scrollXAt(y);
		m.y = y + scrollY();
		m.click = false;
		m.frames = 3;
		_moves.push_back(m);
		m.click = true;
		m.frames = 1;
		_moves.push_back(m);
		m.click = false;
		m.frames = 3;
		_moves.push_back(m);
	} else if (c == "hover") {
		// point the mouse there, no click (screen coordinates)
		Move m;
		m.target = Move::kPoint;
		m.x = x + scrollXAt(y);
		m.y = y + scrollY();
		m.frames = 10;
		_moves.push_back(m);
	} else if (c == "point") {
		// the pointer on a box, no click (its name shows)
		Move m;
		m.target = Move::kBox;
		m.id = x;
		m.frames = 10;
		_moves.push_back(m);
	} else if (c == "roomsub") {
		// the room's own script (it sets where its exits are), dumped to the log
		Item *r = _vm->derefItem(x ? x : room());
		SubRoom *sr = r ? (SubRoom *)_vm->findChildOfType(r, kRoomType) : nullptr;
		logf("room %d subroutine %d", x ? x : room(), sr ? sr->subroutine_id : -1);
		Subroutine *sub = sr ? _vm->getSubroutineByID(sr->subroutine_id) : nullptr;
		if (sub)
			_vm->dumpSubroutine(sub);
	} else if (c == "sub") {
		// one subroutine by its number (the current room's ones are not in "dump")
		Subroutine *sub = _vm->getSubroutineByID(x);
		if (sub)
			_vm->dumpSubroutine(sub);
		else
			logf("no subroutine %d", x);
	} else if (c == "item") {
		Item *it = (x > 0 && x < (int)_vm->_itemArraySize) ? _vm->derefItem(x) : nullptr;
		if (it)
			logf("item %d '%s' state %d classes %x parent %d", x, itemName(it).c_str(), it->state, it->classFlags, it->parent);
	} else if (c == "paths") {
		// the room's walking paths, point by point (room coordinates)
		for (int path = 1; path <= maxPaths(); path++) {
			Common::String points;
			int px, py;
			for (int j = 0; pathPoint(path, j, px, py); j++)
				points += Common::String::format(" %d:%d,%d", j, px, py);
			if (!points.empty())
				logf("path %d:%s", path, points.c_str());
		}
	} else if (c == "dump") {
		// the game's scripts, to the log (x: one subroutine, 0: all)
		if (x) {
			Subroutine *sub = _vm->getSubroutineByID(x);
			if (sub)
				_vm->dumpSubroutine(sub);
		} else {
			_vm->dumpAllSubroutines();
		}
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

void AGOSGuide::pollControl() {
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

} // End of namespace AGOS
