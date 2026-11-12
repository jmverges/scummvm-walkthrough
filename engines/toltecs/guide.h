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

#ifndef TOLTECS_GUIDE_H
#define TOLTECS_GUIDE_H

#include "common/array.h"
#include "common/str.h"

namespace Toltecs {

class ToltecsEngine;

/**
 * The walkthrough player for 3 Skulls of the Toltecs, the same idea as the other engines' ones: a walkthrough is the
 * list of things a player does, played back with the mouse the way a player would, one at a time when the game waits
 * for the player.
 *
 * The whole game is one script; the engine only gives it the mouse. The script looks the mouse up in hotspot tables
 * (rectangles in its own data, each with the offset of its name just before it), so the player finds an object by its
 * name there, points at it, checks that the verb line shows that name (a hotspot can be switched off) and clicks.
 *
 * File format, one entry per line (UTF-8; names as the game shows them, any case):
 *   == Section title
 *   s <verb> <name> [> <name>] [; comment]   a sentence: the verb in the bottom bar, then the object (a hotspot of the
 *                                            scene or an inventory item), then the second one if any. Verbs: abrir,
 *                                            cerrar, coger, dar, usar, mover, hablar, mirar, ir (no verb: walk to it)
 *   b <name> [; comment]        a few steps away from a hotspot, onto the floor below it (an action on a hotspot whose
 *                               spot he already stands at does nothing: he walks there first and acts on arriving)
 *   k <text> [; comment]        the dialogue line that contains that text
 *   g <x> <y> [; comment]       a left click on a point of the scene (scene coordinates: walk there)
 *   c <x> <y> [; comment]       a left click on a point of the screen (640x400)
 *   r <x> <y> [; comment]       a right click on a point of the screen
 *   d <x> <y> [; comment]       a double click on a point of the screen (an exit: go through it at once)
 *   a <scene> [; comment]       wait until the game shows that scene (its resource number)
 *   t <text> [; comment]        wait until the verb line reads that
 *   w <seconds> [; comment]     wait
 *   Upper case S/K/G/C/R/D: the same as soon as the player has control, even while someone talks.
 *   # comment
 */
class ToltecsGuide {
public:
	explicit ToltecsGuide(ToltecsEngine *vm);

	/** Once per frame, after the engine read the real mouse: may move and click the mouse instead of the player. */
	void tick();

	// Hooks.
	void onRects(int kind, int16 slot, int16 offset, int16 itemSize, int16 first, int16 found);
	void onVerbLine(int16 slot, int16 offset);
	void onTalk(int16 slot, int16 offset);
	void onWalk();

	bool load(const Common::String &path);

private:
	struct Step {
		enum Kind { kSentence, kDialogue, kPoint, kScreenClick, kScreenRight, kScreenDouble, kSceneWait, kVerbWait,
			kWait, kStepBack } kind;
		int a = 0, b = 0;
		Common::String verb, first, second, text;   // names in the game's code page (cp850)
		bool now = false;
		Common::String comment, section;
	};
	struct Action {
		enum Type { kMove, kAim, kReveal, kChoose } type;
		int x, y;          // screen point (kMove)
		bool left, right;  // buttons held this frame (kMove)
		bool dbl;          // the second press of a double click (kAim: the second object, may scroll the items)
		bool inv = false;  // kAim: an inventory item only (the first object of "usar X > Y")
		int frames;
		Common::String name;   // the object to point at and click (kAim), the dialogue line's text (kChoose)
	};
	struct Table {
		int kind;
		int16 slot, offset, itemSize;
		uint32 lastFrame;
		int scene;   // the scene it was last seen in
	};
	struct Rect {
		int16 x, y, w, h;
	};
	struct Spot {
		int table, index;   // into _tables, the hotspot's index in it
	};
	enum State { kStopped, kWaitIdle, kRunning, kSettle };

	bool parseLine(const Common::String &line, Step &st) const;
	bool isIdle() const;
	bool hasControl() const;
	bool talking() const;
	Common::Array<int> currentTables() const;
	int tableCount(const Table &t) const;
	bool rectOf(const Table &t, int index, Rect &r) const;
	Common::String nameOf(const Table &t, int index) const;
	bool tablePoint(const Table &t, int index, int &x, int &y) const;
	Common::Array<Spot> spotsNamed(const Common::String &name) const;
	int offVertically(const Common::String &name) const;
	bool offScreen(const Common::String &name, int &x, int &y) const;
	bool onHotspot(int x, int y) const;
	bool stepBack(const Common::String &name, int &x, int &y) const;
	void move(int x, int y, int frames);
	void click(int x, int y, bool right, bool dbl = false);
	void aim(const Common::String &name);
	bool runAim(Action &a);
	bool runChoose(Action &a);
	bool runReveal(Action &a);
	bool choosing() const;
	bool quiet() const;
	int buttonCount() const;
	int buttonX(int i, int n) const;
	void execute(const Step &st);
	void fail(const char *why);
	Common::String describe(const Step &st) const;
	void logf(const char *fmt, ...);
	void listState();
	void startProbe();
	Common::String entryBytes(const Table &t, int index) const;
	void pollControl();
	void runCommand(const Common::String &cmd);
	void setCurrent(int step);
	void next();

	ToltecsEngine *_vm;
	Common::Array<Step> _steps;
	Common::Array<Action> _actions;
	Common::Array<Table> _tables;
	int _current, _remaining;
	State _state;
	uint32 _frame, _stateSince, _idleSince, _lastRectFrame, _lastWalkFrame, _missingSince, _choiceSince;
	uint32 _chatterSince = 0;
	uint32 _talkSince = 0;      // since when someone talks at all
	int _lastScene = -1;
	uint32 _sceneSince = 0;   // the frame the current scene began   // since when someone talks while the player has control
	uint32 _quietSince = 0;     // since when the game waits (quiet())
	bool _busy, _spoke, _approaching;
	int _approaches;
	int _retries = 0;   // screen clicks repeated because nothing happened
	int _stepScene = 0;   // the scene the step started in
	bool _verbChosen = false;
	bool _dropTried = false;   // a failure of this step already right-clicked to drop a verb   // the step clicked a verb in the bar (a failure drops it with a right click)
	bool _hideCursor, _logVerb;
	int _saveSections, _sectionSaved;
	Common::Array<Spot> _aimSpots;   // the hotspots with the name being aimed at, and which one is tried
	int _aimSpot, _aimSlot = 0, _aimFrames;
	int _scrollPhase = 0, _scrollMoves = 0;   // scrolling the inventory for an item: 0 to the first, 1 to the last
	Common::String _scrollSeen;   // the item in the slot read before the last scroll
	int _chooseButton, _chooseCount;   // the dialogue button being read, how many there are
	Common::Array<Common::String> _lines;   // the dialogue lines read so far
	Common::Array<int> _probeList;   // "objects": hover each hotspot to read its verb line text
	int _probeTable, _probeIndex, _probeFrames;
	int _mouseX, _mouseY;
	int _sceneMouseX = 320, _sceneMouseY = 150;   // where the guide last pointed in the scene
	Common::String _controlPath, _lastVerb, _verbLine;
	int _verbSlot, _verbOffset;   // where the verb line's last text is
	Common::String _lastTalk;   // the last line said or label shown, and when
	uint32 _lastTalkFrame = 0;
	uint32 _controlOffset;
	int _actionFrames;   // frames left of a key action ("key")
};

} // End of namespace Toltecs

#endif
