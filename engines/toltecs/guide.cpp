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

#include "audio/mixer.h"

#include "graphics/cursorman.h"

#include "toltecs/guide.h"
#include "toltecs/screen.h"
#include "toltecs/script.h"
#include "toltecs/toltecs.h"

namespace Toltecs {

static const uint32 kSettle = 1000;      // ms a step waits before it can be done (the walk or the talk starts)
static const uint32 kIdleSettle = 800;   // ms the game must stay idle (a character turns before he answers)
static const uint32 kNoReaction = 2000;  // ms after which a step that changed nothing is done (or tried again)
static const uint32 kMissing = 30000;    // ms to wait for a step's object to show up
static const int kAimFrames = 4;         // frames pointing at an object before reading the verb line
static const uint32 kChatter = 8000;     // ms of talk with the player in control after which it is background chatter
static const uint32 kQuietSettle = 1500; // ms a step that made the game busy waits for it to be quiet (a menu in the bar)

// The verbs in the bottom bar (screen points).
static const struct { const char *name; int x, y; } kVerbs[] = {
	{"abrir", 50, 379}, {"coger", 127, 379}, {"usar", 202, 379}, {"hablar", 287, 379},
	{"cerrar", 50, 394}, {"dar", 127, 394}, {"mover", 202, 394}, {"mirar", 287, 394},
};

// The dialogue buttons A, B... at the bottom: as many as lines, 59 pixels wide every 60, centered on x 320 (five
// buttons: 171-229, 231-289...); pointing at one shows its line on the verb line.
static const int kMaxButtons = 10;
static const int kButtonY = 394;
static const int kButtonFaceY = 396;    // a row of the buttons' face
static const byte kButtonFace = 231;    // its color
static const uint32 kChoiceSettle = 700;    // ms a dialogue must wait for a line

// The inventory in the bottom bar, right of the verbs: five items 45 pixels apart (pointing at one shows its name).
static const int kInvSlots = 5;
static const int kInvX = 376, kInvStep = 48, kInvY = 375;
// The arrow left of the items scrolls them one place: its top half to the first items, its bottom half to the last.
static const int kScrollX = 343, kScrollFirstY = 378, kScrollLastY = 392;

// The text of a script string: its leading control codes (position, color, font, duration) skipped, line breaks as
// spaces, up to its end code (0xF0 and up; 0xFE is followed by the speech resource).
static Common::String scriptText(const byte *p, const byte *end) {
	while (p < end) {
		if (*p == 0x0A)
			p += 5;
		else if (*p == 0x14 || *p == 0x19)
			p += 2;
		else if (*p < 0x0A)
			p++;
		else
			break;
	}
	Common::String s;
	while (p < end && *p < 0xF0) {
		if (*p == 0x1E)
			s += ' ';
		else if (*p >= 0x20)
			s += (char)*p;
		p++;
	}
	return s;
}

// UTF-8 (the walkthrough file) to the game's code page 850, for the letters Spanish needs.
static Common::String toCp850(const Common::String &in) {
	static const struct { uint32 cp; byte c; } map[] = {
		{0xE1, 0xA0}, {0xE9, 0x82}, {0xED, 0xA1}, {0xF3, 0xA2}, {0xFA, 0xA3}, {0xF1, 0xA4}, {0xD1, 0xA5},
		{0xFC, 0x81}, {0xDC, 0x9A}, {0xBF, 0xA8}, {0xA1, 0xAD}, {0xC1, 0xB5}, {0xC9, 0x90}, {0xCD, 0xD6},
		{0xD3, 0xE0}, {0xDA, 0xE9},
	};
	Common::String out;
	for (uint i = 0; i < in.size(); i++) {
		byte c = in[i];
		if (c < 0x80) {
			out += (char)c;
			continue;
		}
		uint32 cp = 0;
		if ((c & 0xE0) == 0xC0 && i + 1 < in.size()) {
			cp = ((c & 0x1F) << 6) | (in[i + 1] & 0x3F);
			i++;
		} else if ((c & 0xF0) == 0xE0 && i + 2 < in.size()) {
			cp = ((c & 0x0F) << 12) | ((in[i + 1] & 0x3F) << 6) | (in[i + 2] & 0x3F);
			i += 2;
		}
		for (uint j = 0; j < ARRAYSIZE(map); j++)
			if (map[j].cp == cp)
				out += (char)map[j].c;
	}
	return out;
}

// A code page 850 text folded for comparing: upper case, no accents (the game's own texts are not consistent about
// them), no punctuation, single spaces.
static Common::String upper850(const Common::String &in) {
	static const byte fold[][2] = {{0xA0, 'A'}, {0xB5, 'A'}, {0x82, 'E'}, {0x90, 'E'}, {0xA1, 'I'}, {0xD6, 'I'},
		{0xA2, 'O'}, {0xE0, 'O'}, {0xA3, 'U'}, {0xE9, 'U'}, {0x81, 'U'}, {0x9A, 'U'}, {0xA4, 'N'}, {0xA5, 'N'}};
	Common::String out;
	for (uint i = 0; i < in.size(); i++) {
		byte c = in[i];
		if (c >= 'a' && c <= 'z')
			c -= 32;
		for (uint j = 0; j < ARRAYSIZE(fold); j++)
			if (c == fold[j][0])
				c = fold[j][1];
		if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
			c = ' ';
		if (c == ' ' && (out.empty() || out.lastChar() == ' '))
			continue;
		out += (char)c;
	}
	out.trim();
	return out;
}

// A walkthrough object name folded for comparing, keeping a "#n" suffix (the n-th of several with that name).
static Common::String foldName(const Common::String &utf8) {
	const size_t hash = utf8.findLastOf('#');
	if (hash == Common::String::npos)
		return upper850(toCp850(utf8));
	return upper850(toCp850(utf8.substr(0, hash))) + "#" + Common::String::format("%d", atoi(utf8.c_str() + hash + 1));
}

ToltecsGuide::ToltecsGuide(ToltecsEngine *vm) : _vm(vm), _current(0), _remaining(0), _state(kStopped), _frame(0),
	  _stateSince(0), _idleSince(0), _lastRectFrame(0), _lastWalkFrame(0), _missingSince(0), _choiceSince(0), _busy(false),
	  _spoke(false), _approaching(false), _approaches(0), _hideCursor(false), _logVerb(false), _saveSections(0),
	  _sectionSaved(-1), _aimSpot(0), _aimFrames(0), _chooseButton(0), _probeTable(-1), _probeIndex(0), _probeFrames(0), _mouseX(320),
	  _mouseY(200), _verbSlot(-1), _verbOffset(0), _controlOffset(0), _actionFrames(0) {
	if (ConfMan.hasKey("guide_file"))
		load(ConfMan.get("guide_file"));
	if (ConfMan.hasKey("guide_control"))
		_controlPath = ConfMan.get("guide_control");
	if (ConfMan.hasKey("guide_hide_cursor"))
		_hideCursor = ConfMan.getBool("guide_hide_cursor");
	if (ConfMan.hasKey("guide_save_sections"))
		_saveSections = ConfMan.getInt("guide_save_sections");
}

void ToltecsGuide::logf(const char *fmt, ...) {
	va_list va;
	va_start(va, fmt);
	Common::String s = Common::String::vformat(fmt, va);
	va_end(va);
	debug("guide: %s", s.c_str());
}

bool ToltecsGuide::parseLine(const Common::String &text, Step &st) const {
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
	st.now = line[0] >= 'A' && line[0] <= 'Z';
	const char kind = st.now ? line[0] - 'A' + 'a' : line[0];
	Common::String args = line.substr(2);
	args.trim();
	switch (kind) {
	case 's': {
		st.kind = Step::kSentence;
		size_t sp = args.findFirstOf(' ');
		if (sp == Common::String::npos)
			return false;
		st.verb = args.substr(0, sp);
		st.verb.toLowercase();
		Common::String rest = args.substr(sp + 1);
		size_t gt = rest.findFirstOf('>');
		st.first = foldName(gt == Common::String::npos ? rest : rest.substr(0, gt));
		if (gt != Common::String::npos)
			st.second = foldName(rest.substr(gt + 1));
		bool known = st.verb == "ir";
		for (uint i = 0; i < ARRAYSIZE(kVerbs); i++)
			known = known || st.verb == kVerbs[i].name;
		return known && !st.first.empty();
	}
	case 'b':
		st.kind = Step::kStepBack;
		st.first = foldName(args);
		return !st.first.empty();
	case 'k':
		st.kind = Step::kDialogue;
		st.text = upper850(toCp850(args));
		return !st.text.empty();
	case 'g':
		st.kind = Step::kPoint;
		return sscanf(args.c_str(), "%d %d", &st.a, &st.b) == 2;
	case 'c':
		st.kind = Step::kScreenClick;
		return sscanf(args.c_str(), "%d %d", &st.a, &st.b) == 2;
	case 'r':
		st.kind = Step::kScreenRight;
		return sscanf(args.c_str(), "%d %d", &st.a, &st.b) == 2;
	case 'd':
		st.kind = Step::kScreenDouble;
		return sscanf(args.c_str(), "%d %d", &st.a, &st.b) == 2;
	case 'a':
		st.kind = Step::kSceneWait;
		return sscanf(args.c_str(), "%d", &st.a) == 1;
	case 't':
		st.kind = Step::kVerbWait;
		st.text = upper850(toCp850(args));
		return !st.text.empty();
	case 'w':
		st.kind = Step::kWait;
		return sscanf(args.c_str(), "%d", &st.a) == 1;
	default:
		return false;
	}
}

bool ToltecsGuide::load(const Common::String &path) {
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

void ToltecsGuide::setCurrent(int step) {
	_current = CLIP<int>(step, 0, _steps.size());
	_dropTried = false;
	_retries = 0;
	_stepScene = _vm->_sceneResIndex;
	_approaches = 0;
	_approaching = false;
	_missingSince = 0;
}

// Hooks -------------------------------------------------------------------------------------------------------------

void ToltecsGuide::onRects(int kind, int16 slot, int16 offset, int16 itemSize, int16 first, int16 found) {
	_lastRectFrame = _frame;
	for (uint i = 0; i < _tables.size(); i++) {
		Table &t = _tables[i];
		if (t.kind == kind && t.slot == slot && t.offset == offset && t.itemSize == itemSize) {
			t.lastFrame = _frame;
			t.scene = _vm->_sceneResIndex;
			return;
		}
	}
	Table t;
	t.scene = _vm->_sceneResIndex;
	t.kind = kind;
	t.slot = slot;
	t.offset = offset;
	t.itemSize = itemSize;
	t.lastFrame = _frame;
	_tables.push_back(t);
}

void ToltecsGuide::onVerbLine(int16 slot, int16 offset) {
	ScriptInterpreter *s = _vm->_script;
	const byte *data = s->getSlotData(slot);
	if (!data)
		return;
	const Common::String text = scriptText(data + offset, data + s->getSlotSize(slot));
	_verbSlot = slot;
	_verbOffset = offset;
	// the whole line: its items so far (a sentence being built: "ESTACA + PICO")
	Screen *scr = _vm->_screen;
	Common::String line;
	for (int i = 0; i <= scr->_verbLineNum && i < 8; i++) {
		const byte *d = s->getSlotData(scr->_verbLineItems[i].slotIndex);
		if (d)
			line += (i ? "|" : "") + scriptText(d + scr->_verbLineItems[i].slotOffset,
				d + s->getSlotSize(scr->_verbLineItems[i].slotIndex));
	}
	_verbLine = line;
	if (text != _lastVerb) {
		_lastVerb = text;
		if (_logVerb)
			logf("verb: %s [%s] item %d", text.c_str(), line.c_str(), scr->_verbLineNum);
	}
}

void ToltecsGuide::onTalk(int16 slot, int16 offset) {
	ScriptInterpreter *s = _vm->_script;
	const byte *data = s->getSlotData(slot);
	if (!data)
		return;
	_spoke = true;
	_lastTalk = scriptText(data + offset, data + s->getSlotSize(slot));
	_lastTalkFrame = _frame;
	Common::String bare = _lastTalk;
	bare.trim();
	bool snore = true;   // a sleeper's "Z", "ZZZZZ" or a blank: not worth the log
	for (uint i = 0; i < bare.size() && snore; i++)
		snore = bare[i] == 'Z' || bare[i] == 'z';
	if (!snore)
		logf("talk %d: %s", slot, _lastTalk.c_str());
}

void ToltecsGuide::onWalk() {
	_lastWalkFrame = _frame;
}

// State -------------------------------------------------------------------------------------------------------------

bool ToltecsGuide::talking() const {
	Screen *scr = _vm->_screen;
	for (int i = 0; i <= scr->_talkTextItemNum && i < 5; i++)
		if (scr->_talkTextItems[i].duration > 0)
			return true;
	return _vm->_mixer->hasActiveChannelOfType(Audio::Mixer::kSpeechSoundType);
}

// The script's input loop runs (it looks the mouse up in its hotspot tables every frame) and the mouse is on.
bool ToltecsGuide::hasControl() const {
	return !_vm->_mouseDisabled && _frame - _lastRectFrame <= 3;
}

// Someone talks while the player has control: lines said on their own in a loop (a character muttering) once that has
// gone on for a while; the game waits for the player then too.
bool ToltecsGuide::isIdle() const {
	const bool chatter = _chatterSince && g_system->getMillis() - _chatterSince >= kChatter;
	return hasControl() && (!talking() || chatter) && _frame - _lastWalkFrame > 8;
}

// The game waits with the mouse on and nobody talking or walking: in the scene, a dialogue or a menu in the bottom bar
// (the donkey's ways, the map).
bool ToltecsGuide::quiet() const {
	return !_vm->_mouseDisabled && !talking() && _frame - _lastWalkFrame > 8;
}

// How many dialogue buttons the bottom bar shows (0: none), from where button A's face starts.
int ToltecsGuide::buttonCount() const {
	const byte *bar = _vm->_screen->_frontScreen + kButtonFaceY * 640;
	for (int x = 0; x < 330; x++) {
		int run = 0;
		while (run < 15 && bar[x + run] == kButtonFace)
			run++;
		if (run < 15)
			continue;   // not a button's face (a button is 58 wide, its letter in the middle)
		const int start = x - 1;   // its 1-pixel border
		const int n = (321 - start + 15) / 30;
		return n >= 1 && n <= kMaxButtons && ABS(321 - 30 * n - start) <= 2 ? n : 0;
	}
	return 0;
}

int ToltecsGuide::buttonX(int i, int n) const {
	return 321 - 30 * n + 29 + 60 * i;
}

// A dialogue waits for a line: its buttons are in the bottom bar, the mouse is on and nobody talks.
bool ToltecsGuide::choosing() const {
	// (someone may walk meanwhile: sfWalk moves any actor, not only Fenimore)
	// (someone may talk on and on meanwhile: a sleeper snoring)
	const bool chatter = _talkSince && g_system->getMillis() - _talkSince >= kChatter;
	return buttonCount() > 0 && !_vm->_mouseDisabled && (!talking() || chatter);
}

// The tables the script looked the mouse up in on this screen, in the order first seen (it only does while the mouse
// is above the bottom bar: they stay current until the scene changes).
Common::Array<int> ToltecsGuide::currentTables() const {
	Common::Array<int> current;
	for (uint i = 0; i < _tables.size(); i++)
		if (_tables[i].scene == (int)_vm->_sceneResIndex && _tables[i].lastFrame >= _sceneSince)
			current.push_back(i);
	return current;
}

int ToltecsGuide::tableCount(const Table &t) const {
	int n = 0;
	Rect r;
	while (rectOf(t, n, r))
		n++;
	return n;
}

bool ToltecsGuide::rectOf(const Table &t, int index, Rect &r) const {
	ScriptInterpreter *s = _vm->_script;
	const byte *data = s->getSlotData(t.slot);
	if (!data || t.itemSize < 8)
		return false;
	const uint32 pos = t.offset + index * t.itemSize;
	if (pos + 8 > s->getSlotSize(t.slot))
		return false;
	const byte *p = data + pos;
	r.y = READ_LE_UINT16(p);
	if (r.y == -10)
		return false;
	r.x = READ_LE_UINT16(p + 2);
	r.h = READ_LE_UINT16(p + 4);
	r.w = READ_LE_UINT16(p + 6);
	return true;
}

// A hotspot's name: its record ends with the rectangle, and the offset of its name (in the same slot) is 8 bytes before.
Common::String ToltecsGuide::nameOf(const Table &t, int index) const {
	ScriptInterpreter *s = _vm->_script;
	const byte *data = s->getSlotData(t.slot);
	const int32 pos = t.offset + index * t.itemSize - 8;
	if (!data || t.itemSize < 32 || pos < 0)
		return "";
	const int16 ofs = READ_LE_UINT16(data + pos);
	if (ofs <= 0 || (uint32)ofs >= s->getSlotSize(t.slot))
		return "";
	return scriptText(data + ofs, data + s->getSlotSize(t.slot));
}

// A screen point on a hotspot that the script's lookup gives that hotspot (not an earlier one of the same table that
// overlaps it), the nearest to its middle; false when no part of it is on the screen.
bool ToltecsGuide::tablePoint(const Table &t, int index, int &x, int &y) const {
	Rect r;
	if (!rectOf(t, index, r))
		return false;
	const int cx = _vm->_cameraX, cy = _vm->_cameraY;
	// away from the edges: the mouse resting there scrolls some scenes
	const int x1 = MAX<int>(r.x, cx + 24), x2 = MIN<int>(r.x + r.w, cx + 615);
	const int y1 = MAX<int>(r.y, cy + 8), y2 = MIN<int>(r.y + r.h, cy + _vm->_cameraHeight - 4);
	if (x1 > x2 || y1 > y2)
		return false;
	const int mx = (x1 + x2) / 2, my = (y1 + y2) / 2;
	// fallback: the middle, even under an earlier rectangle (the script skips a switched-off hotspot and looks on:
	// two hotspots with the same rectangle are one thing in two states, the furrows over the plot)
	x = mx - cx;
	y = my - cy;
	int best = -1;
	for (int py = y1; py <= y2; py += 2)
		for (int px = x1; px <= x2; px += 2) {
			bool earlier = false;
			for (int j = 0; j < index && !earlier; j++) {
				Rect o;
				if (rectOf(t, j, o))
					earlier = px >= o.x && px <= o.x + o.w && py >= o.y && py <= o.y + o.h;
			}
			if (earlier)
				continue;
			const int d = ABS(px - mx) + ABS(py - my);
			if (best < 0 || d < best) {
				best = d;
				x = px - cx;
				y = py - cy;
			}
		}
	return true;
}

// The hotspots with that name; "NAME#n" picks the n-th of them (in the table's order) when several share it.
Common::Array<ToltecsGuide::Spot> ToltecsGuide::spotsNamed(const Common::String &fullName) const {
	Common::Array<Spot> spots;
	Common::String name = fullName;
	int nth = 0;
	const size_t hash = fullName.findLastOf('#');
	if (hash != Common::String::npos) {
		nth = atoi(fullName.c_str() + hash + 1);
		name = fullName.substr(0, hash);
		name.trim();
	}
	const Common::Array<int> current = currentTables();
	for (uint i = 0; i < current.size(); i++) {
		const Table &t = _tables[current[i]];
		const int n = tableCount(t);
		for (int j = 0; j < n; j++)
			if (upper850(nameOf(t, j)) == name) {
				Spot s;
				s.table = current[i];
				s.index = j;
				spots.push_back(s);
			}
	}
	if (nth > 0) {
		Common::Array<Spot> one;
		if (nth <= (int)spots.size())
			one.push_back(spots[nth - 1]);
		return one;
	}
	return spots;
}

// A hotspot with that name above or below the view: -1 above, 1 below, 0 none (some scenes scroll up and down when
// the mouse rests at the edge: the handcar's map).
int ToltecsGuide::offVertically(const Common::String &name) const {
	const Common::Array<Spot> spots = spotsNamed(name);
	for (uint i = 0; i < spots.size(); i++) {
		Rect r;
		int px, py;
		const Table &t = _tables[spots[i].table];
		if (!rectOf(t, spots[i].index, r) || tablePoint(t, spots[i].index, px, py))
			continue;
		if (r.y + r.h < _vm->_cameraY)
			return -1;
		if (r.y > _vm->_cameraY + _vm->_cameraHeight)
			return 1;
	}
	return 0;
}

// A hotspot with that name that is out of view (the scene scrolls): a floor point toward it to walk to first.
bool ToltecsGuide::offScreen(const Common::String &name, int &x, int &y) const {
	const Common::Array<Spot> spots = spotsNamed(name);
	for (uint i = 0; i < spots.size(); i++) {
		Rect r;
		int px, py;
		const Table &t = _tables[spots[i].table];
		if (!rectOf(t, spots[i].index, r) || tablePoint(t, spots[i].index, px, py))
			continue;
		const int ox = r.x + r.w / 2 - _vm->_cameraX;
		x = ox < 0 ? 40 : 600;
		y = CLIP<int>(r.y + r.h - _vm->_cameraY, 200, _vm->_cameraHeight - 20);
		// floor, not a hotspot (a door there would take him through it): the nearest free point
		static const int dy[] = {0, 15, -15, 30, -30, 45, -45, 60};
		static const int dx[] = {0, 40, 80, 120};
		for (int a = 0; a < ARRAYSIZE(dx); a++)
			for (int b = 0; b < ARRAYSIZE(dy); b++) {
				const int qx = ox < 0 ? x + dx[a] : x - dx[a], qy = CLIP<int>(y + dy[b], 20, _vm->_cameraHeight - 10);
				if (!onHotspot(qx, qy)) {
					x = qx;
					y = qy;
					return true;
				}
			}
		return true;
	}
	return false;
}

// A floor point below a hotspot with that name (screen coordinates), off any hotspot: where to step back to.
bool ToltecsGuide::stepBack(const Common::String &name, int &x, int &y) const {
	const Common::Array<Spot> spots = spotsNamed(name);
	for (uint i = 0; i < spots.size(); i++) {
		Rect r;
		int px, py;
		const Table &t = _tables[spots[i].table];
		if (!rectOf(t, spots[i].index, r) || !tablePoint(t, spots[i].index, px, py))
			continue;
		static const int dy[] = {50, 70, 90, 35};
		static const int dx[] = {0, -40, 40, -80, 80};
		for (int a = 0; a < ARRAYSIZE(dy); a++)
			for (int b = 0; b < ARRAYSIZE(dx); b++) {
				const int qx = CLIP<int>(r.x + r.w / 2 - _vm->_cameraX + dx[b], 20, 620);
				const int qy = CLIP<int>(r.y + r.h - _vm->_cameraY + dy[a], 20, _vm->_cameraHeight - 10);
				if (!onHotspot(qx, qy)) {
					x = qx;
					y = qy;
					return true;
				}
			}
	}
	return false;
}

// Whether a screen point is on any hotspot of the scene's tables.
bool ToltecsGuide::onHotspot(int x, int y) const {
	const int sx = x + _vm->_cameraX, sy = y + _vm->_cameraY;
	const Common::Array<int> current = currentTables();
	for (uint i = 0; i < current.size(); i++) {
		const Table &t = _tables[current[i]];
		Rect r;
		for (int j = 0; rectOf(t, j, r); j++)
			if (sx >= r.x && sx <= r.x + r.w && sy >= r.y && sy <= r.y + r.h)
				return true;
	}
	return false;
}

// Actions ----------------------------------------------------------------------------------------------------------

void ToltecsGuide::move(int x, int y, int frames) {
	Action a;
	a.type = Action::kMove;
	a.x = CLIP(x, 0, 639);
	a.y = CLIP(y, 0, 399);
	a.left = a.right = a.dbl = false;
	a.frames = frames;
	_actions.push_back(a);
}

// Point for a few frames, press, release (the script reads a click once: the engine waits for the release).
void ToltecsGuide::click(int x, int y, bool right, bool dbl) {
	move(x, y, 6);
	for (int i = 0; i < (dbl ? 2 : 1); i++) {
		move(x, y, 2);
		_actions.back().left = !right;
		_actions.back().right = right;
		_actions.back().dbl = i > 0;
		move(x, y, 3);
	}
}

void ToltecsGuide::aim(const Common::String &name) {
	Action a;
	a.type = Action::kAim;
	a.x = a.y = 0;
	a.left = a.right = a.dbl = false;
	a.frames = 0;
	a.name = name;
	_actions.push_back(a);
}

// One frame of pointing at a named object: try its hotspots one by one until the verb line shows the name, then click
// there. False when none of them does (the step fails).
bool ToltecsGuide::runAim(Action &a) {
	if (a.frames == 0) {
		_aimSpots = a.inv ? Common::Array<Spot>() : spotsNamed(a.name);
		_aimSpot = 0;
		_aimSlot = 0;
		_aimFrames = 0;
		_scrollPhase = 0;
		_scrollMoves = 0;
		_scrollSeen.clear();
	}
	a.frames++;
	while (_aimSpot < (int)_aimSpots.size()) {
		const Spot &s = _aimSpots[_aimSpot];
		int x, y;
		if (!tablePoint(_tables[s.table], s.index, x, y)) {
			_aimSpot++;
			_aimFrames = 0;
			continue;
		}
		_vm->_mouseX = _mouseX = x;
		_vm->_mouseY = _mouseY = y;
		_vm->_leftButtonDown = _vm->_rightButtonDown = false;
		g_system->warpMouse(x, y);
		if (++_aimFrames < kAimFrames)
			return true;
		Common::String bare = a.name;
		if (bare.findLastOf('#') != Common::String::npos) {
			bare = bare.substr(0, bare.findLastOf('#'));
			bare.trim();
		}
		// the name on the verb line, or as a label over it (the map and the handcar's map show names that way)
		if (upper850(_lastVerb) == bare || (upper850(_lastTalk) == bare && _frame - _lastTalkFrame <= kAimFrames + 2)) {
			// on it: click here (after this action, before the rest)
			Common::Array<Action> rest;
			for (uint i = 1; i < _actions.size(); i++)
				rest.push_back(_actions[i]);
			_actions.resize(1);
			click(x, y, false);
			_actions.remove_at(0);
			_actions[0].frames = 1;   // already pointing
			for (uint i = 0; i < rest.size(); i++)
				_actions.push_back(rest[i]);
			return true;
		}
		_aimSpot++;
		_aimFrames = 0;
		return true;
	}
	// not in the scene: an inventory item
	while (_aimSlot < kInvSlots) {
		const int x = kInvX + kInvStep * _aimSlot, y = kInvY;
		if (_aimFrames == 0)
			_lastVerb.clear();   // an empty slot leaves it empty
		_vm->_mouseX = _mouseX = x;
		_vm->_mouseY = _mouseY = y;
		_vm->_leftButtonDown = _vm->_rightButtonDown = false;
		g_system->warpMouse(x, y);
		if (++_aimFrames < kAimFrames + 1)
			return true;
		_aimFrames = 0;
		if (upper850(_lastVerb) == a.name) {
			Common::Array<Action> rest;
			for (uint i = 1; i < _actions.size(); i++)
				rest.push_back(_actions[i]);
			_actions.resize(1);
			click(x, y, false);
			_actions.remove_at(0);
			_actions[0].frames = 1;
			for (uint i = 0; i < rest.size(); i++)
				_actions.push_back(rest[i]);
			return true;
		}
		_aimSlot++;   // (an empty one may be the item in hand, for "usar X con Y": read them all)
		if (_aimSlot == 1)
			_scrollSeen = _lastVerb;
		return true;
	}
	// the second object of a sentence, with the first in hand: the arrow scrolls the items now; to the first ones
	// reading the first slot, then to the last ones reading the fifth (quickly: some uses are timed)
	while (a.dbl && _scrollPhase < 2) {
		const int f = _aimFrames++;
		const int slot = _scrollPhase == 0 ? 0 : kInvSlots - 1;
		if (f < 5) {   // point 2 frames, press 1, release 2
			_vm->_mouseX = _mouseX = kScrollX;
			_vm->_mouseY = _mouseY = _scrollPhase == 0 ? kScrollFirstY : kScrollLastY;
			_vm->_leftButtonDown = f == 2;
			_vm->_rightButtonDown = false;
			if (f == 2)
				_vm->_mouseDblClickTicks = 0;
			g_system->warpMouse(_mouseX, _mouseY);
			return true;
		}
		if (f < 9) {
			if (f == 5)
				_lastVerb.clear();
			_vm->_mouseX = _mouseX = kInvX + kInvStep * slot;
			_vm->_mouseY = _mouseY = kInvY;
			g_system->warpMouse(_mouseX, _mouseY);
			return true;
		}
		_aimFrames = 0;
		if (upper850(_lastVerb) == a.name) {
			const int x = kInvX + kInvStep * slot;
			Common::Array<Action> rest;
			for (uint i = 1; i < _actions.size(); i++)
				rest.push_back(_actions[i]);
			_actions.resize(1);
			click(x, kInvY, false);
			_actions.remove_at(0);
			_actions[0].frames = 1;
			for (uint i = 0; i < rest.size(); i++)
				_actions.push_back(rest[i]);
			return true;
		}
		if (_lastVerb == _scrollSeen || _lastVerb.empty() || ++_scrollMoves > 30) {
			_scrollPhase++;   // it did not move: the end on this side
			_scrollSeen.clear();
			_scrollMoves = 0;
		} else {
			_scrollSeen = _lastVerb;
		}
		return true;
	}
	return false;
}

// One frame of reading the dialogue buttons: point at each for a few frames and read its line; click the first that
// contains the text (an empty text only logs the lines). False when no line has it.
bool ToltecsGuide::runChoose(Action &a) {
	if (a.frames == 0) {
		_chooseButton = 0;
		_chooseCount = buttonCount();
		_aimFrames = 0;
		_lines.clear();
	}
	a.frames++;
	if (_chooseButton < _chooseCount) {
		const int x = buttonX(_chooseButton, _chooseCount);
		if (_aimFrames == 0)
			_lastVerb.clear();   // a button with no line leaves it empty
		_vm->_mouseX = _mouseX = x;
		_vm->_mouseY = _mouseY = kButtonY;
		_vm->_leftButtonDown = _vm->_rightButtonDown = false;
		g_system->warpMouse(x, kButtonY);
		if (++_aimFrames < kAimFrames + 1)
			return true;
		_aimFrames = 0;
		_lines.push_back(_lastVerb);
		if (!a.name.empty() && upper850(_lastVerb).contains(a.name)) {
			// this one: click it (after this action, before the rest)
			Common::Array<Action> rest;
			for (uint i = 1; i < _actions.size(); i++)
				rest.push_back(_actions[i]);
			_actions.resize(1);
			click(x, kButtonY, false);
			// then back up to the scene: the script only looks the mouse up in the scene's hotspots (what tells the
			// player has control) while it is there
			move(_sceneMouseX, _sceneMouseY, 2);
			_actions.remove_at(0);
			_actions[0].frames = 1;
			for (uint i = 0; i < rest.size(); i++)
				_actions.push_back(rest[i]);
			return true;
		}
		_chooseButton++;
		return true;
	}
	for (uint i = 0; i < _lines.size(); i++)
		if (!_lines[i].empty())
			logf("  line %c: %s", 'A' + i, _lines[i].c_str());
	const bool logOnly = a.name.empty();
	_actions.remove_at(0);   // a is gone
	return logOnly;
}

// One frame of bringing an inventory item into view before a sentence (the arrow left of the items only scrolls them
// while no verb is chosen): read the items shown; if it is not there, scroll to the first ones one place at a time
// reading the first slot, then to the last ones reading the fifth. Done (true) once seen or when both ends were reached.
bool ToltecsGuide::runReveal(Action &a) {
	if (a.frames == 0) {
		_aimSlot = 0;
		_aimFrames = 0;
		_scrollPhase = 0;
		_scrollMoves = 0;
		_scrollSeen.clear();
	}
	a.frames++;
	while (_aimSlot < kInvSlots) {
		const int x = kInvX + kInvStep * _aimSlot;
		if (_aimFrames == 0)
			_lastVerb.clear();
		_vm->_mouseX = _mouseX = x;
		_vm->_mouseY = _mouseY = kInvY;
		_vm->_leftButtonDown = _vm->_rightButtonDown = false;
		g_system->warpMouse(x, kInvY);
		if (++_aimFrames < kAimFrames + 1)
			return false;
		_aimFrames = 0;
		if (upper850(_lastVerb) == a.name)
			return true;
		if (_aimSlot == 0)
			_scrollSeen = _lastVerb;   // to tell when scrolling stops moving
		_aimSlot = _lastVerb.empty() ? kInvSlots : _aimSlot + 1;
		if (_aimSlot == kInvSlots && _scrollSeen.empty())
			return true;   // no items at all
	}
	while (_scrollPhase < 2) {
		const int f = _aimFrames++;
		const int slot = _scrollPhase == 0 ? 0 : kInvSlots - 1;
		if (f < 5) {   // point 2 frames, press 1, release 2
			_vm->_mouseX = _mouseX = kScrollX;
			_vm->_mouseY = _mouseY = _scrollPhase == 0 ? kScrollFirstY : kScrollLastY;
			_vm->_leftButtonDown = f == 2;
			_vm->_rightButtonDown = false;
			if (f == 2)
				_vm->_mouseDblClickTicks = 0;
			g_system->warpMouse(_mouseX, _mouseY);
			return false;
		}
		if (f < 9) {
			if (f == 5)
				_lastVerb.clear();
			_vm->_mouseX = _mouseX = kInvX + kInvStep * slot;
			_vm->_mouseY = _mouseY = kInvY;
			g_system->warpMouse(_mouseX, _mouseY);
			return false;
		}
		_aimFrames = 0;
		if (upper850(_lastVerb) == a.name)
			return true;
		if (_lastVerb == _scrollSeen || _lastVerb.empty() || ++_scrollMoves > 20) {
			// it did not move: the end on this side
			_scrollPhase++;
			_scrollSeen.clear();
			_scrollMoves = 0;
		} else {
			_scrollSeen = _lastVerb;
		}
	}
	if (!_missingSince)
		logf("step %d: '%s' is not in the inventory", _current + 1, a.name.c_str());
	return true;
}

void ToltecsGuide::execute(const Step &st) {
	_actions.clear();
	switch (st.kind) {
	case Step::kSentence: {
		// the object out of view: walk toward it first (with no verb chosen: a click on the floor with a verb does
		// nothing)
		// with a second object the first one is an item of the inventory (it may also name a hotspot of the scene
		// that is gone: the sponge, once taken), and the one to walk to is the second
		const bool firstIsItem = !st.second.empty();
		bool visible = false;
		const Common::String &target = firstIsItem ? st.second : st.first;
		const Common::Array<Spot> spots = spotsNamed(target);
		for (uint i = 0; i < spots.size() && !visible; i++) {
			int x, y;
			visible = tablePoint(_tables[spots[i].table], spots[i].index, x, y);
		}
		int wx, wy;
		const int vert = visible ? 0 : offVertically(target);
		if (vert && _approaches < 6) {
			_approaches++;
			logf("step %d: %s is %s the view, the mouse at the edge", _current + 1, target.c_str(),
				vert < 0 ? "above" : "below");
			move(320, vert < 0 ? 1 : _vm->_cameraHeight - 2, 90);
			_approaching = true;
			break;
		}
		if (!visible && _approaches < 6 && offScreen(target, wx, wy)) {
			_approaches++;
			logf("step %d: %s is out of view, walking to %d,%d", _current + 1, target.c_str(), wx, wy);
			click(wx, wy, false);
			_approaching = true;
			break;
		}
		// the first object an inventory item (no hotspot of the scene with that name): into view first (the arrow
		// only scrolls the items while no verb is chosen, or with an item in hand: the second one is looked for then)
		if (firstIsItem || spotsNamed(st.first).empty()) {
			aim(st.first);
			_actions.back().type = Action::kReveal;
		}
		for (uint i = 0; i < ARRAYSIZE(kVerbs); i++)
			if (st.verb == kVerbs[i].name) {
				click(kVerbs[i].x, kVerbs[i].y, false);
				_verbChosen = true;
			}
		aim(st.first);
		_actions.back().inv = firstIsItem;
		if (!st.second.empty()) {
			aim(st.second);
			_actions.back().dbl = true;   // the second object: may scroll the items (the first one is in hand)
		}
		break;
	}
	case Step::kDialogue: {
		Action a;
		a.type = Action::kChoose;
		a.x = a.y = 0;
		a.left = a.right = a.dbl = false;
		a.frames = 0;
		a.name = st.text;
		_actions.push_back(a);
		break;
	}
	case Step::kPoint:
		click(st.a - _vm->_cameraX, st.b - _vm->_cameraY, false);
		break;
	case Step::kStepBack: {
		int x, y;
		if (stepBack(st.first, x, y))
			click(x, y, false);
		else
			logf("step %d: no floor below %s", _current + 1, st.first.c_str());
		break;
	}
	case Step::kScreenClick:
		click(st.a, st.b, false);
		break;
	case Step::kScreenRight:
		click(st.a, st.b, true);
		break;
	case Step::kScreenDouble:
		click(st.a, st.b, false, true);
		break;
	default:
		break;
	}
}

// A step's object is not here: walk toward it if it is out of view, or wait for it to show up, then stop.
void ToltecsGuide::fail(const char *why) {
	const uint32 now = g_system->getMillis();
	const Step &st = _steps[_current];
	_actions.clear();
	if (_verbChosen || !_dropTried) {
		// the verb chosen for it (or left on before it) may still be on, and then the names over the hotspots are only
		// those that verb applies to: a right click on the floor drops it ("Ir a" again)
		int fx = _sceneMouseX, fy = _sceneMouseY;
		for (int i = 0; i < 40 && onHotspot(fx, fy); i++) {
			fx = 40 + (i * 97) % 560;
			fy = 60 + (i * 53) % 260;
		}
		click(fx, fy, true);
		_verbChosen = false;
		_dropTried = true;
	}
	int x, y;
	if (st.kind == Step::kSentence && _approaches < 6 && ((st.second.empty() && offScreen(st.first, x, y)) ||
		(!st.second.empty() && offScreen(st.second, x, y)))) {
		_approaches++;
		logf("step %d: %s is out of view, walking to %d,%d", _current + 1, why, x, y);
		click(x, y, true);    // a right click drops the verb chosen ("Ir a" again)
		click(x, y, false);
		_approaching = true;
		_busy = false;
		_state = kRunning;
		_stateSince = now;
		return;
	}
	if (!_missingSince)
		_missingSince = now;
	if (now - _missingSince > kMissing) {
		logf("step %d: %s is not here (scene %d); stopping", _current + 1, why, _vm->_sceneResIndex);
		_state = kStopped;
		_missingSince = 0;
		return;
	}
	// try again (after the right click: as an approach, it leads back here)
	_approaching = !_actions.empty();
	_state = _actions.empty() ? kWaitIdle : kRunning;
	_stateSince = now;
	_idleSince = 0;
}

Common::String ToltecsGuide::describe(const Step &st) const {
	static const char *const names[] = {"sentence", "dialogue", "point", "click", "right", "double", "scene", "verb",
		"wait", "step back from"};
	Common::String s = names[st.kind];
	if (st.kind == Step::kSentence)
		s += " " + st.verb + " " + st.first + (st.second.empty() ? "" : " > " + st.second);
	else if (st.kind == Step::kStepBack)
		s += " " + st.first;
	else if (!st.text.empty())
		s += " " + st.text;
	else
		s += Common::String::format(" %d %d", st.a, st.b);
	if (!st.comment.empty())
		s += " ; " + st.comment;
	return s;
}

void ToltecsGuide::next() {
	_verbChosen = false;
	logf("step %d done (scene %d)", _current + 1, _vm->_sceneResIndex);
	setCurrent(_current + 1);
	if (_remaining > 0 && --_remaining == 0) {
		_state = kStopped;
		return;
	}
	_state = kWaitIdle;
}

// The frame ------------------------------------------------------------------------------------------------------------

void ToltecsGuide::tick() {
	_frame++;
	if ((int)_vm->_sceneResIndex != _lastScene) {
		_lastScene = _vm->_sceneResIndex;
		_sceneSince = _frame;   // the tables seen from now on are this scene's
	}
	const uint32 now = g_system->getMillis();
	pollControl();
	if (_hideCursor)
		CursorMan.showMouse(false);
	if (_actionFrames > 0 && --_actionFrames == 0)
		_vm->_action = kActionNone;

	if (_probeTable >= 0) {
		// "objects": hover each hotspot of each table for a few frames and log the verb line over it
		while (_probeTable < (int)_probeList.size()) {
			const Table &t = _tables[_probeList[_probeTable]];
			Rect r;
			if (!rectOf(t, _probeIndex, r)) {
				_probeTable++;
				_probeIndex = 0;
				continue;
			}
			int x, y;
			if (!tablePoint(t, _probeIndex, x, y)) {
				logf("  hotspot %d.%d '%s' scene %d,%d %dx%d off the screen", _probeTable + 1, _probeIndex,
					nameOf(t, _probeIndex).c_str(), r.x, r.y, r.w, r.h);
				_probeIndex++;
				continue;
			}
			_vm->_mouseX = x;
			_vm->_mouseY = y;
			g_system->warpMouse(x, y);
			if (++_probeFrames < 5)
				return;
			_probeFrames = 0;
			logf("  hotspot %d.%d '%s' scene %d,%d %dx%d at %d,%d: %s", _probeTable + 1, _probeIndex,
				nameOf(t, _probeIndex).c_str(), r.x, r.y, r.w, r.h, x, y,
				upper850(_lastVerb) == upper850(nameOf(t, _probeIndex)) ||
					(upper850(_lastTalk) == upper850(nameOf(t, _probeIndex)) && _frame - _lastTalkFrame <= 7) ? "on" :
					("off, shows '" + _lastVerb + "'").c_str());
			_probeIndex++;
			return;
		}
		_probeTable = -1;
		_vm->_mouseX = _mouseX;
		_vm->_mouseY = _mouseY;
		g_system->warpMouse(_mouseX, _mouseY);
		logf("objects done");
		return;
	}

	if (!_actions.empty() && _state == kRunning && (int)_vm->_sceneResIndex != _stepScene) {
		// a click of this step went through a door after all (late, after an animation): the rest was meant for the
		// scene left behind
		logf("step %d: the scene changed (%d), dropping the rest of it", _current + 1, _vm->_sceneResIndex);
		_actions.clear();
		_approaching = false;
		_retries = 2;
		_busy = true;
		_state = kSettle;
		_stateSince = now;
		return;
	}

	if (!_actions.empty()) {
		Action &a = _actions[0];
		if (a.type == Action::kAim) {
			if (!runAim(a))
				fail(Common::String::format("'%s'", a.name.c_str()).c_str());
			return;
		}
		if (a.type == Action::kReveal) {
			if (runReveal(a))
				_actions.remove_at(0);
			return;
		}
		if (a.type == Action::kChoose) {
			const Common::String name = a.name;
			if (!runChoose(a)) {
				logf("step %d: no dialogue line with '%s'; stopping", _current + 1, name.c_str());
				_actions.clear();
				_state = kStopped;
			} else if (_actions.empty() && _state == kRunning) {
				_state = kSettle;
				_stateSince = now;
			}
			return;
		}
		_vm->_mouseX = _mouseX = a.x;
		_vm->_mouseY = _mouseY = a.y;
		if (a.y < 330) {   // above the bottom bar even when it is hidden (a close-up)
			_sceneMouseX = a.x;
			_sceneMouseY = a.y;
		}
		_vm->_leftButtonDown = a.left;
		_vm->_rightButtonDown = a.right;
		if ((a.left || a.right) && !a.dbl)
			_vm->_mouseDblClickTicks = 0;   // a single click, however soon after the last one
		g_system->warpMouse(a.x, a.y);
		if (--a.frames <= 0)
			_actions.remove_at(0);
		if (_actions.empty() && _state == kRunning) {
			_state = kSettle;
			_stateSince = now;
		}
		return;
	}

	if (_state == kStopped)
		return;

	if (_vm->_mouseY >= _vm->_cameraHeight && _vm->_cameraHeight > 0 && !_vm->_mouseDisabled) {
		// the mouse in the bottom bar (after a load, an item, a dialogue line): back up to the scene, where the script
		// looks it up in the hotspots (what tells the player has control)
		move(_sceneMouseX, _sceneMouseY, 1);
		return;
	}

	if (hasControl() && talking()) {
		if (!_chatterSince)
			_chatterSince = now;
	} else {
		_chatterSince = 0;
	}
	if (talking()) {
		if (!_talkSince)
			_talkSince = now;
	} else {
		_talkSince = 0;
	}
	const bool idle = isIdle();
	if (idle) {
		if (!_idleSince)
			_idleSince = now;
	} else {
		_idleSince = 0;
		_busy = true;
	}
	if (choosing()) {
		if (!_choiceSince)
			_choiceSince = now;
	} else {
		_choiceSince = 0;
	}
	const bool choice = _choiceSince && now - _choiceSince >= kChoiceSettle;
	if (quiet()) {
		if (!_quietSince)
			_quietSince = now;
	} else {
		_quietSince = 0;
	}
	const uint32 quietFor = _quietSince ? now - _quietSince : 0;

	if (_state == kSettle) {
		if (now - _stateSince < kSettle)
			return;
		const Step &cur = _steps[_current];
		const bool waited = cur.kind == Step::kWait || cur.kind == Step::kSceneWait || cur.kind == Step::kVerbWait;
		const bool reacted = waited || _busy || now - _stateSince > kNoReaction;
		const bool ready = waited || (cur.now && hasControl()) || (_idleSince && now - _idleSince >= kIdleSettle) ||
			(_busy && choice) || (_busy && quietFor >= kQuietSettle);
		if (!ready || !reacted)
			return;
		if (_approaching) {
			_approaching = false;   // walked toward the object: now the step itself
			if ((int)_vm->_sceneResIndex != _stepScene && _retries > 0) {
				next();   // the click before the retry did go through a door after all
				return;
			}
			_state = kWaitIdle;
			return;
		}
		const bool screenClick = cur.kind == Step::kScreenClick || cur.kind == Step::kScreenDouble;
		if ((screenClick || cur.kind == Step::kSentence) && !_busy && _retries < 2 &&
			(int)_vm->_sceneResIndex == _stepScene) {
			// nothing happened: a menu in the bar that was not there yet (the donkey getting ready), a click the
			// script missed while an animation ended; once more
			_retries++;
			logf("step %d: no reaction, clicking again", _current + 1);
			_actions.clear();
			int bx, by;
			if (cur.kind == Step::kSentence && stepBack(cur.first, bx, by)) {
				// an action on a hotspot whose spot he already stands at does nothing (he walks there first, and the
				// action happens when he arrives): a few steps away first
				logf("step %d: stepping back to %d,%d first", _current + 1, bx, by);
				click(bx, by, false);
				_approaching = true;
			} else {
				execute(cur);
			}
			_state = kRunning;
			_stateSince = now;
			return;
		}
		next();
		return;
	}

	if (_state == kRunning) {
		// a wait step
		const Step &st = _steps[_current];
		bool done = false;
		if (st.kind == Step::kWait)
			done = now - _stateSince >= (uint32)st.a * 1000;
		else if (st.kind == Step::kSceneWait)
			done = (int)_vm->_sceneResIndex == st.a && idle;
		else if (st.kind == Step::kVerbWait)
			done = upper850(_lastVerb).contains(st.text);
		else
			done = true;
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
	const bool waitStep = st.kind == Step::kWait || st.kind == Step::kSceneWait || st.kind == Step::kVerbWait;
	const bool screenStep = st.kind == Step::kScreenClick || st.kind == Step::kScreenRight ||
		st.kind == Step::kScreenDouble;
	if (st.kind == Step::kDialogue) {
		if (!choice)
			return;   // a dialogue line: once the dialogue waits for one
	} else if (screenStep) {
		if (quietFor < (st.now ? 1 : kIdleSettle))
			return;   // a point of the screen (a menu in the bar too): once the game waits
	} else if (_vm->_guiHeight == 0 && !waitStep) {
		// a map with no bottom bar: the name under the mouse is a label that stays up; the game waits when the mouse is
		// on and nobody walks
		if (_vm->_mouseDisabled || _frame - _lastWalkFrame < 60)
			return;
	} else if (st.now) {
		if (!hasControl())
			return;
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
		if (_saveSections) {
			Common::String desc = st.section;
			_vm->requestSavegame(slot, desc);   // the script saves between two of its instructions
		}
		logf("section %d at %u ms, step %d, slot %d: %s", num, now, _current + 1, slot, st.section.c_str());
		return;   // the save happens before the step
	}
	if (_retries > 0 && (int)_vm->_sceneResIndex != _stepScene) {
		next();   // a retried step whose first click did go through a door after all
		return;
	}
	if (!_missingSince)
		logf("step %d: %s", _current + 1, describe(st).c_str());
	_spoke = false;
	_busy = false;
	_state = kRunning;
	_stateSince = now;
	if (!_approaches && !_retries)
		_stepScene = _vm->_sceneResIndex;
	execute(st);
	if (_actions.empty() && !waitStep) {
		_state = kSettle;   // nothing to click: done once the game is idle
		_stateSince = now;
	}
}

// Control -------------------------------------------------------------------------------------------------------------

void ToltecsGuide::listState() {
	logf("scene %d camera %d,%d gui %d mouse %d,%d disabled %d control %d idle %d choosing %d talking %d walk %u frames "
		"ago, verb '%s' (%d+%d), guide state %d step %d, buttons %d, view %d, scene mouse %d,%d", _vm->_sceneResIndex,
		_vm->_cameraX, _vm->_cameraY,
		_vm->_guiHeight, _vm->_mouseX, _vm->_mouseY, _vm->_mouseDisabled, hasControl(), isIdle(), choosing(), talking(),
		_frame - _lastWalkFrame,
		_lastVerb.c_str(), _verbSlot, _verbOffset, _state, _current + 1, buttonCount(), _vm->_cameraHeight, _sceneMouseX,
		_sceneMouseY);
}

// The bytes of a table entry before its rectangle (the record's other fields), as int16 words.
Common::String ToltecsGuide::entryBytes(const Table &t, int index) const {
	ScriptInterpreter *s = _vm->_script;
	const byte *data = s->getSlotData(t.slot);
	Common::String out;
	const int32 pos = t.offset + index * t.itemSize - (t.itemSize - 8);
	for (int i = 0; i < t.itemSize - 8 && data && pos + i >= 0; i += 2)
		out += Common::String::format("%s%d", i ? " " : "", (int16)READ_LE_UINT16(data + pos + i));
	return out;
}

void ToltecsGuide::startProbe() {
	listState();
	_probeList = currentTables();
	for (uint n = 0; n < _probeList.size(); n++) {
		const Table &t = _tables[_probeList[n]];
		logf("table %d: kind %d slot %d offset %d item %d, %d hotspots", n + 1, t.kind, t.slot, t.offset, t.itemSize,
			tableCount(t));
	}
	_probeTable = 0;
	_probeIndex = 0;
	_probeFrames = 0;
}

void ToltecsGuide::runCommand(const Common::String &text) {
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
		_actions.clear();
	} else if (c == "goto") {
		setCurrent(x > 0 ? x - 1 : 0);
		_sectionSaved = _current;   // no save for the section it starts in
		logf("current step %d", _current + 1);
	} else if (c == "reload") {
		if (ConfMan.hasKey("guide_file"))
			load(ConfMan.get("guide_file"));
	} else if (c == "objects") {
		startProbe();
	} else if (c == "records") {
		// the raw records of each current table
		const Common::Array<int> current = currentTables();
		for (uint n = 0; n < current.size(); n++) {
			const Table &t = _tables[current[n]];
			for (int i = 0; i < tableCount(t); i++) {
				Rect r;
				rectOf(t, i, r);
				logf("  %d.%d '%s' %d,%d %dx%d [%s]", n + 1, i, nameOf(t, i).c_str(), r.x, r.y, r.w, r.h,
					entryBytes(t, i).c_str());
			}
		}
	} else if (c == "lines") {
		// read the dialogue's lines
		Action a;
		a.type = Action::kChoose;
		a.x = a.y = 0;
		a.left = a.right = a.dbl = false;
		a.frames = 0;
		_actions.push_back(a);
	} else if (c == "pixel") {
		// pixel X Y: the palette index on the screen there (the bottom bar included)
		logf("pixel %d,%d = %d", x, y, _vm->_screen->_frontScreen[CLIP(y, 0, 399) * 640 + CLIP(x, 0, 639)]);
	} else if (c == "row") {
		// row Y: the palette indexes of a screen row, runs as value*count
		const byte *row = _vm->_screen->_frontScreen + CLIP(x, 0, 399) * 640;
		Common::String out;
		for (int i = 0; i < 640;) {
			int j = i;
			while (j < 640 && row[j] == row[i])
				j++;
			out += Common::String::format(" %d@%d*%d", row[i], i, j - i);
			i = j;
		}
		logf("row %d:%s", x, out.c_str());
	} else if (c == "state") {
		listState();
	} else if (c == "verbs") {
		_logVerb = x != 0;
	} else if (c == "mouse") {
		move(x, y, 2);
	} else if (c == "click" || c == "rclick" || c == "dclick") {
		click(x, y, c == "rclick", c == "dclick");
	} else if (c == "key") {
		// the engine's actions: 1 skip the line, 4 skip a movie, 5 the options menu, 6 skip the ride
		_vm->_action = x;
		_actionFrames = 3;
	} else if (c == "save") {
		Common::String desc = Common::String::format("guide %d", x);
		_vm->requestSavegame(x, desc);
		logf("saving slot %d", x);
	} else if (c == "load") {
		_vm->requestLoadgame(x);
		_tables.clear();
		logf("loading slot %d", x);
	} else if (c == "dump") {
		// dump SLOT OFFSET [COUNT]: int16 words of a script slot
		char buf[32] = "";
		int n = 32;
		sscanf(line.c_str(), "%31s %d %d %d", buf, &x, &y, &n);
		const byte *data = x >= 0 && x < kMaxScriptSlots ? _vm->_script->getSlotData(x) : nullptr;
		Common::String out;
		for (int i = 0; data && i < n && (uint32)(y + 2 * i + 2) <= _vm->_script->getSlotSize(x); i++)
			out += Common::String::format(" %d", (int16)READ_LE_UINT16(data + y + 2 * i));
		logf("slot %d +%d:%s", x, y, out.c_str());
	} else if (c == "text") {
		// text SLOT OFFSET: the script string there
		const byte *data = x >= 0 && x < kMaxScriptSlots ? _vm->_script->getSlotData(x) : nullptr;
		if (data && (uint32)y < _vm->_script->getSlotSize(x))
			logf("text %d +%d: %s", x, y, scriptText(data + y, data + _vm->_script->getSlotSize(x)).c_str());
	} else if (c == "quit") {
		_vm->quitGame();
	} else if (parseLine(line, st)) {
		// one step now (after those already queued), as a walkthrough of its own
		const bool queued = _state != kStopped && _remaining > 0;
		_steps.insert_at(MIN<int>(_current + (queued ? _remaining : 0), _steps.size()), st);
		_remaining = queued ? _remaining + 1 : 1;
		if (!queued) {
			setCurrent(_current);   // fresh counters (approaches, retries) for it
			_state = kWaitIdle;
		}
	} else {
		logf("unknown command");
	}
}

void ToltecsGuide::pollControl() {
	if (_controlPath.empty() || _frame % 4)
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

} // End of namespace Toltecs
