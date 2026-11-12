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

#include "common/algorithm.h"
#include "common/config-manager.h"
#include "common/debug.h"
#include "common/file.h"
#include "common/fs.h"
#include "common/system.h"
#include "common/ustr.h"

#include "engines/stark/guide.h"
#include "engines/stark/scene.h"
#include "engines/stark/stark.h"
#include "engines/stark/gfx/driver.h"
#include "engines/stark/gfx/renderentry.h"
#include "engines/stark/resources/item.h"
#include "engines/stark/resources/knowledge.h"
#include "engines/stark/resources/knowledgeset.h"
#include "engines/stark/resources/level.h"
#include "engines/stark/resources/location.h"
#include "engines/stark/resources/pattable.h"
#include "engines/stark/resources/script.h"
#include "engines/stark/resources/speech.h"
#include "engines/stark/services/dialogplayer.h"
#include "engines/stark/services/gameinterface.h"
#include "engines/stark/services/global.h"
#include "engines/stark/services/resourceprovider.h"
#include "engines/stark/services/services.h"
#include "engines/stark/services/userinterface.h"
#include "engines/stark/tools/decompiler.h"
#include "engines/stark/ui/cursor.h"
#include "engines/stark/ui/world/actionmenu.h"
#include "engines/stark/ui/world/clicktext.h"
#include "engines/stark/ui/world/dialogpanel.h"
#include "engines/stark/ui/world/gamescreen.h"
#include "engines/stark/ui/world/gamewindow.h"
#include "engines/stark/ui/world/inventorywindow.h"

