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

#ifndef TOON_GUIDE_H
#define TOON_GUIDE_H

#include "common/array.h"
#include "common/rect.h"
#include "common/str.h"
#include "common/system.h"

namespace Toon {

class ToonEngine;

/**
 * The walkthrough player for Toonstruck: a walkthrough is the list of things a player does, played back with the
 * mouse the way a player would, one at a time when Drew is waiting.
 *
 * File format, one entry per line (a hotspot is the id the info line names it by, as "objects" lists them: a room
 * text id, 2000 + a location for the exits on the map, -3 for Flux, -4 for Drew):
 *   == Section title
 *   e <hotspot> [; comment]         a left click on a hotspot (walk there, use, talk)
 *   E <hotspot> [; comment]         the same as soon as the player has control, even while Drew walks or someone talks
 *   l <hotspot> [; comment]         a right click on it (look)
 *   u <item> <hotspot> [; comment]  take an item from the bag and click it on a hotspot
 *   p <item> <x> <y> [; comment]   the same on a point of the screen (one of several hotspots with the same name)
 *   t <item> <item> [; comment]     in the bag, take an item and click it on another one
 *   T <item> [; comment]            the item in Drew's hand on an item of the bag (one that cannot go in the bag)
 *   i <item> [; comment]            in the bag, a right click on an item (Drew says what it is, or uses it)
 *   k <n> [; comment]               a conversation icon, the n-th from the left (0 is the first)
 *   G <x> <y> [; comment]           the same as soon as the player has control
 *   C [; comment]                   play the clown door's Simon game until it opens
 *   A [; comment]                   play Fingers' WACMAN game in the arcade until it is over
 *   c <slot> [dx] [; comment]       a left click on the middle of a scene animation (a crate that moves; dx pixels
 *                                   right of it, a little lower), as soon as the player has control
 *   P <x> <y> [; comment]           a press there right away, whatever the game is doing (to stop a machine)
 *   d <hotspot> <hotspot> [; comment] two left clicks one right after the other (two phone buttons at once)
 *   g <x> <y> [; comment]           a left click on the screen (walk there)
 *   n <flag> <value> [; comment]    wait until a game flag has that value
 *   v <global> <value> [; comment]  wait until a global of the game scripts has that value
 *   V <global> <value> [; comment]  wait until it reaches that value or more
 *   f <slot> <frame> [; comment]    wait until a scene animation (as "anims" lists them) shows that frame; -1: until
 *                                   it plays at all
 *   h <item> [; comment]            wait until Drew holds that item
 *   a <scene> [; comment]           wait until the game is in that scene
 *   w <seconds> [; comment]         wait
 *   W <ms> [; comment]              wait that many milliseconds
 *   J <item> <n> [; comment]        if Drew has the item, skip the next n steps
 *   R <global> <value> [; comment]  as V, the value moved by -1, 0, +1 on each loop of a j (a moving target)
 *   q <scene> <n> [; comment]       until Drew is in that scene, back n steps (a door open only now and then)
 *   H <line> <n> [; comment]        until that dialogue line is said, back n steps; once said, the loop's other steps
 *                                   are skipped (rocking the pot until it tips over)
 *   j <item> <n> [; comment]        until Drew has the item, back n steps (a machine with random prizes); -1: until
 *                                   his hand is empty (the item held got used)
 *   b [; comment]                   put the item in Drew's hand into the bag
 *   # comment
 */
class ToonGuide {
public:
	explicit ToonGuide(ToonEngine *vm);

	/** Each time the engine reads the mouse: may point and press instead of the player. */
	void tick(int32 &mouseX, int32 &mouseY, int32 &buttons);

	void onTalk(int32 dialogId, const char *text);
	void enterFrame() { _depth++; }
	void leaveFrame() { _depth--; }
	bool hidesCursor() const { return _hideCursor; }

private:
	struct Step {
		enum Kind { kClick, kLook, kUse, kCombine, kItem, kSubject, kPoint, kFlagWait, kSceneWait, kWait, kStore, kUsePoint, kDouble, kGlobalWait, kUntil, kPress, kArcade, kFrameWait, kClown, kHeldOnItem, kAnimClick, kHeldWait, kSkipIf, kUntilScene, kUntilTalk } kind;
		int a = 0, b = 0, c = 0;
		bool now = false;   // E: as soon as the player has control, even while Drew walks or someone talks
		Common::String comment, section;
	};
	struct Move {
		enum Target { kHotspot, kPoint, kBagItem, kSubject } target;
		int id = 0, x = 0, y = 0;
		int buttons = 0;
		int frames = 1;
	};
	enum RunState { kStopped, kWaitIdle, kRunning, kSettle };

	bool load(const Common::String &path);
	bool parseLine(const Common::String &line, Step &st) const;
	bool isIdle() const;
	bool choosing() const;
	bool controllable() const;
	bool arcade(uint32 now);
	bool clown(uint32 t);
	bool clownType(uint32 t);
	static uint32 now() { return g_system->getMillis(); }
	bool available(const Step &st) const;
	void execute(const Step &st);
	void click(Move::Target target, int id, int buttons, int x = 0, int y = 0);
	bool position(const Move &m, int &x, int &y) const;
	bool hotspotPoint(int id, int &x, int &y) const;
	int hotspotAt(int x, int y) const;
	void next();
	bool waitStep(const Step &st) const;
	Common::String describe(const Step &st) const;
	void logf(const char *fmt, ...);
	void listObjects();
	void pollControl();
	void runCommand(const Common::String &cmd);

	ToonEngine *_vm;
	Common::Array<Step> _steps;
	Common::Array<Move> _moves;
	int _current = 0, _remaining = 0;
	RunState _state = kStopped;
	uint32 _stateSince = 0, _idleSince = 0, _missingSince = 0, _choiceSince = 0;
	bool _busy = false;
	bool _approaching = false;   // walking toward a hotspot off the screen, the step comes next
	int _approaches = 0;
	int _loops = 0;
	uint32 _arcadeStance = 0;   // when the last stance press was
	uint32 _arcadeFired = 0;
	bool _arcadeOn = false;
	uint32 _bagSince = 0;
	bool _heardTalk = false;
	Common::Array<int> _clownSeq;
	int _clownFrames[7] = {};
	uint32 _clownLast = 0, _clownClicked = 0;
	bool _clownSeen = false, _clownTyping = false;
	int _clownNext = 0;
	uint32 _clownStill = 0;
	int _depth = 0;           // nested doFrame() calls: above 1, something is running
	int _saveSections = 0, _sectionSaved = -1;
	bool _hideCursor = false;
	Common::String _controlPath;
	uint32 _controlOffset = 0;
	Common::Point _mouse;
};

} // End of namespace Toon

#endif
