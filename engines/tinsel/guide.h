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

#ifndef TINSEL_GUIDE_H
#define TINSEL_GUIDE_H

#include "common/array.h"
#include "common/list.h"
#include "common/str.h"

namespace Common {
class SeekableReadStream;
struct PROCESS;
}

namespace Tinsel {

class TinselEngine;

/**
 * The walkthrough player for Tinsel games (Discworld), the same idea as the
 * SCUMM one: a walkthrough is the list of things a player does, played back
 * the way the game's own input would do them, one at a time when the game is
 * waiting for the player. The same hooks record a walkthrough while someone
 * plays.
 *
 * Things on screen are polygons (hotspots and exits, by their number in the
 * scene, "P12") and actors ("A3"); events are walk (a single left click),
 * act (a double click), look (a right click).
 *
 * File format, one entry per line:
 *   == Section title
 *   e <walk|act|look> <P<n>|A<n>> [<item>] [; comment]   an event on a hotspot or actor,
 *                                                        with that item in hand
 *   i <walk|act|look> <item> [<held item>] [; comment]   an event on an item in the
 *                                                        inventory (walk picks it up)
 *   k <icon> [; comment]                                 a topic in the conversation window
 *   h <item> [; comment]                                 take the item in hand (0: drop it)
 *   p <1|2> [; comment]                                  put the held item into the Luggage (1) or
 *                                                        Rincewind's own pockets (2)
 *   g <x> <y> [; comment]                                walk there (a click on the floor; exits
 *                                                        are crossed by walking into them)
 *   a <scene> [; comment]                                wait until the game is in that scene
 *   n <global> <value> [; comment]                       wait until the game's global variable has that
 *                                                        value (something that comes and goes)
 *   c <x> <y> [; comment]                                a left click on the screen
 *   r <x> <y> [; comment]                                a right click
 *   w <seconds> [; comment]                              wait
 *   ?<step>                                              only if its target is here (a walk that an
 *                                                        event may have cut short)
 *   !<step>                                              done as soon as the player has control back,
 *                                                        while its code still runs (a short chance)
 *   # comment
 */
class TinselGuide {
public:
	explicit TinselGuide(TinselEngine *vm);

	/** Once per game cycle. */
	void tick();

	bool hidesCursor() const { return _hideCursor; }

	// Recording hooks.
	void onUserEvent(int actor, int hPoly, int event);
	void onInventoryEvent(int item, int event);
	void onConverse(int icon);
	void onTalk(int actor, const char *text);

	bool load(const Common::String &path);
	void play(int count);
	void stop();

private:
	struct Step {
		enum Kind { kEvent, kInventory, kConverse, kHold, kPutDown, kVarWait, kSceneWait, kClick, kRightClick, kWait, kWalk } kind;
		int event;            // TINSEL_EVENT
		bool actor;           // kEvent: an actor, not a polygon
		bool optional;        // "?": skip it if its target is not here
		bool quick;           // "!": done when the player has control back
		int target, item;     // polygon/actor/inventory item; the item in hand
		int x, y;             // kClick
		Common::String section, comment;
	};
	enum State { kWaitIdle, kRunning };

	bool parseLine(const Common::String &line, Step &st);
	bool isIdle() const;
	bool available(const Step &st) const;
	void execute(const Step &st);
	Common::String describe(const Step &st) const;
	void click(int x, int y, bool right);
	bool pointIn(int hPoly, int &x, int &y) const;
	void logf(const char *fmt, ...);
	Common::String polyName(int hPoly) const;
	Common::String actorName(int actor) const;
	void listObjects();
	void pollControl();
	void runCommand(const Common::String &cmd);
	void saveShot(const Common::String &name);
	void record(const Common::String &line);

	TinselEngine *_vm;
	Common::Array<Step> _steps;
	Common::Array<Step> _live;
	int _current, _remaining;
	State _state;
	uint32 _stateSince, _idleSince, _liveSince;
	bool _hideCursor, _logTalk, _executing;
	int _saveSections, _sectionSaved;
	Common::String _recordPath, _controlPath, _shotDir, _section;
	uint32 _controlOffset;
	mutable Common::List<Common::PROCESS *> _stepProcs;	// code processes the last step started
	int _stepScene = -1;	// and the scene it ran in
	Step _pendingInv;	// an inventory step waiting for its window to open
	int _pendingInvNo = 0;
	mutable uint32 _quietSince = 0;	// when the step started (or the screen last scrolled)
	uint32 _lastTalk = 0;	// when the last line was said
};

} // End of namespace Tinsel

#endif
