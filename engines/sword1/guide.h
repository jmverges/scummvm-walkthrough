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

#ifndef SWORD1_GUIDE_H
#define SWORD1_GUIDE_H

#include "common/array.h"
#include "common/rect.h"
#include "common/str.h"

namespace Sword1 {

class SwordEngine;

/**
 * The walkthrough player for Broken Sword, the same idea as the SCUMM and
 * Tinsel ones: a walkthrough is the list of things a player does, played back
 * with the mouse the way a player would, one at a time when George is waiting.
 *
 * File format, one entry per line:
 *   == Section title
 *   e <object> [; comment]          a left click on an object (walk there, use, talk)
 *   E <object> [; comment]          the same as soon as the player has control, even while people talk
 *   l <object> [; comment]          a right click on it (look)
 *   i <item> [; comment]            take an item from the inventory bar (top of the screen)
 *   I <item> [; comment]            the same as soon as the player has control
 *   u <item> <object> [; comment]   take the item and click it on an object
 *   U <item> <object> [; comment]   the same as soon as the player has control (a short chance to act)
 *   t <item> <item> [; comment]     take the item and click it on another item in the bar
 *   k <subject> [; comment]         a conversation subject in the bottom bar
 *   s <item> [; comment]            in a conversation, show an item from the inventory bar
 *   S <item> [; comment]            the same as soon as the bars open, even while someone still talks
 *   g <x> <y> [; comment]           a left click on the screen (walk there)
 *   G <x> <y> [; comment]           the same as soon as the player has control (to get out of the way)
 *   a <screen> [; comment]          wait until the game is on that screen
 *   n <var> <value> [; comment]     wait until a script variable has that value
 *   w <seconds> [; comment]         wait
 *   y <object> <anim> [; comment]   wait until that object plays that animation (a moment to act)
 *   z <object> <dirs> [; comment]   wait until that character stands facing one of the directions in the mask
 *                                   (bit 0 up, 1 up-right, 2 right... 7 up-left), e.g. when a guard looks away
 *   # comment
 */
class SwordGuide {
public:
	explicit SwordGuide(SwordEngine *vm);

	/** Once per frame, before the mouse engine: may move and click the mouse instead of the player. */
	void tick(Common::Point &mouse, uint16 &state);

	bool hidesCursor() const { return _hideCursor; }

	// Hooks.
	void onSpeak(int32 id, uint32 textNo);

	bool load(const Common::String &path);

private:
	struct Step {
		enum Kind { kClick, kLook, kItem, kUse, kCombine, kSubject, kShow, kPoint, kScreenWait, kVarWait, kWait, kAnimWait, kFaceWait } kind;
		int a, b;
		bool now = false;   // E: don't wait for the game to be idle, only for the player to have control
		Common::String comment, section;
	};
	struct Move {
		enum Target { kObject, kTopItem, kBottomSubject, kPoint, kTopBar } target;
		int id, x, y;
		uint16 flags;  // button flags for this frame
		int frames;    // how many frames to stay here
	};
	enum State { kStopped, kWaitIdle, kRunning, kSettle };

	bool parseLine(const Common::String &line, Step &st) const;
	bool isIdle() const;
	bool choosing() const;
	bool available(const Step &st) const;
	void execute(const Step &st);
	bool position(const Move &m, int &x, int &y) const;
	bool offScreen(int id, int &x, int &y) const;
	bool onHotspot(int x, int y) const;
	int pickAt(int x, int y) const;
	bool isPerson(int id) const;
	void clickOn(Move::Target target, int id, bool right = false);
	Common::String objectName(int id) const;
	Common::String itemName(int item) const;
	Common::String describe(const Step &st) const;
	void logf(const char *fmt, ...);
	void listObjects();
	void pollControl();
	void runCommand(const Common::String &cmd);
	void record(uint16 state, const Common::Point &mouse);
	void setCurrent(int step);

	SwordEngine *_vm;
	Common::Array<Step> _steps;
	Common::Array<Move> _moves;
	int _current, _remaining;
	State _state;
	uint32 _stateSince, _idleSince, _missingSince, _choiceSince = 0;
	bool _busy = false;   // the game took control after the step started
	bool _approaching = false;   // walking toward an object out of view, the step comes next
	int _approaches = 0;
	bool _barOpen = false;
	bool _spoke = false, _retried = false;
	uint32 _barSince = 0;
	int _watchId = 0, _watchAnim = -1, _watchObjs = 0, _watchScreen = 0, _watchCtl = -1;
	bool _hideCursor, _logTalk;
	int _saveSections, _sectionSaved;
	Common::String _recordPath, _controlPath;
	uint32 _controlOffset;
	Common::Point _guideMouse;
	Common::Array<Common::String> _live;
};

} // End of namespace Sword1

#endif
