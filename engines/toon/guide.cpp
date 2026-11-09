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

#include "toon/toon.h"
#include "toon/audio.h"
#include "toon/character.h"
#include "toon/drew.h"
#include "toon/flux.h"
#include "toon/guide.h"
#include "toon/hotspot.h"
#include "toon/state.h"
#include "toon/text.h"

namespace Toon {

static const uint32 kSettle = 1200;      // ms a step waits before it can be done (Drew starts walking or talking)
static const uint32 kIdleSettle = 600;   // ms the game must stay idle
static const uint32 kChoiceSettle = 700; // ms a conversation must wait for a choice
static const uint32 kMissing = 30000;    // ms to wait for a step's hotspot to show up

ToonGuide::ToonGuide(ToonEngine *vm) : _vm(vm) {
	if (ConfMan.hasKey("guide_file"))
		load(ConfMan.get("guide_file"));
	if (ConfMan.hasKey("guide_control"))
		_controlPath = ConfMan.get("guide_control");
	if (ConfMan.hasKey("guide_save_sections"))
		_saveSections = ConfMan.getInt("guide_save_sections");
	if (ConfMan.hasKey("guide_hide_cursor"))
		_hideCursor = ConfMan.getBool("guide_hide_cursor");
}

void ToonGuide::logf(const char *fmt, ...) {
	va_list va;
	va_start(va, fmt);
	Common::String s = Common::String::vformat(fmt, va);
	va_end(va);
	debug("guide: %u %s", g_system->getMillis(), s.c_str());
}

bool ToonGuide::parseLine(const Common::String &text, Step &st) const {
	Common::String line = text, comment;
	size_t semi = line.findFirstOf(';');
	if (semi != Common::String::npos) {
		comment = line.substr(semi + 1);
		comment.trim();
		line = line.substr(0, semi);
	}
	line.trim();
	st.a = st.b = st.c = 0;
	st.now = false;
	st.comment = comment;
	if (line == "b") {
		st.kind = Step::kStore;
		return true;
	}
	if (line == "A") {
		st.kind = Step::kArcade;
		return true;
	}
	if (line == "C") {
		st.kind = Step::kClown;
		return true;
	}
	if (line.size() < 2 || line[1] != ' ')
		return false;
	st.c = 0;
	int n = sscanf(line.c_str() + 2, "%d %d %d", &st.a, &st.b, &st.c);
	switch (line[0]) {
	case 'e': st.kind = Step::kClick; return n >= 1;
	case 'E': st.kind = Step::kClick; st.now = true; return n >= 1;
	case 'l': st.kind = Step::kLook; return n >= 1;
	case 'u': st.kind = Step::kUse; return n == 2;
	case 'p': st.kind = Step::kUsePoint; return n == 3;
	case 'd': st.kind = Step::kDouble; return n == 2;
	case 'v': st.kind = Step::kGlobalWait; return n == 2;
	case 'V': st.kind = Step::kGlobalWait; st.now = true; return n == 2;
	case 'j': st.kind = Step::kUntil; return n == 2;
	case 'J': st.kind = Step::kSkipIf; return n == 2;
	case 'q': st.kind = Step::kUntilScene; return n == 2;
	case 'H': st.kind = Step::kUntilTalk; return n == 2;
	case 'R': st.kind = Step::kGlobalWait; st.now = true; st.c = 1; return n >= 2;
	case 'f': st.kind = Step::kFrameWait; return n == 2;
	case 'h': st.kind = Step::kHeldWait; return n >= 1;
	case 't': st.kind = Step::kCombine; return n == 2;
	case 'T': st.kind = Step::kHeldOnItem; st.now = true; return n >= 1;
	case 'i': st.kind = Step::kItem; return n >= 1;
	case 'k': st.kind = Step::kSubject; return n >= 1;
	case 'g': st.kind = Step::kPoint; return n == 2;
	case 'G': st.kind = Step::kPoint; st.now = true; return n == 2;
	case 'P': st.kind = Step::kPress; return n == 2;
	case 'c': st.kind = Step::kAnimClick; st.now = true; return n >= 1;
	case 'A': st.kind = Step::kArcade; return true;
	case 'n': st.kind = Step::kFlagWait; return n == 2;
	case 'a': st.kind = Step::kSceneWait; return n >= 1;
	case 'w': st.kind = Step::kWait; st.a *= 1000; return n >= 1;
	case 'W': st.kind = Step::kWait; return n >= 1;
	default: return false;
	}
}

bool ToonGuide::load(const Common::String &path) {
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

void ToonGuide::next() {
	_approaches = 0;
	_current = MIN<int>(_current + 1, _steps.size());
	if (_remaining > 0 && --_remaining == 0)
		_state = kStopped;
	else
		_state = kWaitIdle;
}

bool ToonGuide::waitStep(const Step &st) const {
	return st.kind == Step::kWait || st.kind == Step::kFlagWait || st.kind == Step::kSceneWait ||
		st.kind == Step::kGlobalWait || st.kind == Step::kFrameWait || st.kind == Step::kHeldWait;
}

// Drew is waiting for the player: nothing running (every action runs inside a nested doFrame loop), no menu,
// conversation, inventory, voice, walk.
bool ToonGuide::isIdle() const {
	State *s = _vm->state();
	if (_depth > 1 || s->_inConversation || s->_mouseHidden || s->_inInventory || s->_inMenu)
		return false;
	if (_vm->getAudioManager()->voiceStillPlaying())
		return false;
	Character *drew = _vm->getDrew();
	return drew->getX() == drew->getFinalX() && drew->getY() == drew->getFinalY();
}

// Fingers' WACMAN game in the arcade: standing blocks his high stars and lets ours fly; when he ducks he throws
// low, so we duck too (and his target behind him is open: the stars already flying may hit it). Done when the game
// is over (global 1 = 102).
bool ToonGuide::arcade(uint32 now) {
	State *s = _vm->state();
	// lost: global 1 = 102; won: the game screen (the room scrolled to 640) is gone
	if (s->_currentScrollValue >= 640)
		_arcadeOn = true;
	if (s->_gameGlobalData[1] == 102 || (_arcadeOn && s->_currentScrollValue < 640)) {
		_arcadeOn = false;
		return true;
	}
	if (!_moves.empty() || !_arcadeOn)
		return false;
	// global 8: Fingers ducks; global 3: Drew's stance (0 up, 1 ducking, 2 flat)
	const int want = s->_gameGlobalData[8] == 1 ? 1 : 0;
	const int stance = s->_gameGlobalData[3];
	Move m;
	m.target = Move::kPoint;
	if (stance != want) {
		if (now - _arcadeStance < 250)
			return false;
		m.x = stance > want ? 126 : 80;
		m.y = stance > want ? 331 : 387;
		_arcadeStance = now;
	} else if (!want && now - _arcadeFired > 450) {
		m.x = 176;
		m.y = 359;
		_arcadeFired = now;
	} else {
		return false;
	}
	m.buttons = 1;
	m.frames = 2;
	_moves.push_back(m);
	m.buttons = 0;
	m.frames = 3;
	_moves.push_back(m);
	return false;
}

// The clown door in Nefarious' dungeon: a Simon game. Scene animations 14-19 are the parts (ears, tongue, eyes,
// nose); each round the door shows the whole sequence, one part more each time; then the player repeats it.
// Done when the close-up is left (the door opens) or nothing happens for a long time.
bool ToonGuide::clown(uint32 t) {
	State *s = _vm->state();
	if (s->_currentScrollValue >= 640)
		_clownSeen = true;
	if ((_clownSeen && s->_currentScrollValue < 640) || (int32)(t - _clownLast) > 30000) {
		_clownSeen = false;
		logf("clown done (%d parts last)", _clownSeq.size());
		return true;
	}
	if (!_moves.empty())
		return false;
	if (_clownTyping) {
		clownType(t);
		if (_clownTyping)
			return false;
	}
	if ((int32)(t - _clownClicked) < 300) {
		for (int i = 0; i < 6; i++) {
			SceneAnimation *sa = _vm->getSceneAnimation(14 + i);
			_clownFrames[i] = sa->_active && sa->_animInstance ? sa->_animInstance->getFrame() : 0;
		}
		return false;
	}
	for (int i = 0; i < 6; i++) {
		SceneAnimation *sa = _vm->getSceneAnimation(14 + i);
		const int f = sa->_active && sa->_animInstance ? sa->_animInstance->getFrame() : 0;
		if (f > 0 && (_clownFrames[i] == 0 || f < _clownFrames[i])) {   // starts, or starts again
			_clownSeq.push_back(i);
			_clownLast = t;
			logf("clown shows %d", i);
		}
		_clownFrames[i] = f;
	}
	if (!_clownSeq.empty() && !_clownTyping && (int32)(t - _clownLast) > 1500 && controllable()) {
		logf("clown: repeating %d parts", _clownSeq.size());
		_clownTyping = true;
		_clownNext = 0;
	}
	return false;
}

// one part at a time, each once the parts are still (a long one, as the tongue, would eat the next click)
bool ToonGuide::clownType(uint32 t) {
	static const int kPoints[6][2] = {{202, 163}, {453, 161}, {442, 316}, {378, 187}, {284, 188}, {332, 230}};
	for (int i = 0; i < 6; i++) {
		SceneAnimation *sa = _vm->getSceneAnimation(14 + i);
		if (sa->_active && sa->_animInstance && sa->_animInstance->getFrame() > 0)
			_clownStill = t;
	}
	if ((int32)(t - _clownStill) < 250)
		return false;
	if (_clownNext >= (int)_clownSeq.size()) {
		_clownSeq.clear();
		_clownTyping = false;
		_clownClicked = t;
		_clownLast = t;
		return true;
	}
	Move m;
	m.target = Move::kPoint;
	m.x = kPoints[_clownSeq[_clownNext]][0];
	m.y = kPoints[_clownSeq[_clownNext]][1];
	m.frames = 3;
	_moves.push_back(m);
	m.buttons = 1;
	m.frames = 3;
	_moves.push_back(m);
	m.buttons = 0;
	m.frames = 6;
	_moves.push_back(m);
	_clownNext++;
	_clownLast = t;
	_clownStill = t + 400;   // the part starts to move a little after the click
	return false;
}

bool ToonGuide::controllable() const {
	State *s = _vm->state();
	return _depth <= 1 && !s->_inConversation && !s->_mouseHidden && !s->_inInventory && !s->_inMenu;
}

bool ToonGuide::choosing() const {
	State *s = _vm->state();
	return s->_inConversation && s->_showConversationIcons && !s->_mouseHidden &&
		!_vm->getAudioManager()->voiceStillPlaying();
}

// what the game picks at a screen point: the bag, Drew (with an item in hand), Flux, then the room's hotspots
int ToonGuide::hotspotAt(int x, int y) const {
	State *s = _vm->state();
	if (s->_sackVisible && x > 0 && x < 40 && y > 356 && y < 396)
		return -2;
	int16 x1, y1, x2, y2;
	const int sx = x + s->_currentScrollValue;
	if (s->_mouseState > 0) {
		_vm->getDrew()->getAnimationInstance()->getRect(&x1, &y1, &x2, &y2);
		if (sx >= x1 && sx <= x2 && y >= y1 && y <= y2)
			return -4;
	}
	if (_vm->getFlux()->getVisible()) {
		_vm->getFlux()->getAnimationInstance()->getRect(&x1, &y1, &x2, &y2);
		if (sx >= x1 && sx <= x2 && y >= y1 && y <= y2)
			return -3;
	}
	int mx = x + s->_currentScrollValue + (s->_inCutaway ? TOON_BACKBUFFER_WIDTH : 0);
	int32 hot = _vm->getHotspots()->find(mx, y);
	if (hot < 0)
		return 0;
	HotspotData *h = _vm->getHotspots()->get(hot);
	return h->getData(14) + (h->getType() == 3 ? 2000 : 0);
}

// A screen point where the mouse picks that hotspot: its middle, or the nearest point that picks it.
bool ToonGuide::hotspotPoint(int id, int &x, int &y) const {
	State *s = _vm->state();
	if (id == -3 || id == -4) {
		int16 x1, y1, x2, y2;
		Character *c = id == -3 ? (Character *)_vm->getFlux() : (Character *)_vm->getDrew();
		if (!c->getVisible())
			return false;
		c->getAnimationInstance()->getRect(&x1, &y1, &x2, &y2);
		x = (x1 + x2) / 2 - s->_currentScrollValue;
		y = (y1 + y2) / 2;
		return true;
	}
	// a hotspot can be in the list more than once (one per place it can be): a point of one in view that the
	// game picks for it, else one in view (something covers it), else one off the screen to walk toward
	Hotspots *hs = _vm->getHotspots();
	bool covered = false, outside = false;
	int cx = 0, cy = 0, ox = 0;
	for (int i = 0; i < hs->getCount(); i++) {
		HotspotData *h = hs->get(i);
		if (h->getData(14) + (h->getType() == 3 ? 2000 : 0) != id)
			continue;
		const int off = s->_currentScrollValue + (s->_inCutaway ? TOON_BACKBUFFER_WIDTH : 0);
		const int x1 = MAX<int>(h->getX1() - off, 0), x2 = MIN<int>(h->getX2() - off, 639);
		const int y1 = MAX<int>(h->getY1(), 0), y2 = MIN<int>(h->getY2(), 399);
		const int mx = (x1 + x2) / 2, my = (y1 + y2) / 2;
		if (x1 > x2 || y1 > y2) {
			if (!outside)
				ox = CLIP<int>((h->getX1() + h->getX2()) / 2 - off, -2000, 2000);
			outside = true;
			continue;
		}
		if (hotspotAt(mx, my) == id) {
			x = mx;
			y = my;
			return true;
		}
		int best = -1, bx = mx, by = my;
		for (int py = y1; py <= y2; py += 4)
			for (int px = x1; px <= x2; px += 4)
				if (hotspotAt(px, py) == id) {
					const int d = ABS(px - mx) + ABS(py - my);
					if (best < 0 || d < best) {
						best = d;
						bx = px;
						by = py;
					}
				}
		if (best >= 0) {
			x = bx;
			y = by;
			return true;
		}
		if (!covered) {
			cx = mx;
			cy = my;
		}
		covered = true;
	}
	if (covered) {
		x = cx;
		y = cy;
		return true;
	}
	if (outside) {
		x = ox;
		y = 200;
		return true;   // off the screen: the caller walks toward it
	}
	return false;
}

bool ToonGuide::available(const Step &st) const {
	State *s = _vm->state();
	int x, y;
	switch (st.kind) {
	case Step::kClick:
	case Step::kLook:
		return hotspotPoint(st.a, x, y);
	case Step::kUse:
		return s->hasItemInInventory(st.a) && hotspotPoint(st.b, x, y);
	case Step::kUsePoint:
		return s->hasItemInInventory(st.a);
	case Step::kDouble:
		return hotspotPoint(st.a, x, y) && hotspotPoint(st.b, x, y);
	case Step::kCombine:
		return s->hasItemInInventory(st.a) && s->hasItemInInventory(st.b);
	case Step::kHeldOnItem:
		return s->_mouseState >= 0 && s->hasItemInInventory(st.a);
	case Step::kItem:
		return s->hasItemInInventory(st.a);
	case Step::kStore:
		return s->_mouseState >= 0;
	case Step::kSubject: {
		int n = 0;
		for (int i = 0; i < 10; i++)
			n += s->_conversationState[s->_currentConversationId].state[i]._data2 == 1;
		return st.a < n;
	}
	default:
		return true;
	}
}

bool ToonGuide::position(const Move &m, int &x, int &y) const {
	State *s = _vm->state();
	switch (m.target) {
	case Move::kHotspot:
		return hotspotPoint(m.id, x, y);
	case Move::kBagItem:
		for (int32 i = 0; i < s->_numInventoryItems; ++i)
			if (s->_inventory[i] == m.id) {
				x = 57 * (i % 7) + 114 + 20;
				y = ((9 * (i % 7)) & 0xf) + 56 * (i / 7) + 80 + 22;
				return true;
			}
		return false;
	case Move::kSubject: {
		int a = 0;
		for (int i = 0; i < 10; i++)
			if (s->_conversationState[s->_currentConversationId].state[i]._data2 == 1) {
				if (a == m.id) {
					x = 50 + a * 60 + 25;
					y = 360;
					return true;
				}
				a++;
			}
		return false;
	}
	default:
		x = m.x;
		y = m.y;
		return true;
	}
}

// Point for a few frames, press for two, release: the game reacts to a button going down.
void ToonGuide::click(Move::Target target, int id, int buttons, int x, int y) {
	Move m;
	m.target = target;
	m.id = id;
	m.x = x;
	m.y = y;
	m.frames = target == Move::kBagItem ? 20 : 6;   // the bag fades in
	_moves.push_back(m);
	m.buttons = buttons;
	m.frames = 2;
	_moves.push_back(m);
	m.buttons = 0;
	m.frames = 4;
	_moves.push_back(m);
}

void ToonGuide::execute(const Step &st) {
	_moves.clear();
	const int bagX = 20, bagY = 376;           // the bag at the bottom left
	const int outX = 600, outY = 20;           // a spot in the bag screen with no item: leaves it
	switch (st.kind) {
	case Step::kClick:
		click(Move::kHotspot, st.a, 1);
		break;
	case Step::kLook:
		click(Move::kHotspot, st.a, 2);
		break;
	case Step::kUse:
		click(Move::kPoint, 0, 1, bagX, bagY);
		click(Move::kBagItem, st.a, 1);
		click(Move::kPoint, 0, 1, outX, outY);
		click(Move::kHotspot, st.b, 1);
		break;
	case Step::kUsePoint:
		click(Move::kPoint, 0, 1, bagX, bagY);
		click(Move::kBagItem, st.a, 1);
		click(Move::kPoint, 0, 1, outX, outY);
		click(Move::kPoint, 0, 1, st.b, st.c);
		break;
	case Step::kCombine:
		click(Move::kPoint, 0, 1, bagX, bagY);
		click(Move::kBagItem, st.a, 1);
		click(Move::kBagItem, st.b, 1);
		click(Move::kPoint, 0, 2, outX, outY);
		break;
	case Step::kHeldOnItem:
		click(Move::kPoint, 0, 2, bagX, bagY);   // a right click opens the bag without putting the held item in
		click(Move::kBagItem, st.a, 1);
		break;
	case Step::kItem:
		// the bag is left once Drew has spoken (or a page has been seen): see kSettle
		click(Move::kPoint, 0, 1, bagX, bagY);
		click(Move::kBagItem, st.a, 2);
		break;
	case Step::kSubject:
		click(Move::kSubject, st.a, 1);
		break;
	case Step::kPoint:
		click(Move::kPoint, 0, 1, st.a, st.b);
		break;
	case Step::kStore:
		click(Move::kPoint, 0, 1, 20, 376);
		break;
	case Step::kArcade:
		_arcadeStance = 0;
		_arcadeFired = 0;
		break;
	case Step::kClown:
		_clownSeq.clear();
		_clownLast = now();
		for (int i = 0; i < 7; i++)
			_clownFrames[i] = 0;
		break;
	case Step::kPress: {
		// a press right now, whatever the game is doing (a running machine reads the button)
		Move m;
		m.target = Move::kPoint;
		m.x = st.a;
		m.y = st.b;
		m.buttons = 0;
		m.frames = 2;   // point first: the game picks what is under the mouse
		_moves.push_back(m);
		m.buttons = 1;
		m.frames = 3;
		_moves.push_back(m);
		m.buttons = 0;
		m.frames = 3;
		_moves.push_back(m);
		break;
	}
	case Step::kAnimClick: {
		SceneAnimation *sa = _vm->getSceneAnimation(st.a);
		int16 x1 = 0, y1 = 0, x2 = 0, y2 = 0;
		if (sa->_active && sa->_animInstance)
			sa->_animInstance->getRect(&x1, &y1, &x2, &y2);
		click(Move::kPoint, 0, 1, (x1 + x2) / 2 + st.b - _vm->state()->_currentScrollValue, (y1 + y2) / 2 + (st.b ? 8 : 0));
		break;
	}
	case Step::kDouble:
		click(Move::kHotspot, st.a, 1);
		click(Move::kHotspot, st.b, 1);
		break;
	default:
		break;
	}
}

Common::String ToonGuide::describe(const Step &st) const {
	static const char *const names[] = {"click", "look", "use", "combine", "item", "subject", "point", "flag",
		"scene", "wait", "store", "use at", "double", "global", "until", "press", "arcade", "frame", "clown", "held on", "anim click", "held", "skip if", "until scene", "until talk"};
	return Common::String::format("%s %d %d %s", names[st.kind], st.a, st.b, st.comment.c_str());
}

void ToonGuide::tick(int32 &mouseX, int32 &mouseY, int32 &buttons) {
	const uint32 now = g_system->getMillis();
	pollControl();

	if (!_moves.empty()) {
		Move &m = _moves[0];
		int x, y;
		if (position(m, x, y))
			_mouse = Common::Point(CLIP(x, 0, 639), CLIP(y, 0, 399));
		mouseX = _mouse.x;
		mouseY = _mouse.y;
		buttons = m.buttons;
		g_system->warpMouse(_mouse.x, _mouse.y);
		if (--m.frames <= 0)
			_moves.remove_at(0);
		if (_moves.empty() && _state == kRunning && _steps[_current].kind != Step::kArcade &&
			_steps[_current].kind != Step::kClown) {
			if (_steps[_current].kind == Step::kPress) {
				// a press does not wait for the game
				logf("step %d done (scene %d)", _current + 1, _vm->state()->_currentScene);
				next();
			} else {
				_state = kSettle;
				_stateSince = now;
			}
		}
		return;
	}
	if (_state == kStopped)
		return;
	// while the guide plays, the real mouse does nothing
	mouseX = _mouse.x;
	mouseY = _mouse.y;
	buttons = 0;

	const bool idle = isIdle();
	if (idle) {
		if (!_idleSince)
			_idleSince = now;
	} else {
		_idleSince = 0;
	}
	const bool choice = choosing();
	if (!idle && !choice)
		_busy = true;
	if (choice) {
		if (!_choiceSince)
			_choiceSince = now;
	} else {
		_choiceSince = 0;
	}

	if (_state == kSettle) {
		// the bag still open (a click on its way out came while Drew was talking): leave it, keeping the item
		State *gs = _vm->state();
		if (gs->_inInventory && now - _stateSince > 2000 && !_vm->getAudioManager()->voiceStillPlaying()) {
			click(Move::kPoint, 0, gs->_mouseState >= 0 ? 1 : 2, 600, 20);
			_stateSince = now;
			return;
		}
		if (now - _stateSince < kSettle)
			return;
		const Step &cur = _steps[_current];
		const bool waited = waitStep(cur);
		// an E step next: as soon as the player has control
		const bool nowNext = _current + 1 < (int)_steps.size() &&
			((_busy && _steps[_current + 1].now && controllable()) || (_steps[_current + 1].kind == Step::kGlobalWait || _steps[_current + 1].kind == Step::kFrameWait ||
			_steps[_current + 1].kind == Step::kHeldWait ||
			_steps[_current + 1].kind == Step::kClown));
		const bool reacted = waited || _busy || nowNext || now - _stateSince > 3000;
		const bool ready = waited || nowNext || cur.now || (_idleSince && now - _idleSince >= kIdleSettle) ||
			(_choiceSince && now - _choiceSince >= kChoiceSettle && (_busy || now - _stateSince > 3000));
		if (!ready || !reacted)
			return;
		if (_approaching) {
			_approaching = false;
			_state = kWaitIdle;
			return;
		}
		logf("step %d done (scene %d)", _current + 1, _vm->state()->_currentScene);
		next();
		return;
	}

	if (_state == kRunning) {
		const Step &st = _steps[_current];
		bool done = false;
		if (st.kind == Step::kArcade)
			done = arcade(now);
		else if (st.kind == Step::kClown)
			done = clown(now);
		else if (st.kind == Step::kWait)
			done = now - _stateSince >= (uint32)st.a;
		else if (st.kind == Step::kFlagWait)
			done = st.a >= 0 && st.a < 256 && _vm->state()->_gameFlag[st.a] == st.b;
		else if (st.kind == Step::kHeldWait)
			done = _vm->state()->_mouseState == st.a;
		else if (st.kind == Step::kFrameWait) {
			SceneAnimation *sa = st.a >= 0 && st.a < 64 ? _vm->getSceneAnimation(st.a) : nullptr;
			done = sa && sa->_active && sa->_animInstance && (st.b < 0 || sa->_animInstance->getFrame() == st.b);
		} else if (st.kind == Step::kGlobalWait && now - _stateSince > 10000) {
			logf("step %d: global %d never got there, going on", _current + 1, st.a);   // a machine that stopped
			done = true;
		} else if (st.kind == Step::kGlobalWait)
			done = st.a >= 0 && st.a < 256 && (st.now ? _vm->state()->_gameGlobalData[st.a] >= st.b + (st.c ? _loops % 3 - 1 : 0) :
				_vm->state()->_gameGlobalData[st.a] == st.b);
		else if (st.kind == Step::kSceneWait)
			done = _vm->state()->_currentScene == st.a && idle;
		if (done) {
			_state = kSettle;
			_stateSince = now - kSettle;
		}
		return;
	}

	// kWaitIdle
	{
		// the bag left open by a step (an item held on another one): leave it, keeping the item
		State *gs = _vm->state();
		if (gs->_inInventory && !_vm->getAudioManager()->voiceStillPlaying()) {
			if (!_bagSince)
				_bagSince = now;
			if (now - _bagSince > 2000) {
				click(Move::kPoint, 0, gs->_mouseState >= 0 ? 1 : 2, 600, 20);
				_bagSince = 0;
			}
			return;
		}
		_bagSince = 0;
	}
	if (_current >= (int)_steps.size()) {
		logf("walkthrough finished");
		_state = kStopped;
		return;
	}
	const Step &st = _steps[_current];
	if (st.kind == Step::kSubject) {
		if (!_choiceSince && _idleSince && now - _idleSince > 8000) {
			logf("step %d: no conversation, skipped", _current + 1);
			next();
			return;
		}
		if (!_choiceSince || now - _choiceSince < kChoiceSettle)
			return;
	} else if (st.now && !waitStep(st)) {
		if (!controllable())
			return;
	} else if (st.kind == Step::kPress || st.kind == Step::kArcade || st.kind == Step::kClown) {
		// no wait
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
			_vm->saveGame(slot, st.section);
		logf("section %d step %d slot %d: %s", num, _current + 1, slot, st.section.c_str());
	}
	if (_heardTalk && st.kind != Step::kUntilTalk) {
		next();   // the loop is over: straight to its end
		return;
	}
	if (st.kind == Step::kUntilTalk) {
		// a loop: back to an earlier step until that line has been said (the pot tips over)
		if (_heardTalk || ++_loops > 40) {
			logf("step %d: until talk %d done (%d loops)", _current + 1, st.a, _loops);
			_loops = 0;
			_heardTalk = false;
			next();
		} else {
			_current = MAX(_current - st.b, 0);
			if (_remaining > 0)
				_remaining += st.b;
			_state = kWaitIdle;
		}
		return;
	}
	if (st.kind == Step::kUntilScene) {
		// a loop: back to an earlier step until Drew is in that scene (a door that is open only now and then)
		if (_vm->state()->_currentScene == st.a || ++_loops > 40) {
			logf("step %d: until scene %d done (%d loops)", _current + 1, st.a, _loops);
			_loops = 0;
			next();
		} else {
			logf("step %d: not in scene %d yet, back %d steps", _current + 1, st.a, st.b);
			_current = MAX(_current - st.b, 0);
			if (_remaining > 0)
				_remaining += st.b;
			_state = kWaitIdle;
		}
		return;
	}
	if (st.kind == Step::kSkipIf) {
		State *gs = _vm->state();
		if (gs->hasItemInInventory(st.a) || gs->_mouseState == st.a) {
			logf("step %d: have %d, skip %d steps", _current + 1, st.a, st.b);
			const int skip = MIN<int>(st.b, _steps.size() - 1 - _current);
			_current += skip;
			if (_remaining > 0)
				_remaining = MAX(_remaining - skip, 1);
		}
		next();
		return;
	}
	if (st.kind == Step::kUntil) {
		// a loop: back to an earlier step until Drew has the item (a random prize)
		State *gs = _vm->state();
		const bool got = st.a < 0 ? gs->_mouseState < 0 : gs->hasItemInInventory(st.a) || gs->_mouseState == st.a;
		if (got || ++_loops > 40) {
			logf("step %d: until %d done (%d loops)", _current + 1, st.a, _loops);
			_loops = 0;
			next();
		} else {
			logf("step %d: no %d yet, back %d steps", _current + 1, st.a, st.b);
			_current = MAX(_current - st.b, 0);
			if (_remaining > 0)
				_remaining += st.b;
			_state = kWaitIdle;
		}
		return;
	}
	if (st.kind == Step::kStore && _vm->state()->_mouseState < 0) {
		logf("step %d: nothing in hand, skipped", _current + 1);
		next();
		return;
	}
	if (!available(st)) {
		if (!_missingSince)
			_missingSince = now;
		if (now - _missingSince > kMissing) {
			logf("step %d: its object is not here (scene %d); stopping", _current + 1, _vm->state()->_currentScene);
			_state = kStopped;
			_missingSince = 0;
		}
		return;
	}
	_missingSince = 0;
	int hx, hy;
	const int target = st.kind == Step::kUse ? st.b : st.a;
	if ((st.kind == Step::kClick || st.kind == Step::kLook || st.kind == Step::kUse) && hotspotPoint(target, hx, hy) &&
		(hx < 0 || hx > 639)) {
		// off a scrolling room: walk toward it, the room scrolls; then this step again
		Character *drew = _vm->getDrew();
		Step walk;
		walk.kind = Step::kPoint;
		walk.a = hx < 0 ? 40 : 600;
		walk.b = CLIP<int>(drew->getY(), 0, 399);
		if (++_approaches > 4) {
			logf("step %d: %d stays out of view; stopping", _current + 1, target);
			_approaches = 0;
			_state = kStopped;
			return;
		}
		logf("step %d: %d is out of view, walking to %d,%d", _current + 1, target, walk.a, walk.b);
		_busy = false;
		_approaching = true;
		_state = kRunning;
		_stateSince = now;
		execute(walk);
		return;
	}
	logf("step %d: %s", _current + 1, describe(st).c_str());
	_busy = false;
	_state = kRunning;
	_stateSince = now;
	execute(st);
}

void ToonGuide::onTalk(int32 dialogId, const char *text) {
	logf("talk %d: %s", dialogId, text);
	for (uint i = _current; i < _steps.size() && i < (uint)_current + 8; i++)
		if (_steps[i].kind == Step::kUntilTalk && _steps[i].a == dialogId)
			_heardTalk = true;
}

void ToonGuide::listObjects() {
	State *s = _vm->state();
	logf("scene %d, idle %d, depth %d, conversation %d, choosing %d, inventory %d, hidden %d, held %d, scroll %d, "
		"drew %d,%d, guide state %d step %d", s->_currentScene, isIdle(), _depth, s->_inConversation, choosing(),
		s->_inInventory, s->_mouseHidden, s->_mouseState, s->_currentScrollValue, _vm->getDrew()->getX(),
		_vm->getDrew()->getY(), _state, _current + 1);
	int16 x1, y1, x2, y2;
	_vm->getFlux()->getAnimationInstance()->getRect(&x1, &y1, &x2, &y2);
	logf("  flux %d,%d-%d,%d visible %d", x1, y1, x2, y2, _vm->getFlux()->getVisible());
	Hotspots *hs = _vm->getHotspots();
	for (int i = 0; i < hs->getCount(); i++) {
		HotspotData *h = hs->get(i);
		const int id = h->getData(14) + (h->getType() == 3 ? 2000 : 0);
		const char *name = id < 2000 && id >= 0 ? _vm->getRoomTexts()->getText(id) : "";
		logf("  hotspot %d '%s' at %d,%d (%d,%d-%d,%d) type %d", id, name ? name : "",
			(h->getX1() + h->getX2()) / 2 - s->_currentScrollValue, (h->getY1() + h->getY2()) / 2,
			h->getX1(), h->getY1(), h->getX2(), h->getY2(), h->getType());
	}
	for (int32 i = 0; i < s->_numInventoryItems; i++)
		if (s->_inventory[i])
			logf("  item %d", s->_inventory[i]);
	if (s->_inConversation)
		for (int i = 0, a = 0; i < 10; i++)
			if (s->_conversationState[s->_currentConversationId].state[i]._data2 == 1)
				logf("  subject %d (slot %d, icon %d)", a++, i, s->_conversationState[s->_currentConversationId].state[i]._data3);
}

void ToonGuide::runCommand(const Common::String &text) {
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
		_current = CLIP<int>(x > 0 ? x - 1 : 0, 0, _steps.size());
		_sectionSaved = _current;
		logf("current step %d", _current + 1);
	} else if (c == "reload") {
		if (ConfMan.hasKey("guide_file"))
			load(ConfMan.get("guide_file"));
	} else if (c == "objects") {
		listObjects();
	} else if (c == "flags") {
		State *gs = _vm->state();
		Common::String g, f;
		for (int i = 0; i < 256; i++) {
			if (gs->_gameGlobalData[i])
				g += Common::String::format(" %d=%d", i, gs->_gameGlobalData[i]);
			if (gs->_gameFlag[i])
				f += Common::String::format(" %d=%d", i, gs->_gameFlag[i]);
		}
		logf("globals%s", g.c_str());
		logf("flags%s", f.c_str());
	} else if (c == "stack") {
		for (int i = 0; i < 4; i++) {
			EMCState *es = _vm->getScriptState(i);
			Common::String r = Common::String::format("sp %d bp %d:", es->sp, es->bp);
			for (int j = 0; j < 30; j++)
				if (es->regs[j])
					r += Common::String::format(" r%d=%d", j, es->regs[j]);
			r += " |";
			for (int j = es->sp; j < EMCState::kStackSize; j++)
				r += Common::String::format(" %d", es->stack[j]);
			logf("state %d %s", i, r.c_str());
		}
	} else if (c == "anims") {
		Common::String r;
		for (int i = 0; i < 64; i++) {
			SceneAnimation *sa = _vm->getSceneAnimation(i);
			if (!sa->_active || !sa->_animInstance)
				continue;
			Animation *an = sa->_animInstance->getAnimation();
			int16 x1, y1, x2, y2;
			sa->_animInstance->getRect(&x1, &y1, &x2, &y2);
			r += Common::String::format(" %d:%s:%d/%d@%d,%d", i, an ? an->_name : "-", sa->_animInstance->getFrame(),
				an ? an->_numFrames : 0, (x1 + x2) / 2, (y1 + y2) / 2);
		}
		logf("anims%s", r.c_str());
	} else if (c == "regs") {
		for (int i = 0; i < 64; i++) {
			SceneAnimationScript *sc = _vm->getSceneAnimationScript(i);
			if (!sc->_data)
				continue;
			Common::String r = sc->_active ? " active" : "";
			for (int j = 0; j < 30; j++)
				if (sc->_state.regs[j])
					r += Common::String::format(" %d=%d", j, sc->_state.regs[j]);
			logf("script %d:%s", i, r.c_str());
		}
	} else if (c == "flag") {
		logf("flag %d = %d", x, _vm->state()->_gameFlag[x & 0xff]);
	} else if (c == "save") {
		_vm->saveGame(x, Common::String::format("guide %d", x));
		logf("saved slot %d", x);
	} else if (c == "load") {
		_vm->loadGame(x);
		logf("loaded slot %d", x);
	} else if (c == "quit") {
		_vm->quitGame();
	} else if (parseLine(line, st)) {
		const bool queued = _state != kStopped && _remaining > 0;
		_steps.insert_at(MIN<int>(_current + (queued ? _remaining : 0), _steps.size()), st);
		_remaining = queued ? _remaining + 1 : 1;
		if (!queued)
			_state = kWaitIdle;
	}
}

void ToonGuide::pollControl() {
	if (_controlPath.empty())
		return;
	Common::File f;
	if (!f.open(Common::FSNode(Common::Path(_controlPath, '/'))))
		return;
	if (f.size() < (int64)_controlOffset)
		_controlOffset = 0;
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

} // End of namespace Toon
