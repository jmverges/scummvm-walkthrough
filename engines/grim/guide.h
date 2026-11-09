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

#ifndef GRIM_GUIDE_H
#define GRIM_GUIDE_H

#include "common/array.h"
#include "common/str.h"

namespace Grim {

/**
 * Walkthrough player for Grim Fandango.  The game's logic is Lua, so each step is a few lines of Lua run as a
 * script of its own, doing what the keys do: Sentence("use", object), Sentence("lookAt", ...), the inventory
 * hot key, the dialog's execute_line.  A step waits until Manny is idle (no cut scene, nobody talking, nobody
 * walking, no step still running) and is done when its script ends and the game is idle again.
 *
 * guide_file: the walkthrough, one step a line ("== name" starts section n, saved to guide_save_sections + n - 1).
 *   l OBJ          look at          p OBJ          pick up         o OBJ        walk out (doors, exits)
 *   u OBJ [ITEM]   use (with ITEM, taken out of the inventory first)
 *   h ITEM         take ITEM out    h -            put it away
 *   c N | c TEXT   a dialog line, by number or by (part of) its text; C TEXT: only if offered
 *   g X Y Z        walk there       t X Y Z        turn to look there
 *   x LUA          any Lua (a script: break_here(), sleep_for() are fine)
 *   X LUA          the same, also while an interface of the game's own is up (a computer, a panel)
 *   w SECS         wait             W LUA          wait until the Lua expression is true
 *   @SET step      only in that set (the set's table name, mo, le, ...)
 *   step => LUA    LUA must be true once the step is done, or the step is done again (twice at most)
 *   !step          starts as soon as the step before is done, without waiting for others' talk (a song)
 * OBJ and ITEM are Lua expressions (mo.scythe).  Lines between "{{{lua" and "}}}" are Lua of the walkthrough's
 * own, run after the player's (and so is the file guide_lua).  guide_control: commands appended to a file while
 * it runs.
 */
class Guide {
public:
	Guide();

	void tick();
	void onSay(const char *actor, const Common::String &text);
	void onLoaded() { _prelude = false; }
	// guide_speed: game time runs that many times faster (for testing a walkthrough; full screen movies are skipped)
	uint scaleTime(uint ms) const { return ms * _speed; }

private:
	struct Step {
		char kind;
		Common::String arg;
		Common::String onlySet;
		Common::String section;
		Common::String expect;
		bool optional;
		bool eager;
	};
	enum State { kStopped, kWaitIdle, kRunning, kSettle };

	void logf(const char *fmt, ...);
	bool load(const Common::String &path);
	bool parseLine(const Common::String &text, Step &st) const;
	static bool isLoose(const Step &st) { return st.kind == 'X' || st.kind == 'o'; }
	void setCurrent(int step);
	void skipStep();
	Common::String luaCall(const char *fn, const Common::String &arg = Common::String());
	bool luaRun(const Common::String &code);
	bool luaTrue(const Common::String &expr);
	void definePrelude();
	void runFile(const Common::String &path);
	Common::String busy();
	Common::Array<Common::String> choices();
	int choiceWith(const Common::String &text);
	Common::String stepCode(const Step &st);
	void runCommand(const Common::String &text);
	void pollControl();
	void save(int slot, const Common::String &desc);

	Common::Array<Step> _steps;
	int _current;
	int _remaining;
	State _state;
	uint32 _stateSince;
	uint32 _idleSince;
	uint32 _calmSince;
	uint32 _missingSince;
	bool _prelude;
	bool _stuckLogged;
	int _saveSections;
	int _sectionSaved;
	int _pendingSave;
	int _pendingLoad;
	Common::String _controlPath;
	uint32 _controlOffset;
	Common::String _lastSet;
	Common::String _stepSet;
	int _retries;
	uint _speed;
	Common::String _lastChoices;
	Common::String _lua;
};

} // End of namespace Grim

#endif
