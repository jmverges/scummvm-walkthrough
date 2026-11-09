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

#ifndef AGOS_GUIDE_H
#define AGOS_GUIDE_H

#include "common/array.h"
#include "common/rect.h"
#include "common/str.h"

namespace AGOS {

class AGOSEngine;
struct HitArea;
struct Item;

/**
 * The walkthrough player for Simon the Sorcerer 2, the same idea as the SCUMM, Tinsel and Broken Sword
 * ones: a walkthrough is the list of things a player does, played back with the mouse the way a player
 * would, one at a time when the game waits for the player.
 *
 * File format, one entry per line:
 *   == Section title
 *   e <verb> <item> [<item2>] [; comment]   click a verb (its box, 0 for none: walk; 102 look, 103 pick up, 104 give,
 *                                           105 talk, 106 use, 107 wear, 108 move, 109 open), then the item,
 *                                           then the second one for "use with" and "give to"; an item is its
 *                                           number, or minus a box id for the people and things that are boxes
 *   E <verb> <item> [<item2>] [; comment]   the same as soon as the game takes clicks, even while people talk
 *   h <box> [; comment]                     click a box by its id (one with no item)
 *   k <n> [; comment]                       the n-th line of a conversation's choices, from the top
 *   c <words> [; comment]                   the choice whose text has these words (the game's own text, so
 *                                           better without accented letters)
 *   C <words> [; comment]                   the same if the conversation offers it, else skipped (also when
 *                                           there is no conversation)
 *   @<room> <step>                          a step only done when Simon is in that room
 *   g <x> <y> [; comment]                   click on the room at x, y (room coordinates, scroll included)
 *   p <path> <point> [; comment]            click on a point of the room's walking paths: the exits are such
 *                                           points (the room's script says which)
 *   x <path> <point> [; comment]            the same for an exit: clicked again until Simon is in another room
 *   r <room> [; comment]                    wait until Simon is in that room
 *   r <room> <s> -<n> [; comment]           the same for s seconds at most, then n steps back to try again
 *   n <var> <value> [; comment]             wait until a script variable has that value
 *   b <bit> <value> [; comment]             wait until a bit flag has that value (256 + n: the second array's n)
 *   w <seconds> [; comment]                 wait
 *   t <words> [; comment]                   wait until someone says something with these words (the game's text)
 *   v <var> <value> [<value2>] [; comment]  The Feeble Files: wait until a variable of the second set (someone
 *                                           else walking: the native in the valley) has that value, or the other
 *   s <who> [; comment]                     The Feeble Files: play as 15 Feeble or 16 SAM (the button by the
 *                                           Oracle's, where both can be played)
 * Any number may be "$<var>[+<n>]" instead: what that script variable holds when the step comes up, plus n
 * (a puzzle the game sets up at random).
 *   # comment
 */
class AGOSGuide {
public:
	explicit AGOSGuide(AGOSEngine *vm);

	/** From the event loop, outside the timer: the state machine and the control file. */
	void tick();
	/** From the timer, once a frame: where the mouse is and whether it clicks, when the guide moves it. */
	bool frame(Common::Point &mouse, bool &click);

	bool hidesCursor() const { return _hideCursor; }
	bool active() const { return !_moves.empty(); }

	// Hooks.
	void onTalk(uint sprite, const byte *text);
	void onMessage(const char *text);
	void onInteract(uint num, int y, const char *text);

