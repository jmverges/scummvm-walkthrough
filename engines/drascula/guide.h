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

#ifndef DRASCULA_GUIDE_H
#define DRASCULA_GUIDE_H

#include "common/array.h"
#include "common/str.h"

namespace Drascula {

class DrasculaEngine;

/**
 * The walkthrough player for Drascula, the same idea as the other engines' ones: a walkthrough is the list of
 * things a player does, played back with the mouse the way a player would, one at a time when John waits.
 *
 * Drascula runs every action to its end inside one pass of its main loop (walking up to an object, talking,
 * cutscenes), so the guide only acts from the main loop and from the conversation menu.
 *
 * File format, one entry per line:
 *   == Section title
 *   e <verb> <object> [; comment]   pick a verb from the top bar (0 none: walk, go through a door) and click an object
 *   u <item> <object> [; comment]   take an item from the inventory screen and click it on an object (50: John)
 *   i <verb|item> <item> [; comment]  in the inventory screen, a verb or an item on an item (look at it, combine)
 *   k <option> [; comment]          a line of the conversation menu (1-4)
 *   g <x> <y> [; comment]           a click on the floor (walk there)
 *   r <room> [; comment]            wait until John is in that room
 *   n <flag> <value> [; comment]    wait until a game flag has that value
 *   w <seconds> [; comment]         wait
 *   # comment
 * Verbs: 1 look, 2 pick up, 3 open, 4 close, 5 talk, 6 move.
 */
class DrasculaGuide {
public:
	explicit DrasculaGuide(DrasculaEngine *vm);

	/** Once per pass of the main loop, after the events: may move the mouse and press its buttons. */
	void tick();
	/** Once per pass of the conversation menu; bottom[i] is where line i + 1 ends. */
	void tickConverse(const int bottom[4]);
	/** The game read the events again: a button the guide pressed is released. */
	void afterEvents();

	bool hidesCursor() const { return _hideCursor; }
	bool playing() const { return _state != kStopped || !_moves.empty(); }
	void onTalk(const char *file, const char *said);
	void onConverse(const char *const lines[4]);

	bool load(const Common::String &path);

private:
	struct Step {
		enum Kind { kObject, kUse, kInventory, kOption, kPoint, kRoomWait, kFlagWait, kWait } kind;
		int a, b;
		Common::String comment, section;
	};
	struct Move {
		enum Target { kObject, kBar, kSlot, kOption, kPoint, kJohn } target;
		int id, x, y;
		int button;    // 0 none, 1 left, 2 right
		int frames;    // passes of the loop to stay here
	};
	enum State { kStopped, kWaitIdle, kRunning, kSettle };

	bool parseLine(const Common::String &line, Step &st) const;
	bool available(const Step &st) const;
	void execute(const Step &st);
	void step(const Step &st, uint32 now, bool converse);
	bool position(const Move &m, int &x, int &y) const;
	int objectIndex(int num) const;
	int slotOf(int thing) const;
	void push(Move::Target target, int id, int button, int x = 0, int y = 0);
	void dropHeld();
	Common::String describe(const Step &st) const;
	void logf(const char *fmt, ...);
	void listObjects();
	void pollControl();
	void runCommand(const Common::String &cmd);
	void setCurrent(int step);
	void next();
	void runMoves(bool converse);

	DrasculaEngine *_vm;
	Common::Array<Step> _steps;
	Common::Array<Move> _moves;
	int _current, _remaining;
	State _state;
	uint32 _stateSince, _idleSince, _missingSince, _converseSince;
	bool _inConverse;   // the last pass was the conversation menu's
	bool _pressed;      // the guide holds the left button down until the game reads the events again
	const int *_bottom;
	bool _hideCursor, _logTalk;
	int _saveSections, _sectionSaved;
	Common::String _controlPath;
	uint32 _controlOffset;
	int _mouseX, _mouseY;
	int _pendingLoad = -1;
	int _startRoom = 0;
	bool _retried = false;
};

} // End of namespace Drascula

#endif
