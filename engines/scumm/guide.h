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

#ifndef SCUMM_GUIDE_H
#define SCUMM_GUIDE_H

#include "common/array.h"
#include "common/str.h"
#include "gui/dialog.h"

namespace Common {
class SeekableReadStream;
}

namespace GUI {
class StaticTextWidget;
}

namespace Scumm {

class ScummEngine;

/**
 * The walkthrough player.
 *
 * A walkthrough is the list of sentences a player gives the game ("pick up
 * the gun", "use the key with the door") and the dialogue choices they make,
 * by verb and object number.  It is played back the way the game's own
 * input script would: doSentence() for a sentence, the input script for a
 * click on a verb (dialogue icons and lines are verbs), each one only when
 * the game is idle and waiting for the player.
 *
 * Playing them is how easy mode (do the next action for me) and movie mode
 * (play the rest of the game by itself) work.  The same hooks also record a
 * walkthrough while someone plays, so writing one is playing the game once.
 *
 * File format, one entry per line:
 *   == Section title
 *   s <verb> <objectA> [<objectB>] [; comment]   a sentence
 *   v <verbid> [; comment]                       a click on a verb
 *   x <script> <room> [a b c] [; comment]        a script the input script starts itself
 *                                                (talking to someone, in Sam & Max)
 *   t <actor> [; comment]                        talk to that actor (Sam & Max: the talk
 *                                                cursor on them, through the input script)
 *   w <seconds> [; comment]                      wait (something in the game that takes time)
 *   u <actor> <xmin> <xmax> [; comment]          wait until that actor's x is in the range
 *                                                (something moving past, a boat ride)
 *   o <object> <state> [now] [; comment]         wait until the object turns to that state; with
 *                                                "now" it starts right after the step before
 *                                                instead of after the game settles
 *                                                (Sam & Max: the sky car at its stop)
 *   a <actor> [; comment]                        wait until the actor is here and standing still
 *   g <object> [; comment]                       walk to the object without using it
 *   q <object> <class> [; comment]               wait until ifClassOfIs(object, [class]) holds
 *                                                (Sam & Max: the sky car is at the station)
 *   m <name> [; comment]                         play a minigame until its room is left or it
 *                                                is solved (Sam & Max: wakarat, gatorgolf,
 *                                                frogrock, wires, vortex)
 *   i <object> [; comment]                       a click on it in the inventory bar
 *                                                (Sam & Max's dialogue icons are there)
 *   c <room> <x> <y> [; comment]                 a left click on the screen, in that room
 *                                                (the map in Sam & Max)
 *   # comment
 */
class ScummGuide {
public:
	explicit ScummGuide(ScummEngine *vm);
	~ScummGuide();

	/** Once per game loop iteration. */
	void tick();

	/** The engine is about to queue a sentence (doSentence). */
	void onSentence(int verb, int objectA, int objectB);
	/** Any script is about to start (logged for the scripts in guide_trace_scripts). */
	void onRunScript(int script, const int *args);
	/** The input script is about to run for a click on a verb. */
	void onVerbClick(int verbid, int x, int y);
	/** Someone says something (logged with guide_log_talk, for writing walkthroughs). */
	void onTalk(int actor, const char *text);
	/** The input script is about to run for a click on something in the inventory bar. */
	void onInventoryClick(int object);

	// Playback control (the menu, the debugger, test scripts).
	bool load(const Common::String &path);
	void play(int count);        // -1: to the end
	void stop();
	bool isPlaying() const { return _remaining != 0; }
	int stepCount() const { return _steps.size(); }
	int current() const { return _current; }
	Common::String describe(int i) const;
	Common::String section(int i) const;
	void setCurrent(int i);

private:
	struct Step {
		enum Kind { kSentence, kVerb, kScript, kClick, kInventory, kTalk, kWait, kMinigame, kUntil, kUntilClass, kGoTo } kind;
		int verb, objectA, objectB;   // kScript: verb = script, objectA = its room (0: any)
		int args[3];
		Common::String section, comment;
		Common::String name;   // kMinigame
	};

	enum State { kWaitIdle, kRunning };

	void parse(Common::SeekableReadStream &f, const Common::String &path);
	void loadBundled();
	bool isIdle() const;
	bool available(const Step &st) const;
	void execute(const Step &st);
	void parseScript(const Common::String &line, Step &st);
	void runStepScript(const Step &st);
	void click(int x, int y);
	void talkTo(int actor);
	void goTo(int object);
	bool playMinigame(const Step &st);   // true when it is over
	int _minigameRoom;
	int _minigameStage, _minigameX, _minigameY;
	int _talkPending;
	bool _untilSeenFalse;  // q steps wait for the condition to turn true      // talking to this actor once the player's actor stops
	uint32 _minigameNext;
	void logf(const char *fmt, ...);
	Common::String objectName(int obj) const;

	struct TestEvent;
	void runTestScript();
	void runCommand(const TestEvent &t);
	void pollControl();
	void runLive();
	void loadTestScript(const Common::String &path);
	void saveShot(const Common::String &name);

	ScummEngine *_vm;
	Common::Array<Step> _steps;
	int _current;
	int _remaining;              // steps still to play; -1 to the end
	State _state;
	uint32 _stateSince, _idleSince;
	bool _executing;
	Common::String _section;     // the last section seen while recording

	// Recording.
	Common::String _recordPath;

	// Scripted input for testing (guide_test).
	struct TestEvent {
		uint32 at;
		Common::String cmd, arg;
		int x, y;
	};
	Common::Array<TestEvent> _test;
	uint _testNext;
	uint32 _testStart;
	Common::String _shotDir;

	// Live control (guide_control).
	Common::String _controlPath;
	uint32 _controlOffset;
	Common::Array<Step> _live;
	uint32 _liveSince;
	Common::String _traceScripts;   // " 50 51 " : scripts whose start is logged with its arguments
};

/**
 * The walkthrough's page in the game menu (the Help button becomes
 * "Walkthrough" when the game has one): do the next step, play to the end,
 * hand the game back, or show the controls help as before.
 */
class GuideDialog : public GUI::Dialog {
public:
	enum Result { kClosed = 0, kNextStep = 1, kPlayToEnd = 2, kNormal = 3, kShowHelp = 4 };
	GuideDialog(ScummGuide *guide);
	void handleCommand(GUI::CommandSender *sender, uint32 cmd, uint32 data) override;

private:
	void refresh();
	ScummGuide *_guide;
	GUI::StaticTextWidget *_where, *_next, *_after;
};

} // End of namespace Scumm

#endif