	bool load(const Common::String &path);

private:
	struct Step {
		enum Kind { kVerb, kBox, kChoice, kPoint, kRoomWait, kVarWait, kBitWait, kWait, kChoiceText, kPathPoint, kTalkWait, kSwitch, kVar2Wait } kind;
		int a = 0, b = 0, c = 0;
		bool now = false;   // E: as soon as the game takes clicks
		bool exit = false;  // x: done only once Simon is in another room
		bool optional = false;   // C: a line that may not be offered, then skipped
		int onlyRoom = 0;        // @<room>: only in that room, else skipped
		Common::String text, comment, section;
		Common::String raw;      // a line with $<var> in it, read again when the step comes up
	};
	struct Move {
		enum Target { kBox, kItem, kChoice, kPoint, kIdle, kStay, kCursor, kLeave, kOpen } target = kPoint;
		int id = 0, x = 0, y = 0;
		bool click = false;
		int frames = 0;   // how many frames to stay here
		int gate = 0;     // 2: not before the game asks for a second item
		int waited = 0, arrows = 0, walks = 0;
		Target aheadTarget = kItem;   // a verb's click: the item that follows, walked to first when out of view
		int aheadId = 0;
	};
	enum State { kStopped, kWaitIdle, kRunning, kSettle };
	enum Aim { kHere, kGone, kArrow, kScrolling, kWalk, kOracle, kLeaveOracle };

	Common::String withVariables(const Common::String &line) const;
	bool parseLine(const Common::String &line, Step &st) const;
	bool isIdle() const;
	bool available(const Step &st) const;
	void execute(const Step &st);
	Aim aim(const Move &m, int &x, int &y) const;
	Aim toward(int rx, int ry, int &x, int &y) const;
	void pushClick(Common::Array<Move> &list, uint at, Move::Target target, int id, int x, int y, int gate);
	HitArea *findItemBox(int item) const;
	HitArea *findBoxId(int id) const;
	Common::Array<HitArea *> choices() const;
	int choiceWith(const Common::String &text) const;
	bool inInventory(int item) const;
	bool pathPoint(int path, int point, int &x, int &y) const;
	void nearestPathPoint(int x, int y, int &path, int &point) const;
	const HitArea *pickAt(int x, int y) const;
	bool floorPoint(int path, int point, int &x, int &y) const;
	bool boxOnScreen(const HitArea *ha, int &x, int &y) const;
	int room() const;
	int inventoryCount() const;   // a signature of the inventory
	uint32 boxSignature() const;
	uint32 flagSignature() const;
	bool feeble() const;
	bool bit(int n) const;
	int maxPaths() const;
	uint pathEnd() const;
	int scrollXAt(int y) const;
	int scrollY() const;
	int screenW() const;
	int screenH() const;
	int roomBottom() const;
	int itemId(Item *item) const;
	Common::String itemName(Item *item) const;
	Common::String describe(const Step &st) const;
	void logf(const char *fmt, ...);
	void listObjects(bool all = false);
	void pollControl();
	void runCommand(const Common::String &cmd);
	void setCurrent(int step);
	bool save(int slot, const Common::String &desc);
	bool restore(int slot);

	AGOSEngine *_vm;
	Common::Array<Step> _steps;
	Common::Array<Move> _moves;
	int _current, _remaining;
	State _state;
	uint32 _stateSince, _idleSince, _missingSince;
	mutable uint32 _walkSince = 0;
	uint32 _stepFlags = 0;
	bool _busy = false;   // the game took control after the step started
	bool _effect = false;   // somebody talked, a choice came up (since the step started)
	int _stepInv = 0;
	uint32 _stepBoxes = 0;
	bool _logClicks = true;
	int _stepRoom = 0, _retries = 0;
	int _choiceGen = 0, _clickGen = -1;
	int _seenRoom = 0;
	uint32 _roomSince = 0;   // when the room the guide last saw came up
	uint32 _lastFrame = 0;   // when the timer last took the guide's pointer
	uint32 _clickableSince = 0;   // conversation lines printed so far, and when the step clicked
	bool _stepMap = false;
	bool _hideCursor, _logTalk;
	int _saveSections, _sectionSaved;
	int _pendingLoad = -1, _pendingSave = -1;
	Common::String _controlPath;
	uint32 _controlOffset;
	Common::Point _guideMouse;
	Common::String _msgLine;
	Common::String _heard;   // what was said since the step started
	Common::Array<Common::String> _choiceText;
};

} // End of namespace AGOS

#endif
