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

#ifndef STARK_GUIDE_H
#define STARK_GUIDE_H

#include "common/array.h"
#include "common/rect.h"
#include "common/str.h"

namespace Stark {

class ActionMenu;

namespace Resources {
class ItemVisual;
class Speech;
}

/**
 * Walkthrough player for The Longest Journey.  A step does what the mouse does: the cursor goes to the hotspot,
 * a click opens the action menu and the eye, hand or mouth is clicked; an inventory item is taken out of the
 * inventory and clicked on the hotspot; a dialog line is clicked.  A step waits until April can be moved (no
 * cut scene, no dialog, no one talking, no action's script running, April not walking) and is done once the
 * game waits for the player again.
 *
 * guide_file: the walkthrough, one step a line ("== name" starts section n, saved to guide_save_sections + n - 1).
 * Hotspots go by the title the game shows over them (or "=resource name"), " [n]" picks the n-th of that title.
 *   l HOTSPOT       look             u HOTSPOT       use (the hand)     t HOTSPOT     talk (the mouth)
 *   o HOTSPOT       an exit          i ITEM > HOTSPOT  an inventory item on a hotspot (or on another item)
 *   L ITEM / T ITEM look at / talk to an inventory item, in the inventory
 *   h ITEM          take an inventory item in hand and keep it there: an "i ITEM > ..." after it is one click
 *   c N | c TEXT    a dialog line, by number or by (part of) its text; C TEXT: only if offered
 *   g X Y           walk to that point of the screen (640x480)
 *   w SECS          wait             W COND          wait until COND
 *   @LOC step       only in a location whose name contains LOC
 *   !step           starts while the step before still has a script running (the game lets the player act)
 *   step => COND    COND must hold once the step is done, or the step is done again (four times at most)
 * "STEP @X,Y": the cursor goes to that point of the screen (640x480), for a hotspot with buttons.
 * COND: "has ITEM", "loc TEXT", "see TITLE" (that hotspot is on the screen), "know NAME" (a knowledge of the
 * location, the level or the game), each one
 * with "not " in front if wanted, joined by " and ".  guide_control: commands appended to a file while it runs.
 */
class Guide {
public:
	Guide();

	void tick();
	void onSpeech(Resources::Speech *speech);
	// guide_speed: game loops run each frame (for testing a walkthrough; speech and movies are skipped)
	uint loopsPerFrame() const { return _speed; }

private:
	struct Step {
		char kind;
		Common::String arg;
		Common::String onlyLoc;
		Common::String section;
		Common::String expect;
		bool optional;
		bool eager;
	};
	struct Target {
		Resources::ItemVisual *item;
		int hotspot;
		Common::String title;
		Common::Point screen;   // a point of the game window over it, (-1, -1) if none
	};
	enum State { kStopped, kWaitIdle, kRunning, kSettle };
	// what a step's clicks are doing
	enum UiPhase { kUiNone, kUiToInventory, kUiToSlot, kUiPickItem, kUiToTarget, kUiClickTarget, kUiToButton, kUiClickButton,
		kUiToLine, kUiClickArrow, kUiClickLine, kUiDone };

	void logf(const char *fmt, ...);
	bool load(const Common::String &path);
	bool parseLine(const Common::String &text, Step &st) const;
	void setCurrent(int step);
	void skipStep();
	void save(int slot, const Common::String &desc);
	Common::String busy();
	Common::String locationName();
	Common::Array<Common::String> choices();
	int choiceWith(const Common::String &text);
	bool findTarget(const Common::String &spec, Target &t, bool sceneOnly, int action = -1);
	bool findInventory(const Common::String &spec, Target &t);
	Common::Point screenPointFor(Resources::ItemVisual *item, int hotspot, int action);
	bool cond(const Common::String &expr);
	void listObjects(bool all);
	Common::Array<Resources::ItemVisual *> sceneItems();
	bool startStep(const Step &stIn);
	void uiTick(uint32 now);
	void glideTo(const Common::Point &original, uint32 now);
	bool menuAt(ActionMenu *menu, int action, uint32 now);
	bool slotPoint(Resources::ItemVisual *item, Common::Point &p);
	void click();
	void runCommand(const Common::String &text);
	void pollControl();

	Common::Array<Step> _steps;
	int _current;
	int _remaining;
	State _state;
	uint32 _stateSince;
	uint32 _idleSince;
	uint32 _calmSince;
	uint32 _missingSince;
	int _saveSections;
	int _sectionSaved;
	int _pendingSave;
	int _pendingLoad;
	Common::String _controlPath;
	uint32 _controlOffset;
	Common::String _lastLoc;
	Common::String _stepLoc;
	int _retries;
	uint _speed;
	Common::String _lastChoices;

	// the clicks of the step running
	UiPhase _ui;
	uint32 _uiAt;           // when the next phase starts
	int _uiAction;          // the action wanted: PAT action, or an inventory item's index
	Target _uiTarget;
	Target _uiItem;
	bool _uiInInventory;    // the target is in the inventory
	bool _uiHand;           // the action menu's hand takes the item first
	bool _uiHoldOnly;       // "h": the item stays in hand
	Common::Point _buttonAt;
	Common::Point _uiAtPoint;     // the screen point a step asked for ("@X,Y")
	int _uiLine;
	Common::Point _glideFrom, _glideTo;
	uint32 _glideStart, _glideTime;
};

extern Guide *g_guide;

} // End of namespace Stark

#endif
