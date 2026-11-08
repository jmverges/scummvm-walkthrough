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

#ifndef SWORD2_GUIDE_H
#define SWORD2_GUIDE_H

#include "common/array.h"
#include "common/rect.h"
#include "common/str.h"

namespace Sword2 {

class Sword2Engine;

/**
 * The walkthrough player for Broken Sword II, the same idea as the Broken Sword one: a walkthrough is the list of
 * things a player does, played back with the mouse the way a player would, one at a time when George (or Nico)
 * is waiting.
 *
 * File format, one entry per line (ids are the objects' resource ids, as "objects" lists them):
 *   == Section title
 *   e <object> [; comment]          a left click on an object (walk there, use, talk)
 *   E <object> [; comment]          the same as soon as the player has control, even while walking
 *   l <object> [; comment]          a right click on it (look)
 *   i <item> [; comment]            a right click on an item of the inventory menu (look at it)
 *   u <item> <object> [; comment]   take the item from the inventory menu and click it on an object
 *   t <item> <item> [; comment]     take the item and click it on another item of the menu
 *   v <object> <ms> [; comment]     wait until that object has been off the mouse list (out of sight) that long
 *   h <item> [; comment]            take the item from the menu and hold it: the next click on an object uses it
 *   k <subject> [; comment]         a conversation subject (its icon resource), or an item shown in one
 *   g <x> <y> [; comment]           a left click on the screen (walk there; 0-639, 0-399 below the top bar)
 *   a <location> [; comment]        wait until the game is in that location
 *   n <var> <value> [; comment]     wait until a script variable has that value
 *   w <seconds> [; comment]         wait
 *   x <object> <min> <max>          wait until the left edge of that object's mouse box (its sprite) is in that
 *                                   range: a pose of someone, a moment to act
 *   # comment
 */
class Sword2Guide {
public:
	explicit Sword2Guide(Sword2Engine *vm);

	/** Once per game cycle, before the mouse engine: may point and click instead of the player. */
	void tick();

	/** The guide's mouse position (game coordinates: y 0 is the top of the game area), while it plays. */
	bool mousePos(int &x, int &y) const;

	bool hidesCursor() const { return _hideCursor; }

	void onSpeak(uint32 id, const char *text);

	bool load(const Common::String &path);

private:
	struct Step {
		enum Kind { kClick, kLook, kItem, kUse, kCombine, kSubject, kPoint, kScreenWait, kVarWait, kWait, kRectWait, kHold, kGoneWait } kind;
		int a = 0, b = 0, c = 0;
		bool now = false;   // E: right away, without waiting for the player character to stand still
		Common::String comment, section;
	};
	struct Move {
		enum Target { kObject, kMenuItem, kSubject, kPoint } target;
		int id = 0, x = 0, y = 0;
		uint16 buttons = 0;   // a button event sent this frame
		int frames = 1;       // how many frames to stay here
	};
	enum State { kStopped, kWaitIdle, kRunning, kSettle };

	bool parseLine(const Common::String &line, Step &st) const;
	bool isIdle() const;
	bool choosing() const;
	bool available(const Step &st) const;
	void execute(const Step &st);
	void clickOn(Move::Target target, int id, uint16 button);
	bool position(const Move &m, int &x, int &y) const;
	int pickAt(int x, int y) const;
	bool offScreen(int id, int &x, int &y) const;
	Common::String name(uint32 res) const;
	Common::String describe(const Step &st) const;
	void logf(const char *fmt, ...);
	void listObjects();
	void refreshItems();
	int pointerOf(int id) const;
	void pollControl();
	void runCommand(const Common::String &cmd);
	void setCurrent(int step);
	void next();
	bool waitStep(const Step &st) const;

	Sword2Engine *_vm;
	Common::Array<Step> _steps;
	Common::Array<Move> _moves;
	Common::Array<uint32> _varSnap;
	int _current = 0, _remaining = 0;
	State _state = kStopped;
	uint32 _stateSince = 0, _idleSince = 0, _missingSince = 0, _choiceSince = 0;
	bool _spoke = false, _retried = false;
	uint32 _goneSince = 0;
	int _stepLocation = 0;
	int _exitTries = 0;
	int _stepFeetX = 0, _stepFeetY = 0;   // where George stood as the last step started
	int _retries = 0;   // sections tried again after George or Nico died   // where the last step started
	bool _busy = false;        // the game reacted after the step started
	bool _approaching = false; // walking toward an object out of view, the step comes next
	int _approaches = 0;
	bool _active = false;      // the guide's mouse position is the one the game sees
	Common::Point _mouse;
	int _feetX = 0, _feetY = 0;
	uint32 _feetSince = 0;
	int _saveSections = 0, _sectionSaved = -1;
	bool _hideCursor = false;
	Common::String _controlPath;
	uint32 _controlOffset = 0;
};

} // End of namespace Sword2

#endif