namespace Stark {

Guide *g_guide = nullptr;

// the game's text is Windows-1252; logs and the walkthrough are UTF-8
static Common::String toUtf8(const Common::String &s) {
	return Common::U32String(s, Common::kWindows1252).encode();
}

// lower case and trimmed (titles have stray spaces: "Escaleras ")
static Common::String lower(const Common::String &s) {
	Common::U32String u = Common::U32String(s, Common::kUtf8);
	u.toLowercase();
	Common::String r = u.encode();
	r.trim();
	return r;
}

static const uint32 kSettle = 700;       // ms a step must have been running before it counts as done
static const uint32 kIdleSettle = 600;   // ms the game must stay idle before the next step
static const uint32 kMissing = 30000;    // ms to wait for a step's hotspot or line to show up
static const uint32 kGlide = 350;        // ms the cursor takes to a hotspot
static const uint32 kHover = 300;        // ms on a hotspot (its title shows) before the click
static const uint32 kMenu = 350;         // ms the action menu is up before its button is clicked

Guide::Guide() : _current(0), _remaining(0), _state(kStopped), _stateSince(0), _idleSince(0), _calmSince(0),
	  _missingSince(0), _saveSections(0), _sectionSaved(-1), _pendingSave(-1), _pendingLoad(-1), _controlOffset(0),
	  _retries(0), _speed(1), _ui(kUiNone), _uiAt(0), _uiAction(-1), _uiInInventory(false), _uiHand(false), _uiHoldOnly(false), _uiLine(-1), _uiAtPoint(-1, -1),
	  _glideStart(0), _glideTime(0) {
	if (ConfMan.hasKey("guide_file"))
		load(ConfMan.get("guide_file"));
	if (ConfMan.hasKey("guide_control"))
		_controlPath = ConfMan.get("guide_control");
	if (ConfMan.hasKey("guide_save_sections"))
		_saveSections = ConfMan.getInt("guide_save_sections");
	if (ConfMan.hasKey("guide_step"))
		setCurrent(ConfMan.getInt("guide_step"));
	if (ConfMan.hasKey("guide_speed"))
		_speed = MAX(1, ConfMan.getInt("guide_speed"));
	if (ConfMan.hasKey("guide_play") && ConfMan.getBool("guide_play"))
		_state = kWaitIdle;
	_uiTarget.item = _uiItem.item = nullptr;
}

void Guide::logf(const char *fmt, ...) {
	va_list va;
	va_start(va, fmt);
	Common::String s = Common::String::vformat(fmt, va);
	va_end(va);
	debug("guide: %s", s.c_str());
}

bool Guide::parseLine(const Common::String &text, Step &st) const {
	Common::String line = text;
	line.trim();
	st.onlyLoc.clear();
	st.optional = false;
	// "!STEP": starts while an action's script still runs in the background (the duck deflating on the clamp)
	st.eager = line.hasPrefix("!");
	if (st.eager) {
		line = line.substr(1);
		line.trim();
	}
	if (line.hasPrefix("@")) {
		size_t sp = line.findFirstOf(' ');
		if (sp == Common::String::npos)
			return false;
		st.onlyLoc = line.substr(1, sp - 1);
		line = line.substr(sp + 1);
		line.trim();
	}
	if (line.empty() || !strchr("lutoihLTcCgwW", line[0]) || (line.size() > 1 && line[1] != ' '))
		return false;
	st.kind = line[0];
	if (st.kind == 'C') {
		st.kind = 'c';
		st.optional = true;
	}
	st.arg = line.size() > 2 ? line.substr(2) : Common::String();
	// "STEP => COND": what must hold once the step is done (else it is done again, twice at most)
	st.expect.clear();
	size_t arrow = st.arg.find(" => ");
	if (arrow != Common::String::npos) {
		st.expect = st.arg.substr(arrow + 4);
		st.expect.trim();
		st.arg = st.arg.substr(0, arrow);
	}
	st.arg.trim();
	return !st.arg.empty();
}

bool Guide::load(const Common::String &path) {
	Common::File f;
	if (!f.open(Common::FSNode(Common::Path(path, '/')))) {
		logf("cannot open %s", path.c_str());
		return false;
	}
	_steps.clear();
	Common::String section;
	while (!f.eos()) {
		Common::String line = f.readLine();
		// comments: "#" at the start of a line, or " ;" after a step
		size_t semi = line.find(" ;");
		if (semi != Common::String::npos)
			line = line.substr(0, semi);
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

void Guide::skipStep() {
	_missingSince = 0;
	setCurrent(_current + 1);
	if (_remaining > 0 && --_remaining == 0)
		_state = kStopped;
}

void Guide::setCurrent(int step) {
	_current = CLIP<int>(step, 0, _steps.size());
	_retries = 0;
	ConfMan.setInt("guide_step", _current);
}

void Guide::save(int slot, const Common::String &desc) {
	g_engine->saveGameState(slot, desc, false);
	logf("saving slot %d: %s", slot, desc.c_str());
}

Common::String Guide::locationName() {
	Current *cur = StarkGlobal->getCurrent();
	if (!cur || !cur->getLocation() || !cur->getLevel())
		return "";
	return Common::String::format("%02x/%02x %s", cur->getLevel()->getIndex(), cur->getLocation()->getIndex(),
		toUtf8(cur->getLocation()->getName()).c_str());
}

// the resources whose state says whether the game is busy: the game's, the level's and the location's
static void listBusyResources(Common::Array<Resources::Script *> &scripts, Common::Array<Resources::Speech *> &speeches) {
	static Resources::Location *cachedLoc = nullptr;
	static Common::Array<Resources::Script *> cachedScripts;
	static Common::Array<Resources::Speech *> cachedSpeeches;
	Current *cur = StarkGlobal->getCurrent();
	if (cur->getLocation() != cachedLoc) {
		cachedLoc = cur->getLocation();
		cachedScripts.clear();
		cachedSpeeches.clear();
		Resources::Object *roots[3] = { StarkGlobal->getLevel(), cur->getLevel(), cur->getLocation() };
		for (int i = 0; i < 3; i++) {
			cachedScripts.push_back(roots[i]->listChildrenRecursive<Resources::Script>());
			cachedSpeeches.push_back(roots[i]->listChildrenRecursive<Resources::Speech>());
		}
	}
	scripts = cachedScripts;
	speeches = cachedSpeeches;
}

Common::String Guide::busy() {
	if (!StarkGlobal->getLevel() || !StarkGlobal->getCurrent() || !StarkGlobal->getCurrent()->getLocation())
		return "nolocation";
	if (!StarkUserInterface->isInGameScreen())
		return "screen";
	if (StarkResourceProvider->hasLocationChangeRequest())
		return "location";
	if (_ui != kUiNone && _ui != kUiDone)
		return "step";
	Common::Array<Resources::Script *> scripts;
	Common::Array<Resources::Speech *> speeches;
	listBusyResources(scripts, speeches);
	for (uint i = 0; i < speeches.size(); i++)
		if (speeches[i]->isPlaying())
			return "talking";
	if (StarkDialogPlayer->areOptionsAvailable())
		return "choice";
	if (StarkDialogPlayer->isRunning())
		return "dialog";
	if (!StarkUserInterface->isInteractive())
		return "cutscene";
	// an action's script still running (the locations' own scripts can run for as long as April is there: thunder)
	for (uint i = 0; i < scripts.size(); i++) {
		Resources::Script *s = scripts[i];
		if (!s->isOnBegin() && !s->isOnEnd() && s->getScriptType() == Resources::Script::kScriptTypeOnPlayerAction)
			return "script";
	}
	if (StarkGameInterface->isAprilWalking())
		return "walking";
	return "";
}

Common::Array<Common::String> Guide::choices() {
	Common::Array<Common::String> list;
	if (!StarkDialogPlayer->areOptionsAvailable())
		return list;
	Common::Array<DialogPlayer::Option> options = StarkDialogPlayer->listOptions();
	for (uint i = 0; i < options.size(); i++)
		list.push_back(toUtf8(options[i]._caption));
	return list;
}

int Guide::choiceWith(const Common::String &text) {
	Common::Array<Common::String> list = choices();
	const int num = atoi(text.c_str());
	if (num > 0 && Common::String::format("%d", num) == text)
		return num <= (int)list.size() ? num : 0;
	Common::String want = lower(text);
	for (uint i = 0; i < list.size(); i++)
		if (lower(list[i]).contains(want))
			return i + 1;
	return 0;
}

// the game window's position on the screen (in 640x480 coordinates) and the screen point of one of its points
static Common::Point windowOrigin() {
	return Common::Point(0, Gfx::Driver::kTopBorderHeight);
}

static Common::Point toCurrent(const Common::Point &original) {
	Common::Rect vp = StarkGfx->getScreenViewport();
	return Common::Point(vp.left + original.x * vp.width() / Gfx::Driver::kOriginalWidth,
		vp.top + original.y * vp.height() / Gfx::Driver::kOriginalHeight);
}

// a point of the game window over the item's hotspot (as the game picks what is under the cursor), near the
// middle of it; (-1, -1) when it isn't on the screen
Common::Point Guide::screenPointFor(Resources::ItemVisual *item, int hotspot, int action) {
	GameWindow *gw = StarkUserInterface->getGameScreen()->getGameWindow();
	const Gfx::RenderEntryArray &entries = gw->_renderEntries;
	Common::Array<Common::Point> hits;
	const int step = 6;
	for (int y = step / 2; y < Gfx::Driver::kGameViewportHeight; y += step) {
		for (int x = step / 2; x < Gfx::Driver::kGameViewportWidth; x += step) {
			Common::Point pos(x, y), rel;
			Math::Ray ray = StarkScene->makeRayFromMouse(toCurrent(pos + windowOrigin()));
			Resources::ItemVisual *owner = nullptr;
			for (int i = entries.size() - 1; i >= 0; i--) {
				if (entries[i]->containsPoint(pos, rel, Common::Rect()) || entries[i]->intersectRay(ray)) {
					owner = entries[i]->getOwner();
					break;
				}
			}
			if (owner != item || item->getHotspotIndexForPoint(rel) != hotspot)
				continue;
			if (action >= 0 && !StarkGameInterface->itemHasActionAt(item, rel, action))
				continue;
			hits.push_back(pos);
		}
	}
	if (hits.empty())
		return Common::Point(-1, -1);
	int32 sx = 0, sy = 0;
	for (uint i = 0; i < hits.size(); i++) {
		sx += hits[i].x;
		sy += hits[i].y;
	}
	Common::Point mid(sx / (int32)hits.size(), sy / (int32)hits.size());
	uint best = 0;
	for (uint i = 1; i < hits.size(); i++)
		if (hits[i].sqrDist(mid) < hits[best].sqrDist(mid))
			best = i;
	return hits[best];
}

// the items a step can act on: the location's, and whatever else the game window draws (a robot of the level's)
Common::Array<Resources::ItemVisual *> Guide::sceneItems() {
	Common::Array<Resources::ItemVisual *> items = StarkGlobal->getCurrent()->getLocation()->listChildrenRecursive<Resources::ItemVisual>();
	const Gfx::RenderEntryArray &entries = StarkUserInterface->getGameScreen()->getGameWindow()->_renderEntries;
	for (uint i = 0; i < entries.size(); i++) {
		Resources::ItemVisual *owner = entries[i]->getOwner();
		if (owner && Common::find(items.begin(), items.end(), owner) == items.end())
			items.push_back(owner);
	}
	return items;
}

// "TITLE", "TITLE [n]" (the n-th hotspot with that title) or "=NAME" (by the item's resource name); titles can
// have a '#' of their own (a fuse box's "Enchufe #1")
static void splitSpec(const Common::String &spec, Common::String &want, int &nth, bool &byName) {
	want = spec;
	want.trim();
	nth = 1;
	if (want.hasSuffix("]")) {
		size_t open = want.findLastOf('[');
		if (open != Common::String::npos && open > 0 && Common::isDigit(want[open + 1])) {
			nth = atoi(want.c_str() + open + 1);
			want = want.substr(0, open);
			want.trim();
		}
	}
	byName = want.hasPrefix("=");
	if (byName)
		want = want.substr(1);
	want = lower(want);
}

bool Guide::findTarget(const Common::String &spec, Target &t, bool sceneOnly, int action) {
	Common::String want;
	int nth;
	bool byName;
	splitSpec(spec, want, nth, byName);
	const bool nthGiven = spec.hasSuffix("]");
	Current *cur = StarkGlobal->getCurrent();
	if (!cur || !cur->getLocation())
		return false;
	Common::Array<Resources::ItemVisual *> items = sceneItems();
	// exact titles first, then titles that contain the text
	for (int pass = 0; pass < 2; pass++) {
		Common::Array<Target> found;
		for (uint i = 0; i < items.size(); i++) {
			Resources::ItemVisual *item = items[i];
			if (!item->isEnabled())
				continue;
			Common::Array<Resources::PATTable *> pats = item->listChildren<Resources::PATTable>();
			for (uint h = 0; h < pats.size(); h++) {
				if (pats[h]->listPossibleActions().empty() && pats[h]->getDefaultAction() < 0)
					continue;
				Common::String title = toUtf8(item->getHotspotTitle(h));
				Common::String key = lower(byName ? toUtf8(item->getName()) : title);
				if (pass == 0 ? key != want : !key.contains(want))
					continue;
				// of two hotspots with the same title (a door and the way through it), the one that does it
				if (action >= 0 && !item->canPerformAction(action, h))
					continue;
				Target c;
				c.item = item;
				c.hotspot = h;
				c.title = title;
				c.screen = Common::Point(-1, -1);
				found.push_back(c);
			}
		}
		if (found.empty())
			continue;
		if (nthGiven) {
			if (nth < 1 || nth > (int)found.size())
				return false;
			t = found[nth - 1];
			return true;
		}
		// a close-up has a hotspot of each of its states (the duck deflated, inflated): the one on the screen
		t = found[0];
		if (found.size() > 1)
			for (uint i = 0; i < found.size(); i++)
				if (screenPointFor(found[i].item, found[i].hotspot, action).x >= 0) {
					t = found[i];
					break;
				}
		return true;
	}
	if (!sceneOnly)
		return findInventory(spec, t);
	return false;
}

bool Guide::findInventory(const Common::String &spec, Target &t) {
	Common::String want;
	int nth;
	bool byName;
	splitSpec(spec, want, nth, byName);
	Common::Array<Resources::Item *> inv = StarkGlobal->getInventory()->listChildren<Resources::Item>(Resources::Item::kItemInventory);
	for (int pass = 0; pass < 2; pass++) {
		int seen = 0;
		for (uint i = 0; i < inv.size(); i++) {
			if (!inv[i]->isEnabled())
				continue;
			Resources::ItemVisual *item = Resources::Object::cast<Resources::ItemVisual>(inv[i]);
			Common::String title = toUtf8(StarkGameInterface->getItemTitle(item));
			Common::String key = lower(byName ? toUtf8(item->getName()) : title);
			if (pass == 0 ? key != want : !key.contains(want))
				continue;
			if (++seen != nth)
				continue;
			t.item = item;
			t.hotspot = -1;
			t.title = title;
			t.screen = Common::Point(-1, -1);
			return true;
		}
	}
	return false;
}

static Resources::Knowledge *findKnowledge(const Common::String &name) {
	Current *cur = StarkGlobal->getCurrent();
	Resources::Object *roots[3] = { cur ? cur->getLocation() : nullptr, cur ? cur->getLevel() : nullptr, StarkGlobal->getLevel() };
	for (int i = 0; i < 3; i++) {
		if (!roots[i])
			continue;
		Common::Array<Resources::Knowledge *> list = roots[i]->listChildrenRecursive<Resources::Knowledge>();
		for (uint j = 0; j < list.size(); j++)
			if (list[j]->getName().equalsIgnoreCase(name))
				return list[j];
	}
	return nullptr;
}

bool Guide::cond(const Common::String &expr) {
	Common::String rest = expr;
	while (!rest.empty()) {
		Common::String term;
		size_t a = rest.find(" and ");
		if (a == Common::String::npos) {
			term = rest;
			rest.clear();
		} else {
			term = rest.substr(0, a);
			rest = rest.substr(a + 5);
		}
		term.trim();
		bool neg = term.hasPrefix("not ");
		if (neg)
			term = term.substr(4);
		bool ok = false;
		Target t;
		if (term.hasPrefix("has ")) {
			ok = findInventory(term.substr(4), t);
		} else if (term.hasPrefix("see ")) {
			// a hotspot of that title on the screen (a door that is open)
			ok = findTarget(term.substr(4), t, true) && screenPointFor(t.item, t.hotspot, -1).x >= 0;
		} else if (term.hasPrefix("loc ")) {
			ok = lower(locationName()).contains(lower(term.substr(4)));
		} else if (term.hasPrefix("know ")) {
			Resources::Knowledge *k = findKnowledge(term.substr(5));
			if (k)
				ok = k->getSubType() == Resources::Knowledge::kBoolean || k->getSubType() == Resources::Knowledge::kBooleanWithChild ?
					k->getBooleanValue() : k->getIntegerValue() != 0;
			else
				logf("no knowledge %s", term.substr(5).c_str());
		} else {
			logf("bad condition: %s", term.c_str());
		}
		if (ok == neg)
			return false;
	}
	return true;
}

void Guide::listObjects(bool all) {
	logf("location %s", locationName().c_str());
	Common::Array<Resources::ItemVisual *> items = sceneItems();
	for (uint i = 0; i < items.size(); i++) {
		Resources::ItemVisual *item = items[i];
		if (!item->isEnabled() && !all)
			continue;
		Common::Array<Resources::PATTable *> pats = item->listChildren<Resources::PATTable>();
		for (uint h = 0; h < pats.size(); h++) {
			Resources::ActionArray actions = pats[h]->listPossibleActions();
			if (actions.empty() && pats[h]->getDefaultAction() < 0 && !all)
				continue;
			Common::String acts;
			for (uint a = 0; a < actions.size(); a++)
				acts += Common::String::format(" %d", actions[a]);
			Common::Point p = screenPointFor(item, h, -1);
			logf("  '%s' (=%s #%d) actions%s default %d at %d,%d%s", toUtf8(item->getHotspotTitle(h)).c_str(),
				toUtf8(item->getName()).c_str(), h, acts.c_str(), pats[h]->getDefaultAction(), p.x, p.y,
				item->isEnabled() ? "" : " (disabled)");
		}
	}
	Common::Array<Resources::Item *> inv = StarkGlobal->getInventory()->listChildren<Resources::Item>(Resources::Item::kItemInventory);
	Common::String held;
	for (uint i = 0; i < inv.size(); i++)
		if (inv[i]->isEnabled())
			held += Common::String::format(" '%s'(%d)", toUtf8(StarkGameInterface->getItemTitle(
				Resources::Object::cast<Resources::ItemVisual>(inv[i]))).c_str(), inv[i]->getIndex());
	logf("inventory:%s", held.c_str());
}

static Common::Point mouseOriginal() {
	return StarkGfx->convertCoordinateCurrentToOriginal(StarkUserInterface->getCursor()->getMousePosition(true));
}

static void setMouse(const Common::Point &original) {
	Common::Point p = toCurrent(original);
	g_system->warpMouse(p.x, p.y);
	StarkUserInterface->handleMouseMove(p);
}

void Guide::glideTo(const Common::Point &original, uint32 now) {
	_glideFrom = mouseOriginal();
	_glideTo = original;
	_glideStart = now;
	_glideTime = _speed > 1 ? 0 : kGlide;
}

void Guide::click() {
	StarkUserInterface->handleClick();
	StarkUserInterface->handleMouseUp();
}

// the middle of an item's slot in the inventory window (scrolled to it)
bool Guide::slotPoint(Resources::ItemVisual *item, Common::Point &p) {
	InventoryWindow *inv = StarkUserInterface->getGameScreen()->getInventoryWindow();
	for (uint i = 0; i < inv->_renderEntries.size(); i++) {
		if (inv->_renderEntries[i]->getOwner() != item)
			continue;
		while (!inv->isSlotVisible(i) && inv->canScrollDown())
			inv->scrollDown();
		while (!inv->isSlotVisible(i) && inv->canScrollUp())
			inv->scrollUp();
		Common::Rect r = inv->getItemRect(i, inv->_renderEntries[i]->getImage());
		p = Common::Point((r.left + r.right) / 2, (r.top + r.bottom) / 2) + windowOrigin();
		return true;
	}
	return false;
}

// the action menu is up: the cursor goes to the action's button after a moment
bool Guide::menuAt(ActionMenu *menu, int action, uint32 now) {
	// its buttons: eye, hand and mouth
	int b = -1;
	for (uint i = 0; i < ARRAYSIZE(menu->_buttons); i++)
		if (menu->_buttons[i].enabled && (int)menu->_buttons[i].action == action)
			b = i;
	if (b < 0) {
		logf("no button for action %d", action);
		menu->close();
		return false;
	}
	Common::Rect r = menu->_buttons[b].rect;
	r.translate(menu->_position.left, menu->_position.top);
	_buttonAt = Common::Point((r.left + r.right) / 2, (r.top + r.bottom) / 2);
	_ui = kUiToButton;
	_uiAt = now + (_speed > 1 ? 0 : kMenu);
	return true;
}

bool Guide::startStep(const Step &stIn) {
	GameScreen *gs = StarkUserInterface->getGameScreen();
	InventoryWindow *inv = gs->getInventoryWindow();
	// "STEP @X,Y": the cursor goes to that point of the screen (one of a hotspot's buttons)
	Step st = stIn;
	_uiAtPoint = Common::Point(-1, -1);
	size_t at = st.arg.findLastOf('@');
	if (at != Common::String::npos && at > 0 && st.arg[at - 1] == ' ' && strchr(st.arg.c_str() + at, ',')) {
		int px = -1, py = -1;
		if (sscanf(st.arg.c_str() + at + 1, "%d,%d", &px, &py) == 2) {
			_uiAtPoint = Common::Point(px, py);
			st.arg = st.arg.substr(0, at);
			st.arg.trim();
		}
	}
	_uiTarget.item = _uiItem.item = nullptr;
	_uiInInventory = false;
	_uiHand = false;
	_uiHoldOnly = false;
	_uiAction = -1;
	_uiLine = -1;
	switch (st.kind) {
	case 'l':
	case 'u':
	case 't':
	case 'o':
		_uiAction = st.kind == 'l' ? Resources::PATTable::kActionLook : st.kind == 'u' ? Resources::PATTable::kActionUse :
			st.kind == 't' ? Resources::PATTable::kActionTalk : Resources::PATTable::kActionExit;
		if (!findTarget(st.arg, _uiTarget, true, _uiAction))
			return false;
		break;
	case 'L':
	case 'T':
		if (!findInventory(st.arg, _uiTarget))
			return false;
		_uiInInventory = true;
		_uiAction = st.kind == 'L' ? Resources::PATTable::kActionLook : Resources::PATTable::kActionTalk;
		break;
	case 'i': {
		size_t gt = st.arg.find(" > ");
		if (gt == Common::String::npos)
			return false;
		if (!findInventory(st.arg.substr(0, gt), _uiItem))
			return false;
		if (findTarget(st.arg.substr(gt + 3), _uiTarget, true, _uiItem.item->getIndex())) {
		} else if (findInventory(st.arg.substr(gt + 3), _uiTarget)) {
			_uiInInventory = true;
		} else {
			return false;
		}
		_uiAction = _uiItem.item->getIndex();
		break;
	}
	case 'h':
		// taken in hand and kept there (for a quick use later: the eye on the floor)
		if (!findInventory(st.arg, _uiItem))
			return false;
		_uiHoldOnly = true;
		break;
	case 'c':
		_uiLine = choiceWith(st.arg) - 1;
		if (_uiLine < 0)
			return false;
		break;
	case 'g':
		break;
	default:
		break;
	}
	const uint32 now = g_system->getMillis();
	_uiAt = now;
	if (st.kind == 'c') {
		_ui = kUiToLine;
	} else if (st.kind == 'g') {
		int x = 0, y = 0;
		sscanf(st.arg.c_str(), "%d %d", &x, &y);
		_uiTarget.screen = Common::Point(x, y) - windowOrigin();
		_ui = kUiToTarget;
	} else if (_uiItem.item && !_uiHoldOnly && !_uiInInventory && inv->getSelectedInventoryItem() == (int)_uiItem.item->getIndex()) {
		// already in hand ("h"): one click
		_ui = kUiToTarget;
	} else if (_uiItem.item || _uiInInventory) {
		inv->setSelectedInventoryItem(-1);
		_ui = kUiToInventory;
	} else {
		_ui = kUiToTarget;
	}
	return true;
}

// the clicks of the step running, one phase after another
void Guide::uiTick(uint32 now) {
	if (_glideTime || _glideStart) {
		const uint32 t = now - _glideStart;
		if (t >= _glideTime) {
			setMouse(_glideTo);
			_glideStart = _glideTime = 0;
		} else {
			setMouse(Common::Point(_glideFrom.x + (_glideTo.x - _glideFrom.x) * (int32)t / (int32)_glideTime,
				_glideFrom.y + (_glideTo.y - _glideFrom.y) * (int32)t / (int32)_glideTime));
		}
		return;
	}
	if (_ui == kUiNone || _ui == kUiDone || now < _uiAt)
		return;
	GameScreen *gs = StarkUserInterface->getGameScreen();
	InventoryWindow *inv = gs->getInventoryWindow();
	ActionMenu *menu = inv->_actionMenu;
	const uint32 wait = _speed > 1 ? 0 : kHover;

	switch (_ui) {
	case kUiToInventory:
		// as the right button does; its slots are there once it has been drawn
		if (!inv->isVisible())
			inv->open();
		_ui = kUiToSlot;
		_uiAt = now + (_speed > 1 ? 50 : 400);
		break;
	case kUiToSlot: {
		Common::Point p;
		if (!slotPoint(_uiItem.item ? _uiItem.item : _uiTarget.item, p)) {
			logf("not in the inventory window: %s", (_uiItem.item ? _uiItem : _uiTarget).title.c_str());
			_ui = kUiDone;
			break;
		}
		glideTo(p, now);
		_ui = kUiPickItem;
		_uiAt = now + wait;
		break;
	}
	case kUiPickItem:
		click();
		if (menu->isVisible()) {
			// the item's action menu: the hand takes it, the eye looks at it, the mouth talks to it
			_uiHand = _uiItem.item != nullptr;
			if (!menuAt(menu, _uiHand ? (int)Resources::PATTable::kActionUse : _uiAction, now))
				_ui = kUiDone;
			break;
		}
		_ui = _uiItem.item && !_uiHoldOnly ? kUiToTarget : kUiDone;
		if (_uiHoldOnly)
			inv->close();
		_uiAt = now + wait;
		break;
	case kUiToTarget: {
		if (_uiItem.item && inv->getSelectedInventoryItem() != (int)_uiItem.item->getIndex()) {
			logf("%s isn't in hand", _uiItem.title.c_str());
			_ui = kUiDone;
			break;
		}
		if (_uiItem.item && _uiInInventory) {
			// the other inventory item
			Common::Point p;
			if (!slotPoint(_uiTarget.item, p)) {
				logf("not in the inventory window: %s", _uiTarget.title.c_str());
				_ui = kUiDone;
				break;
			}
			glideTo(p, now);
			_ui = kUiClickTarget;
			_uiAt = now + wait;
			break;
		}
		if (_uiItem.item && inv->isVisible())
			inv->close();
		if (_uiAtPoint.x >= 0)
			_uiTarget.screen = _uiAtPoint - windowOrigin();
		else if (_uiTarget.item)
			_uiTarget.screen = screenPointFor(_uiTarget.item, _uiTarget.hotspot, _uiAction);
		if (_uiTarget.screen.x < 0) {
			// not on the screen: done without the cursor
			logf("off screen: %s", _uiTarget.title.c_str());
			_uiTarget.item->doAction(_uiAction, _uiTarget.hotspot);
			_ui = kUiDone;
			break;
		}
		glideTo(_uiTarget.screen + windowOrigin(), now);
		_ui = kUiClickTarget;
		_uiAt = now + wait;
		break;
	}
	case kUiClickTarget: {
		if (!_uiInInventory && _uiTarget.item) {
			// a hotspot whose click does something else (an exit's walk): done without the cursor
			Resources::PATTable *pat = _uiTarget.item->findChildWithOrder<Resources::PATTable>(_uiTarget.hotspot);
			const int def = pat ? pat->getDefaultAction() : -1;
			const bool stock = _uiAction == Resources::PATTable::kActionUse || _uiAction == Resources::PATTable::kActionLook ||
				_uiAction == Resources::PATTable::kActionTalk || _uiItem.item;
			if ((def >= 0 && def != _uiAction) || (def < 0 && !stock)) {
				_uiTarget.item->doAction(_uiAction, _uiTarget.hotspot);
				_ui = kUiDone;
				break;
			}
		}
		click();
		if (menu->isVisible() && !_uiItem.item) {
			if (!menuAt(menu, _uiAction, now))
				_ui = kUiDone;
			break;
		}
		_ui = kUiDone;
		break;
	}
	case kUiToButton:
		glideTo(_buttonAt, now);
		_ui = kUiClickButton;
		_uiAt = now + wait;
		break;
	case kUiClickButton:
		click();
		if (_uiHand && _uiHoldOnly) {
			_uiHand = false;
			inv->close();
			_ui = kUiDone;
			break;
		}
		if (_uiHand) {
			// the item is in hand now: on to the target
			_uiHand = false;
			_uiAction = _uiItem.item->getIndex();
			_ui = kUiToTarget;
			_uiAt = now + wait;
			break;
		}
		_ui = kUiDone;
		break;
	case kUiToLine: {
		DialogPanel *panel = gs->getDialogPanel();
		if (_uiLine >= (int)panel->_options.size()) {
			_ui = kUiNone;
			break;
		}
		// a line out of sight: the arrow scrolls the lines (they are laid out when drawn)
		const bool down = _uiLine > (int)panel->_lastVisibleOption && panel->_scrollDownArrowVisible;
		const bool up = _uiLine < (int)panel->_firstVisibleOption && panel->_scrollUpArrowVisible;
		if (down || up) {
			Common::Rect r = down ? panel->_scrollDownArrowRect : panel->_scrollUpArrowRect;
			r.translate(panel->_position.left, panel->_position.top);
			glideTo(Common::Point((r.left + r.right) / 2, (r.top + r.bottom) / 2), now);
			_ui = kUiClickArrow;
			_uiAt = now + wait;
			break;
		}
		ClickText *text = panel->_options[_uiLine];
		Common::Rect r = text->_bbox;
		r.translate(text->_position.x + panel->_position.left, text->_position.y + panel->_position.top);
		glideTo(Common::Point(MIN<int>(r.left + 40, (r.left + r.right) / 2), (r.top + r.bottom) / 2), now);
		_ui = kUiClickLine;
		_uiAt = now + (_speed > 1 ? 0 : 500);
		break;
	}
	case kUiClickArrow:
		click();
		_ui = kUiToLine;
		_uiAt = now + (_speed > 1 ? 50 : 300);
		break;
	case kUiClickLine:
		click();
		_ui = kUiDone;
		break;
	default:
		break;
	}
}

void Guide::onSpeech(Resources::Speech *speech) {
	Common::String name = "?";
	const int32 id = speech->getCharacterId();
	Resources::KnowledgeSet *persons = StarkGlobal->getLevel() ?
		StarkGlobal->getLevel()->findChildWithSubtype<Resources::KnowledgeSet>(Resources::KnowledgeSet::kPersons) : nullptr;
	Resources::Knowledge *who = persons && id >= 0 ? persons->findChildWithIndex<Resources::Knowledge>(id) : nullptr;
	if (who)
		name = who->getName();
	logf("talk %s: %s", toUtf8(name).c_str(), toUtf8(speech->getPhrase()).c_str());
}

void Guide::tick() {
	const uint32 now = g_system->getMillis();
	pollControl();

	// testing at speed: speech and full screen movies are skipped
	if (_speed > 1) {
		StarkUserInterface->skipFMV();
		if (StarkGlobal->getCurrent())
			StarkGameInterface->skipCurrentSpeeches();
	}

	const Common::String loc = locationName();
	if (loc != _lastLoc) {
		logf("location %s", loc.c_str());
		_lastLoc = loc;
	}

	if (_pendingLoad >= 0) {
		logf("loading slot %d", _pendingLoad);
		g_engine->loadGameState(_pendingLoad);
		_pendingLoad = -1;
		_idleSince = 0;
		_ui = kUiNone;
		return;
	}

	uiTick(now);

	Common::String why = busy();
	// an eager step doesn't wait for the scripts of the steps before it
	const int next = _state == kWaitIdle ? _current : _current + 1;
	if (why == "script" && _state != kStopped && next < (int)_steps.size() && _steps[next].eager)
		why.clear();
	const bool idle = why.empty();
	const bool choice = why == "choice";
	if (idle || choice) {
		if (!_idleSince)
			_idleSince = now;
	} else {
		_idleSince = 0;
	}
	// the dialog's lines, once each time they change
	if (choice) {
		Common::Array<Common::String> list = choices();
		Common::String all;
		for (uint i = 0; i < list.size(); i++)
			all += list[i] + "\n";
		if (all != _lastChoices) {
			_lastChoices = all;
			for (uint i = 0; i < list.size(); i++)
				logf("line %d: %s", i + 1, list[i].c_str());
		}
	} else {
		_lastChoices.clear();
	}

	if (idle) {
		if (!_calmSince)
			_calmSince = now;
	} else {
		_calmSince = 0;
	}
	if (_pendingSave >= 0 && _calmSince && now - _calmSince >= kIdleSettle) {
		save(_pendingSave, Common::String::format("guide %d", _pendingSave));
		_pendingSave = -1;
		return;
	}

	if (_state == kStopped)
		return;
	if (_state != kWaitIdle && _current >= (int)_steps.size()) {
		_state = kStopped;
		return;
	}

	if (_state == kRunning) {
		if (now - _stateSince < kSettle || why == "step")
			return;
		_ui = kUiNone;
		_state = kSettle;
		_stateSince = now;
	}
	if (_state == kSettle) {
		if (!_idleSince || now - _idleSince < kIdleSettle)
			return;
		// whatever is left in hand after an inventory item's use goes back (as the right button does)
		InventoryWindow *inv = StarkUserInterface->getGameScreen()->getInventoryWindow();
		if (_steps[_current].kind == 'i' && inv->getSelectedInventoryItem() != -1)
			inv->setSelectedInventoryItem(-1);
		if (inv->isVisible() && !choice)
			inv->close();
		if (!_steps[_current].expect.empty() && !cond(_steps[_current].expect)) {
			if (++_retries <= 4) {
				logf("step %d: not (%s), again", _current + 1, _steps[_current].expect.c_str());
				_state = kWaitIdle;
				return;
			}
			logf("step %d: still not (%s), stopping", _current + 1, _steps[_current].expect.c_str());
			_state = kStopped;
			return;
		}
		// an exit that only starts a scene (a step with a COND of its own) needn't take April anywhere
		if (_steps[_current].kind == 'o' && loc == _stepLoc && !choice && _steps[_current].expect.empty()) {
			if (++_retries <= 2) {
				logf("step %d: still in %s, again", _current + 1, loc.c_str());
				_state = kWaitIdle;
				return;
			}
			logf("step %d: never left %s, stopping", _current + 1, loc.c_str());
			_state = kStopped;
			return;
		}
		_retries = 0;
		logf("step %d done (%s)", _current + 1, loc.c_str());
		setCurrent(_current + 1);
		_state = kWaitIdle;
		if (_remaining > 0 && --_remaining == 0) {
			_state = kStopped;
			return;
		}
	}

	// kWaitIdle: the next step, once the game waits for the player
	if (_current >= (int)_steps.size()) {
		logf("walkthrough finished");
		_state = kStopped;
		return;
	}
	const Step &st = _steps[_current];
	const bool wantChoice = st.kind == 'c';
	if (!_idleSince || now - _idleSince < kIdleSettle)
		return;
	if (st.kind == 'w' || st.kind == 'W') {
		// waits don't need the game to wait for the player, only to be past what came before
		if (st.kind == 'w') {
			if (!_missingSince)
				_missingSince = now;
			if (now - _missingSince < (uint32)(atof(st.arg.c_str()) * 1000 / _speed))
				return;
		} else if (!cond(st.arg)) {
			return;
		}
		_missingSince = 0;
		logf("step %d: %c %s", _current + 1, st.kind, st.arg.c_str());
		logf("step %d done (%s)", _current + 1, loc.c_str());
		setCurrent(_current + 1);
		if (_remaining > 0 && --_remaining == 0)
			_state = kStopped;
		return;
	}
	if (wantChoice != choice) {
		if (st.optional && idle && now - _idleSince > 3000) {
			logf("step %d: no dialog, skipped", _current + 1);
			skipStep();
			return;
		}
		if (!_missingSince)
			_missingSince = now;
		if (now - _missingSince > kMissing) {
			logf("step %d: %s, stopping", _current + 1, wantChoice ? "no dialog" : "a dialog is open");
			_state = kStopped;
			_missingSince = 0;
		}
		return;
	}
	if (!st.section.empty() && _sectionSaved != _current) {
		_sectionSaved = _current;
		int num = 0;
		for (int i = 0; i <= _current; i++)
			if (!_steps[i].section.empty())
				num++;
		logf("section %d at %u ms, step %d: %s", num, now, _current + 1, st.section.c_str());
		if (_saveSections && !choice && StarkUserInterface->isInteractive()) {
			save(_saveSections + num - 1, st.section);
			return;
		}
	}
	if (!st.onlyLoc.empty() && !lower(loc).contains(lower(st.onlyLoc))) {
		logf("step %d: not in %s, skipped", _current + 1, st.onlyLoc.c_str());
		skipStep();
		return;
	}
	if (!startStep(st)) {
		if (!_missingSince)
			_missingSince = now;
		if (st.optional && now - _missingSince > 1500) {
			logf("step %d: no such line, skipped", _current + 1);
			_missingSince = 0;
			skipStep();
		} else if (now - _missingSince > kMissing) {
			logf("step %d: no '%s' here, stopping", _current + 1, st.arg.c_str());
			_missingSince = 0;
			_state = kStopped;
		}
		return;
	}
	_missingSince = 0;
	logf("step %d: %c %s", _current + 1, st.kind, st.arg.c_str());
	_stepLoc = loc;
	_state = kRunning;
	_stateSince = now;
}

void Guide::runCommand(const Common::String &text) {
	Common::String line = text;
	line.trim();
	if (line.empty())
		return;
	logf("control: %s", line.c_str());
	size_t sp = line.findFirstOf(' ');
	Common::String c = line.substr(0, sp), rest = sp == Common::String::npos ? "" : line.substr(sp + 1);
	rest.trim();
	const int x = atoi(rest.c_str());
	Step st;
	if (c == "play") {
		_remaining = 0;
		_state = kWaitIdle;
		logf("play from step %d", _current + 1);
	} else if (c == "stop") {
		_state = kStopped;
		_ui = kUiNone;
	} else if (c == "goto") {
		if (_state == kRunning || _state == kSettle)
			_state = kStopped;
		_ui = kUiNone;
		setCurrent(x > 0 ? x - 1 : 0);
		_sectionSaved = _current;   // no save for the section it starts in
		logf("current step %d", _current + 1);
	} else if (c == "reload") {
		if (_state == kRunning || _state == kSettle)
			_state = kStopped;
		_ui = kUiNone;
		if (ConfMan.hasKey("guide_file"))
			load(ConfMan.get("guide_file"));
		setCurrent(_current);
	} else if (c == "objects") {
		// "objects all": the disabled items and the hotspots without actions too
		listObjects(rest == "all");
	} else if (c == "state") {
		logf("%s, busy '%s', step %d, state %d", locationName().c_str(), busy().c_str(), _current + 1, _state);
		Common::Array<Common::String> list = choices();
		for (uint i = 0; i < list.size(); i++)
			logf("line %d: %s", i + 1, list[i].c_str());
	} else if (c == "items") {
		// every item of the location, whatever it is (for finding a hotspot the walkthrough can't see)
		Common::Array<Resources::Item *> items = StarkGlobal->getCurrent()->getLocation()->listChildrenRecursive<Resources::Item>();
		for (uint i = 0; i < items.size(); i++)
			logf("item %d '%s' subtype %d enabled %d pats %d", items[i]->getIndex(), toUtf8(items[i]->getName()).c_str(),
				items[i]->getSubType(), items[i]->isEnabled(), items[i]->listChildren<Resources::PATTable>().size());
	} else if (c == "knows") {
		// the knowledges whose name has the text (spaces aside), with their values (for writing COND)
		Common::String want = lower(rest);
		Common::replace(want, " ", "");
		Current *cur = StarkGlobal->getCurrent();
		Resources::Object *roots[3] = { cur ? cur->getLocation() : nullptr, cur ? cur->getLevel() : nullptr, StarkGlobal->getLevel() };
		for (int i = 0; i < 3; i++) {
			if (!roots[i])
				continue;
			Common::Array<Resources::Knowledge *> list = roots[i]->listChildrenRecursive<Resources::Knowledge>();
			for (uint j = 0; j < list.size(); j++) {
				Common::String name = lower(toUtf8(list[j]->getName()));
				Common::String bare = name;
				Common::replace(bare, " ", "");
				if (bare.contains(want))
					logf("knowledge '%s' type %d = %d", toUtf8(list[j]->getName()).c_str(), list[j]->getSubType(),
						list[j]->getSubType() == Resources::Knowledge::kBoolean || list[j]->getSubType() == Resources::Knowledge::kBooleanWithChild ?
						list[j]->getBooleanValue() : list[j]->getIntegerValue());
			}
		}
	} else if (c == "cond") {
		logf("= %d", cond(rest));
	} else if (c == "scripts") {
		// the scripts not at their start (for finding what a stuck step waits for)
		Common::Array<Resources::Script *> scripts;
		Common::Array<Resources::Speech *> speeches;
		listBusyResources(scripts, speeches);
		for (uint i = 0; i < scripts.size(); i++)
			if (!scripts[i]->isOnBegin())
				logf("script '%s' type %d event %d", toUtf8(scripts[i]->getName()).c_str(), scripts[i]->getScriptType(),
					scripts[i]->getRunEvent());
	} else if (c == "at") {
		// what the game finds under a point of the screen (640x480)
		int mx = 0, my = 0;
		sscanf(rest.c_str(), "%d %d", &mx, &my);
		GameWindow *gw = StarkUserInterface->getGameScreen()->getGameWindow();
		Common::Point pos = Common::Point(mx, my) - windowOrigin(), rel;
		Math::Ray ray = StarkScene->makeRayFromMouse(toCurrent(Common::Point(mx, my)));
		for (int i = gw->_renderEntries.size() - 1; i >= 0; i--) {
			const bool in = gw->_renderEntries[i]->containsPoint(pos, rel, Common::Rect());
			const bool hit = gw->_renderEntries[i]->intersectRay(ray);
			Resources::ItemVisual *o = gw->_renderEntries[i]->getOwner();
			if (in || hit)
				logf("entry %d '%s' in %d ray %d rel %d,%d hotspot %d", i, o ? toUtf8(o->getName()).c_str() : "-", in, hit,
					rel.x, rel.y, o ? o->getHotspotIndexForPoint(rel) : -9);
		}
		Common::Array<Common::Point> exits = StarkGameInterface->listExitPositions();
		for (uint i = 0; i < exits.size(); i++)
			logf("exit at %d,%d", exits[i].x, exits[i].y);
	} else if (c == "decompile") {
		// the scripts of a hotspot's actions, or "=NAME" the scripts of that name (for writing a walkthrough)
		Common::Array<Resources::Script *> list;
		Common::Array<Common::String> heads;
		Target t;
		if (rest.hasPrefix("=")) {
			Common::Array<Resources::Script *> all;
			Common::Array<Resources::Speech *> speeches;
			listBusyResources(all, speeches);
			for (uint i = 0; i < all.size(); i++)
				if (lower(toUtf8(all[i]->getName())).contains(lower(rest.substr(1)))) {
					list.push_back(all[i]);
					heads.push_back(toUtf8(all[i]->getName()));
				}
		} else if (findTarget(rest, t, false)) {
			Resources::PATTable *pat = t.hotspot >= 0 ? t.item->findChildWithOrder<Resources::PATTable>(t.hotspot) :
				t.item->findChild<Resources::PATTable>(false);
			if (pat)
				for (Resources::PATTable::EntryMap::iterator it = pat->_itemEntries.begin(); it != pat->_itemEntries.end(); ++it)
					if (it->_value._script) {
						list.push_back(it->_value._script);
						heads.push_back(Common::String::format("action %d: %s", it->_key, toUtf8(it->_value._script->getName()).c_str()));
					}
		}
		for (uint i = 0; i < list.size(); i++) {
			logf("script %s", heads[i].c_str());
			Tools::Decompiler decompiler(list[i]);
			if (!decompiler.getError().empty())
				logf("decompilation failure: %s", decompiler.getError().c_str());
			decompiler.printDecompiled();
		}
	} else if (c == "mouse") {
		int mx = 0, my = 0;
		sscanf(rest.c_str(), "%d %d", &mx, &my);
		setMouse(Common::Point(mx, my));
	} else if (c == "click") {
		click();
	} else if (c == "skip") {
		StarkGameInterface->skipCurrentSpeeches();
		StarkUserInterface->skipFMV();
	} else if (c == "newgame") {
		// what the main menu's new game does
		StarkUserInterface->changeScreen(Screen::kScreenGame);
		StarkResourceProvider->initGlobal();
		StarkGlobal->setCurrentChapter(0);
		StarkResourceProvider->requestLocationChange(0x45, 0x00);
	} else if (c == "save") {
		_pendingSave = x;
	} else if (c == "load") {
		_pendingLoad = x;
	} else if (c == "speed") {
		_speed = MAX(1, x);
	} else if (c == "quit") {
		g_engine->quitGame();
	} else if (parseLine(line, st)) {
		const bool inFlight = _state == kRunning || _state == kSettle;
		if (_state != kStopped && _remaining == 0) {
			// while playing: the next step, and play goes on
			_steps.insert_at(MIN<int>(_current + (inFlight ? 1 : 0), _steps.size()), st);
		} else {
			// one step now (after those already queued), as a walkthrough of its own
			const bool queued = _state != kStopped && _remaining > 0;
			_steps.insert_at(MIN<int>(_current + (queued ? _remaining : 0), _steps.size()), st);
			_remaining = queued ? _remaining + 1 : 1;
			if (!queued)
				_state = kWaitIdle;
		}
	} else {
		logf("unknown command");
	}
}

void Guide::pollControl() {
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

} // End of namespace Stark
