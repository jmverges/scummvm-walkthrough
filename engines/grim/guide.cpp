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

#include "common/config-manager.h"
#include "common/debug.h"
#include "common/file.h"
#include "common/fs.h"
#include "common/system.h"
#include "common/ustr.h"

#include "engines/grim/grim.h"
#include "engines/grim/guide.h"
#include "engines/grim/localize.h"
#include "engines/grim/movie/movie.h"
#include "engines/grim/set.h"
#include "engines/grim/sector.h"
#include "engines/grim/lua/lua.h"

namespace Grim {

// the game's text is Windows-1252 (UTF-8 already from a UTF-16 grim.tab); logs and the walkthrough are UTF-8
static Common::String toUtf8(const Common::String &s) {
	if (g_grim->_isUtf8)
		return s;
	return Common::U32String(s, Common::kWindows1252).encode();
}

static const uint32 kSettle = 700;       // ms a step's script must have been running before it counts as done
static const uint32 kIdleSettle = 500;   // ms the game must stay idle before the next step
static const uint32 kMissing = 30000;    // ms to wait for a step's object or line to show up
static const uint32 kChatter = 8000;     // ms after a step that others' talk still counts as busy

// Lua the steps use.  Lua 3.1: no for loops, no local functions.
static const char *kPrelude =
	// Lua the steps use, defined in the game at start and after each load. Lua 3.1: no for loops, no local
	// functions, and closures reach the outer locals only as %upvalues.
	// what keeps the player from the next step: nil when the game waits for Manny
	"guide_busy = function()\n"
	"  local h = system.buttonHandler\n"
	"  local a = system.currentActor\n"
	"  if __guide_task and find_script(__guide_task) then return \"step\" end\n"
	// a dialog's lines on screen wait for the player even inside a cut scene (dialogs run from one)
	"  if guide_choice_up() then return \"choice\" end\n"
	"  if cutSceneLevel and cutSceneLevel > 0 then return \"cutscene\" end\n"
	"  if a and a.hActor and IsMessageGoing(a.hActor) then return \"talking\" end\n"
	"  if find_script(Sentence) then return \"sentence\" end\n"
	"  if a and a.hActor and IsActorMoving(a.hActor) then return \"walking\" end\n"
	"  if h ~= SampleButtonHandler then\n"
	"    if type(h) == \"table\" and h.current_choices and h.current_choices[1] then return \"choice\" end\n"
	"    return \"handler\"\n"
	"  end\n"
	// someone else talking (Glottis snoring, a crowd): waited for only right after a step
	"  if IsMessageGoing() then return \"chatter\" end\n"
	"  return nil\n"
	"end\n"
	// the choices on screen, one text id a line
	"guide_choices = function()\n"
	"  local h = system.buttonHandler\n"
	"  local s = \"\"\n"
	"  if type(h) == \"table\" and h.current_choices then\n"
	"    local i = 1\n"
	"    while h.current_choices[i] do\n"
	"      s = s .. h.current_choices[i].text .. \"\\n\"\n"
	"      i = i + 1\n"
	"    end\n"
	"  end\n"
	"  return s\n"
	"end\n"
	"guide_in_set = function(name)\n"
	"  if system.currentSet == getglobal(name) then return \"1\" end\n"
	"  return \"\"\n"
	"end\n"
	"guide_set = function()\n"
	"  if system.currentSet then return system.currentSet.setFile end\n"
	"  return \"\"\n"
	"end\n"
	"guide_choice_up = function()\n"
	"  local h = system.buttonHandler\n"
	"  local a = system.currentActor\n"
	"  if type(h) == \"table\" and h.current_choices and h.current_choices[1] and not (a and a.hActor and IsMessageGoing(a.hActor)) then\n"
	"    return TRUE\n"
	"  end\n"
	"end\n"
	// the step's script waits for the scripts it starts (Sentence, then the object's own code), or until a dialog
	// it opened waits for a line (its script waits for the whole dialog)
	"guide_wait_for = function(f)\n"
	"  break_here()\n"
	"  while f and find_script(f) and not guide_choice_up() do break_here() end\n"
	"end\n"
	// an object of another set (the walkthrough got out of step): nothing is done, the player stops
	"guide_here = function(o)\n"
	"  if type(o) == \"table\" and (o.owner == system.currentActor or o.owner == manny or o.obj_set == nil or\n"
	"      o.obj_set == system.currentSet) then\n"
	"    return TRUE\n"
	"  end\n"
	"  guide_error = \"not in this set: \" .. tostring(o and o.name)\n"
	"end\n"
	// taken out of the inventory as with the hot keys (inventory_hot_key_pressed)
	"guide_hold = function(item)\n"
	"  local cur = system.currentActor.is_holding\n"
	"  if cur == item then return end\n"
	// in the Land of the Living there is only the scythe, taken out and put back with the inventory key
	"  if system.currentSet == lr then reaper_toggle_scythe() return end\n"
	"  if cur ~= nil and not cur.can_put_away then\n"
	"    if not cur:put_away() then return end\n"
	"  end\n"
	"  if item == nil then\n"
	"    open_inventory(TRUE)\n"
	"    close_inventory(TRUE)\n"
	"    system.currentActor.is_holding = nil\n"
	"    return\n"
	"  end\n"
	"  local r = open_inventory(TRUE, TRUE)\n"
	"  if r ~= NO_HOT_KEY then\n"
	"    manny.is_holding = item\n"
	"    close_inventory()\n"
	"  end\n"
	"end\n"
	// While the player walks Manny to a door (or an object), the doors he crosses on the way don't take him
	// out: the Doorman only lets him through guide_door (none while walking to an object).  The plain trigger
	// boxes are story events and go off as usual.
	"if not guide_game_Doorman then guide_game_Doorman = Doorman end\n"
	"guide_no_door = { }\n"
	"Doorman = function()\n"
	"  if guide_door then\n"
	"    local box_id, box_name = system.currentActor:find_sector_type(HOT)\n"
	"    local o = box_name and system.currentSet[tostring(box_name)]\n"
	"    if o and o ~= guide_door and type(o) == \"table\" and o.interest_actor then\n"
	"      return\n"
	"    end\n"
	"  end\n"
	"  guide_game_Doorman()\n"
	"end\n"
	// a door: opened (or knocked on, when locked), then Manny walks out through it.  Walking into its box runs
	// its walkOut (the Doorman); when the walk ends short of the box, walkOut is run here.
	"guide_walkout = function(door)\n"
	"  if not door.walkOut then return guide_sentence(\"use\", door) end\n"
	"  local set = system.currentSet\n"
	"  local a = system.currentActor\n"
	"  guide_door = door\n"
	"  if door.open and not door:is_open() then\n"
	"    a:walkto_object(door, FALSE)\n"
	"    if not door:open() then\n"
	"      if door.locked_out then guide_wait_for(start_script(door.locked_out, door)) end\n"
	"      guide_door = nil\n"
	"      return\n"
	"    end\n"
	"  end\n"
	"  if door.out_pnt_x then a:walkto_object(door, TRUE) end\n"
	"  break_here()\n"
	"  break_here()\n"
	"  guide_wait_for(Sentence)\n"
	"  guide_wait_for(door.walkOut)\n"
	"  if system.currentSet == set and cutSceneLevel <= 0 then\n"
	"    guide_wait_for(start_script(Sentence, \"walkOut\", door, nil, TRUE))\n"
	"    guide_wait_for(door.walkOut)\n"
	"  end\n"
	"  guide_door = nil\n"
	"end\n"
	// what the use, look and pick up keys do (SampleButtonHandler), waited for to the end
	"guide_sentence = function(verb, n1, n2)\n"
	"  if not guide_here(n1) then return end\n"
	"  if verb == \"walkOut\" or (verb == \"use\" and not n2 and n1.walkOut) then return guide_walkout(n1) end\n"
	"  guide_door = guide_no_door\n"
	// to the object first: the inventory won't open in an elevator's box
	"  if n2 and system.currentActor.is_holding ~= n2 then\n"
	"    if n1.use_pnt_x and n1.owner ~= system.currentActor then system.currentActor:walkto_object(n1) end\n"
	"    guide_hold(n2)\n"
	"  end\n"
	"  local code\n"
	"  if n2 then code = n1[verb .. \"_\" .. trim_header(n2.string_name)] else code = n1[verb] end\n"
	"  guide_wait_for(start_script(Sentence, verb, n1, n2))\n"
	"  if type(code) == \"function\" then guide_wait_for(code) end\n"
	"  guide_door = nil\n"
	"end\n"
	"guide_choose = function(n)\n"
	"  local d = system.buttonHandler\n"
	"  d.selected_line = n\n"
	"  d:update_colors()\n"
	"  sleep_for(600)\n"
	"  guide_wait_for(start_script(d.execute_line, d, n))\n"
	"end\n"
	// the first object of a table that Manny has (and that passes test), the one in his hand first:
	// balloons, bones, ...
	"guide_item = function(tbl, test)\n"
	"  local found\n"
	"  local k, v = next(tbl, nil)\n"
	"  while k do\n"
	"    if type(v) == \"table\" and v.owner == manny and (not test or test(v)) then\n"
	"      if v == manny.is_holding then return v end\n"
	"      if not found then found = v end\n"
	"    end\n"
	"    k, v = next(tbl, k)\n"
	"  end\n"
	"  return found\n"
	"end\n"
	// Manny's idle animation stops on a key (CommonButtonHandler); the idle timer clears the flag each loop, so
	// it is held for a few frames
	"guide_stop_idle = function()\n"
	"  local a = system.currentActor\n"
	"  local i = 0\n"
	"  guide_door = nil\n"
	"  guide_error = nil\n"
	"  a.stop_idle = TRUE\n"
	"  while a.is_idling and i < 60 do\n"
	"    a.stop_idle = TRUE\n"
	"    break_here()\n"
	"    i = i + 1\n"
	"  end\n"
	"end\n"
	"guide_walk = function(x, y, z)\n"
	"  system.currentActor:walkto(x, y, z)\n"
	"  break_here()\n"
	"  while system.currentActor:is_moving() do break_here() end\n"
	"end\n"
	// up (or down) a ladder, all the way: onto it from its base (top), then one climb after another
	"guide_climb = function(l, up)\n"
	"  local a = system.currentActor\n"
	"  local e = l.base\n"
	"  if not up then e = l.top end\n"
	"  if not a.is_climbing then\n"
	"    guide_wait_for(start_script(l.climbOut, l, e.box))\n"
	"  end\n"
	"  local n = 0\n"
	"  while a.is_climbing and n < 300 do\n"
	"    if up then guide_wait_for(start_script(a.climb_up, a)) else guide_wait_for(start_script(a.climb_down, a)) end\n"
	"    guide_wait_for(l.climbOut)\n"
	"    n = n + 1\n"
	"  end\n"
	"  guide_wait_for(l.walkOut)\n"
	"end\n"
	// the current set's touchable objects and exits (for writing a walkthrough: the "objects" command)
	"guide_list = function()\n"
	"  local s = \"\"\n"
	"  local set = system.currentSet\n"
	"  local k, v = next(set, nil)\n"
	"  while k do\n"
	"    if type(v) == \"table\" and v.interest_actor and v.touchable and v.obj_set == set then\n"
	"      s = s .. tostring(k) .. \":\" .. tostring(v.name)\n"
	"      if v.walkOut then s = s .. \"(exit)\" end\n"
	"      s = s .. \"  \"\n"
	"    elseif type(v) == \"table\" and v.walkOut and not v.interest_actor then\n"
	"      s = s .. tostring(k) .. \"(exit box)  \"\n"
	"    end\n"
	"    k, v = next(set, k)\n"
	"  end\n"
	"  return s\n"
	"end\n"
	// the scripts running now, by function name (for finding what a stuck step waits for)
	"guide_scripts = function()\n"
	"  local s = \"\"\n"
	"  local t = next_script(nil)\n"
	"  while t do\n"
	"    s = s .. tostring(FunctionName(identify_script(t))) .. \"; \"\n"
	"    t = next_script(t)\n"
	"  end\n"
	"  return s\n"
	"end\n";

Guide::Guide() : _current(0), _remaining(0), _state(kStopped), _stateSince(0), _idleSince(0), _calmSince(0),
	  _missingSince(0), _prelude(false), _stuckLogged(false), _saveSections(0), _sectionSaved(-1), _pendingSave(-1),
	  _pendingLoad(-1), _controlOffset(0), _retries(0), _speed(1) {
	if (ConfMan.hasKey("guide_file"))
		load(ConfMan.get("guide_file"));
	if (ConfMan.hasKey("guide_control"))
		_controlPath = ConfMan.get("guide_control");
	if (ConfMan.hasKey("guide_save_sections"))
		_saveSections = ConfMan.getInt("guide_save_sections");
	if (ConfMan.hasKey("guide_step"))
		setCurrent(ConfMan.getInt("guide_step"));
	if (ConfMan.hasKey("guide_speed"))
		_speed = MAX(1, ConfMan.getInt("guide_speed"));
	if (ConfMan.hasKey("guide_play") && ConfMan.getBool("guide_play"))
		_state = kWaitIdle;
}

void Guide::logf(const char *fmt, ...) {
	va_list va;
	va_start(va, fmt);
	Common::String s = Common::String::vformat(fmt, va);
	va_end(va);
	debug("guide: %s", s.c_str());
}

bool Guide::parseLine(const Common::String &text, Step &st) const {
	Common::String line = text;
	line.trim();
	st.onlySet.clear();
	st.optional = false;
	// "!STEP": others' talk (a song, a crowd) doesn't hold up the step before it: it starts right away
	st.eager = line.hasPrefix("!");
	if (st.eager) {
		line = line.substr(1);
		line.trim();
	}
	if (line.hasPrefix("@")) {
		size_t sp = line.findFirstOf(' ');
		if (sp == Common::String::npos)
			return false;
		st.onlySet = line.substr(1, sp - 1);
		line = line.substr(sp + 1);
		line.trim();
	}
	if (line.empty() || !strchr("lpouhcCgtxXwW", line[0]) || (line.size() > 1 && line[1] != ' '))
		return false;
	st.kind = line[0];
	if (st.kind == 'C') {
		st.kind = 'c';
		st.optional = true;
	}
	st.arg = line.size() > 2 ? line.substr(2) : Common::String();
	// "STEP => LUA": what must be true once the step is done (else it is done again, twice at most)
	st.expect.clear();
	size_t arrow = st.arg.find(" => ");
	if (arrow != Common::String::npos) {
		st.expect = st.arg.substr(arrow + 4);
		st.expect.trim();
		st.arg = st.arg.substr(0, arrow);
	}
	st.arg.trim();
	return !st.arg.empty();
}

bool Guide::load(const Common::String &path) {
	Common::File f;
	if (!f.open(Common::FSNode(Common::Path(path, '/')))) {
		logf("cannot open %s", path.c_str());
		return false;
	}
	_steps.clear();
	_lua.clear();
	Common::String section;
	bool inLua = false;
	while (!f.eos()) {
		Common::String line = f.readLine();
		// "{{{lua" ... "}}}": Lua of the walkthrough's own (helpers for its puzzles), run after the player's
		if (inLua) {
			if (line.hasPrefix("}}}"))
				inLua = false;
			else
				_lua += line + "\n";
			continue;
		}
		if (line.hasPrefix("{{{lua")) {
			inLua = true;
			continue;
		}
		// comments: "#" at the start of a line, or " ;" after a step
		size_t semi = line.find(" ;");
		if (semi != Common::String::npos)
			line = line.substr(0, semi);
		line.trim();
		if (line.empty() || line.hasPrefix("#"))
			continue;
		if (line.hasPrefix("==")) {
			section = line.substr(2);
			section.trim();
			continue;
		}
		Step st;
		if (!parseLine(line, st)) {
			logf("bad line: %s", line.c_str());
			continue;
		}
		st.section = section;
		section.clear();
		_steps.push_back(st);
	}
	logf("%d steps in %s", _steps.size(), path.c_str());
	return true;
}

void Guide::skipStep() {
	_missingSince = 0;
	setCurrent(_current + 1);
	if (_remaining > 0 && --_remaining == 0)
		_state = kStopped;
}

void Guide::setCurrent(int step) {
	_current = CLIP<int>(step, 0, _steps.size());
	_retries = 0;
	ConfMan.setInt("guide_step", _current);
}

Common::String Guide::luaCall(const char *fn, const Common::String &arg) {
	Common::String res;
	lua_beginblock();
	lua_Object f = lua_getglobal(fn);
	if (lua_isfunction(f)) {
		if (!arg.empty())
			lua_pushstring(arg.c_str());
		if (lua_callfunction(f) == 0) {
			lua_Object r = lua_getresult(1);
			if (r != LUA_NOOBJECT && lua_isstring(r))
				res = lua_getstring(r);
		}
	}
	lua_endblock();
	return res;
}

bool Guide::luaRun(const Common::String &code) {
	return lua_dostring(code.c_str()) == 0;
}

bool Guide::luaTrue(const Common::String &expr) {
	luaRun("__guide_ok = (" + expr + ")");
	lua_beginblock();
	lua_Object r = lua_getglobal("__guide_ok");
	const bool ok = r != LUA_NOOBJECT && !lua_isnil(r);
	lua_endblock();
	return ok;
}

void Guide::definePrelude() {
	luaRun(kPrelude);
	if (!_lua.empty())
		luaRun(_lua);
	// guide_lua: more Lua of the walkthrough's own (or changes to the above), from a file
	if (ConfMan.hasKey("guide_lua"))
		runFile(ConfMan.get("guide_lua"));
	_prelude = true;
}

void Guide::runFile(const Common::String &path) {
	Common::File f;
	if (!f.open(Common::FSNode(Common::Path(path, '/')))) {
		logf("cannot open %s", path.c_str());
		return;
	}
	luaRun(f.readString(0, f.size()));
}

Common::String Guide::busy() {
	lua_beginblock();
	lua_Object f = lua_getglobal("guide_busy");
	Common::String res = "?";
	if (lua_isfunction(f) && lua_callfunction(f) == 0) {
		lua_Object r = lua_getresult(1);
		res = (r == LUA_NOOBJECT || lua_isnil(r)) ? "" : lua_getstring(r);
	}
	lua_endblock();
	return res;
}

Common::Array<Common::String> Guide::choices() {
	Common::Array<Common::String> list;
	Common::String all = luaCall("guide_choices"), line;
	for (uint i = 0; i < all.size(); i++) {
		if (all[i] == '\n') {
			list.push_back(toUtf8(g_localizer->localize(line.c_str())));
			line.clear();
		} else {
			line += all[i];
		}
	}
	return list;
}

int Guide::choiceWith(const Common::String &text) {
	Common::Array<Common::String> list = choices();
	const int num = atoi(text.c_str());
	if (num > 0 && Common::String::format("%d", num) == text)
		return num <= (int)list.size() ? num : 0;
	Common::String want = text;
	want.toLowercase();
	for (uint i = 0; i < list.size(); i++) {
		Common::String c = list[i];
		c.toLowercase();
		if (c.contains(want))
			return i + 1;
	}
	return 0;
}

Common::String Guide::stepCode(const Step &st) {
	Common::String a = st.arg, b;
	size_t sp = a.findFirstOf(' ');
	if (sp != Common::String::npos && (st.kind == 'u')) {
		b = a.substr(sp + 1);
		b.trim();
		a = a.substr(0, sp);
	}
	switch (st.kind) {
	case 'l': return "guide_sentence(\"lookAt\", " + a + ")";
	case 'p': return "guide_sentence(\"pickUp\", " + a + ")";
	case 'o': return "guide_sentence(\"walkOut\", " + a + ")";
	case 'u':
		if (b.empty())
			return "guide_sentence(\"use\", " + a + ")";
		return "if (" + b + ") == nil then guide_error = [[no " + b + "]] else guide_sentence(\"use\", " + a + ", " + b + ") end";
	case 'h':
		if (a == "-")
			return "guide_hold(nil)";
		return "if (" + a + ") == nil then guide_error = [[no " + a + "]] else guide_hold(" + a + ") end";
	case 'g': {
		float x = 0, y = 0, z = 0;
		sscanf(a.c_str(), "%f %f %f", &x, &y, &z);
		return Common::String::format("guide_walk(%f, %f, %f)", x, y, z);
	}
	case 't': {
		float x = 0, y = 0, z = 0;
		sscanf(a.c_str(), "%f %f %f", &x, &y, &z);
		return Common::String::format("system.currentActor:head_look_at_point(%f, %f, %f) sleep_for(1000)", x, y, z);
	}
	case 'x':
	case 'X': return a;
	case 'w': return Common::String::format("sleep_for(%d)", (int)(atof(a.c_str()) * 1000));
	case 'W': return "while not (" + a + ") do break_here() end";
	default: return "";
	}
}

void Guide::save(int slot, const Common::String &desc) {
	g_grim->saveGame(Common::String::format("grim%02d.gsv", slot));
	logf("saving slot %d: %s", slot, desc.c_str());
}

void Guide::tick() {
	const uint32 now = g_system->getMillis();
	pollControl();
	if (!_prelude)
		definePrelude();

	// testing at speed: full screen movies are skipped
	if (_speed > 1 && g_grim->getMode() == GrimEngine::SmushMode && g_movie->isPlaying())
		g_movie->stop();

	const Common::String set = luaCall("guide_set");
	if (set != _lastSet) {
		logf("set %s", set.c_str());
		_lastSet = set;
	}

	if (_pendingLoad >= 0) {
		g_grim->loadGame(Common::String::format("grim%02d.gsv", _pendingLoad));
		logf("loading slot %d", _pendingLoad);
		_pendingLoad = -1;
		_prelude = false;
		_idleSince = 0;
		return;
	}

	Common::String why = busy();
	const bool eagerNext = _current + 1 < (int)_steps.size() && _steps[_current + 1].eager;
	if (why == "chatter" && !(_state == kSettle && now - _stateSince < kChatter && !eagerNext))
		why.clear();
	const bool idle = why.empty();
	const bool choice = why == "choice";
	// an X step (or the one just run) also takes the game's own interfaces (a computer, a panel) as idle
	// (o too: driving the Bone Wagon, holding a rope, the game's own handler is up)
	const bool loose = _current < (int)_steps.size() && isLoose(_steps[_current]) && _state != kSettle;
	const bool looseNow = (_state == kRunning || _state == kSettle) && _current < (int)_steps.size() &&
		(isLoose(_steps[_current]) || (_current + 1 < (int)_steps.size() && isLoose(_steps[_current + 1])));
	if (idle || choice || ((loose || looseNow) && why == "handler")) {
		if (!_idleSince)
			_idleSince = now;
	} else {
		_idleSince = 0;
	}
	// the dialog's lines, once each time they change
	if (choice) {
		Common::Array<Common::String> list = choices();
		Common::String all;
		for (uint i = 0; i < list.size(); i++)
			all += list[i] + "\n";
		if (all != _lastChoices) {
			_lastChoices = all;
			for (uint i = 0; i < list.size(); i++)
				logf("line %d: %s", i + 1, list[i].c_str());
		}
	} else {
		_lastChoices.clear();
	}

	// a save: also while the game's own handler is up (driving)
	if (idle || why == "handler") {
		if (!_calmSince)
			_calmSince = now;
	} else {
		_calmSince = 0;
	}
	if (_pendingSave >= 0 && _calmSince && now - _calmSince >= kIdleSettle) {
		save(_pendingSave, Common::String::format("guide %d", _pendingSave));
		_pendingSave = -1;
		return;
	}

	if (_state == kStopped)
		return;
	if (_state != kWaitIdle && _current >= (int)_steps.size()) {
		_state = kStopped;
		return;
	}

	if (_state == kRunning) {
		if (now - _stateSince < kSettle || why == "step")
			return;
		_state = kSettle;
		_stateSince = now;
	}
	if (_state == kSettle) {
		if (!_idleSince || now - _idleSince < kIdleSettle)
			return;
		if (luaTrue("guide_error")) {
			luaRun("__guide_out = tostring(guide_error)");
			lua_beginblock();
			lua_Object r = lua_getglobal("__guide_out");
			logf("step %d: %s, stopping", _current + 1, lua_isstring(r) ? lua_getstring(r) : "?");
			lua_endblock();
			_state = kStopped;
			return;
		}
		if (!_steps[_current].expect.empty() && !luaTrue(_steps[_current].expect)) {
			if (++_retries <= 2) {
				logf("step %d: not (%s), again", _current + 1, _steps[_current].expect.c_str());
				_state = kWaitIdle;
				return;
			}
			logf("step %d: still not (%s), stopping", _current + 1, _steps[_current].expect.c_str());
			_state = kStopped;
			return;
		}
		if (_steps[_current].kind == 'o' && set == _stepSet && !choice) {
			if (++_retries <= 2) {
				logf("step %d: still in %s, again", _current + 1, set.c_str());
				_state = kWaitIdle;
				return;
			}
			logf("step %d: never left %s, stopping", _current + 1, set.c_str());
			_state = kStopped;
			return;
		}
		_retries = 0;
		logf("step %d done (set %s)", _current + 1, set.c_str());
		setCurrent(_current + 1);
		_state = kWaitIdle;
		if (_remaining > 0 && --_remaining == 0) {
			_state = kStopped;
			return;
		}
	}

	// kWaitIdle: the next step, once the game waits for the player
	if (_current >= (int)_steps.size()) {
		logf("walkthrough finished");
		_state = kStopped;
		return;
	}
	const Step &st = _steps[_current];
	const bool wantChoice = st.kind == 'c';
	if (!_idleSince || now - _idleSince < kIdleSettle) {
		if (!_idleSince && why == "handler" && now - _stateSince > kMissing && !_stuckLogged) {
			logf("step %d: waiting, the game's own handler is up (an X step runs then)", _current + 1);
			_stuckLogged = true;
		}
		return;
	}
	_stuckLogged = false;
	if (wantChoice != choice && !(st.kind == 'X' && choice)) {
		if (st.optional && idle && now - _idleSince > 3000) {
			logf("step %d: no dialog, skipped", _current + 1);
			skipStep();
			return;
		}
		if (!_missingSince)
			_missingSince = now;
		if (now - _missingSince > kMissing) {
			logf("step %d: %s, stopping", _current + 1, wantChoice ? "no dialog" : "a dialog is open");
			_state = kStopped;
			_missingSince = 0;
		}
		return;
	}
	if (!st.section.empty() && _sectionSaved != _current) {
		_sectionSaved = _current;
		int num = 0;
		for (int i = 0; i <= _current; i++)
			if (!_steps[i].section.empty())
				num++;
		logf("section %d at %u ms, step %d: %s", num, now, _current + 1, st.section.c_str());
		if (_saveSections && !choice) {
			save(_saveSections + num - 1, st.section);
			return;   // the save happens at the top of the next frame; the step after it
		}
	}
	if (!st.onlySet.empty() && luaCall("guide_in_set", st.onlySet).empty()) {
		logf("step %d: not in %s, skipped", _current + 1, st.onlySet.c_str());
		skipStep();
		return;
	}
	Common::String code;
	if (wantChoice) {
		const int n = choiceWith(st.arg);
		if (!n) {
			if (!_missingSince)
				_missingSince = now;
			if (st.optional && now - _missingSince > 1500) {
				logf("step %d: no such line, skipped", _current + 1);
				_missingSince = 0;
				skipStep();
			} else if (now - _missingSince > kMissing) {
				logf("step %d: no line '%s', stopping", _current + 1, st.arg.c_str());
				_missingSince = 0;
				_state = kStopped;
			}
			return;
		}
		code = Common::String::format("guide_choose(%d)", n);
	} else {
		code = stepCode(st);
	}
	_missingSince = 0;
	logf("step %d: %c %s", _current + 1, st.kind, st.arg.c_str());
	_stepSet = set;
	// as a key would: Manny's idle animation stops (CommonButtonHandler)
	luaRun("__guide_task = nil");
	if (!luaRun("__guide_fn = function()\nguide_stop_idle()\n" + code + "\nend\n__guide_task = start_script(__guide_fn)")) {
		logf("step %d: Lua error, stopping", _current + 1);
		_state = kStopped;
		return;
	}
	_state = kRunning;
	_stateSince = now;
}

void Guide::onSay(const char *actor, const Common::String &text) {
	logf("talk %s: %s", actor, toUtf8(text).c_str());
}

void Guide::runCommand(const Common::String &text) {
	Common::String line = text;
	line.trim();
	if (line.empty())
		return;
	logf("control: %s", line.c_str());
	size_t sp = line.findFirstOf(' ');
	Common::String c = line.substr(0, sp), rest = sp == Common::String::npos ? "" : line.substr(sp + 1);
	rest.trim();
	const int x = atoi(rest.c_str());
	Step st;
	if (c == "play") {
		_remaining = 0;
		_state = kWaitIdle;
		logf("play from step %d", _current + 1);
	} else if (c == "stop") {
		_state = kStopped;
	} else if (c == "goto") {
		// a step still running is let be, not marked done (play waits for it)
		if (_state == kRunning || _state == kSettle)
			_state = kStopped;
		setCurrent(x > 0 ? x - 1 : 0);
		_sectionSaved = _current;   // no save for the section it starts in
		logf("current step %d", _current + 1);
	} else if (c == "reload") {
		if (_state == kRunning || _state == kSettle)
			_state = kStopped;
		if (ConfMan.hasKey("guide_file"))
			load(ConfMan.get("guide_file"));
		setCurrent(_current);
		definePrelude();
	} else if (c == "luafile") {
		// the player's Lua and then guide_lua again, so that file can wrap the player's functions every time
		definePrelude();
	} else if (c == "lua") {
		// Lua right away, outside any script (no break_here)
		luaRun(rest);
	} else if (c == "print") {
		// a Lua expression's value, to the log
		luaRun("__guide_out = tostring(" + rest + ")");
		lua_beginblock();
		lua_Object r = lua_getglobal("__guide_out");
		logf("= %s", lua_isstring(r) ? lua_getstring(r) : "?");
		lua_endblock();
	} else if (c == "state") {
		logf("set %s, busy '%s', step %d, state %d", luaCall("guide_set").c_str(), busy().c_str(), _current + 1, _state);
		Common::Array<Common::String> list = choices();
		for (uint i = 0; i < list.size(); i++)
			logf("line %d: %s", i + 1, list[i].c_str());
	} else if (c == "sectors") {
		// the set's sectors (walk boxes, exits, cameras) whose name contains the text: type, centre, vertices
		Set *set = g_grim->getCurrSet();
		for (int i = 0; set && i < set->getSectorCount(); i++) {
			Sector *sec = set->getSectorBase(i);
			if (!rest.empty() && !sec->getName().contains(rest))
				continue;
			Math::Vector3d centre(0, 0, 0);
			Common::String pts;
			for (int j = 0; j < sec->getNumVertices(); j++) {
				const Math::Vector3d &v = sec->getVertices()[j];
				centre += v;
				pts += Common::String::format(" %.2f,%.2f", v.x(), v.y());
			}
			if (sec->getNumVertices())
				centre /= sec->getNumVertices();
			logf("sector %s type %d visible %d centre %.3f %.3f %.3f:%s", sec->getName().c_str(), sec->getType(),
				sec->isVisible(), centre.x(), centre.y(), centre.z(), pts.c_str());
		}
	} else if (c == "save") {
		_pendingSave = x;
	} else if (c == "load") {
		_pendingLoad = x;
	} else if (c == "speed") {
		_speed = MAX(1, x);
	} else if (c == "quit") {
		g_grim->quitGame();
	} else if (parseLine(line, st)) {
		const bool inFlight = _state == kRunning || _state == kSettle;
		if (_state != kStopped && _remaining == 0) {
			// while playing: the next step, and play goes on
			_steps.insert_at(MIN<int>(_current + (inFlight ? 1 : 0), _steps.size()), st);
		} else {
			// one step now (after those already queued), as a walkthrough of its own
			const bool queued = _state != kStopped && _remaining > 0;
			_steps.insert_at(MIN<int>(_current + (queued ? _remaining : 0), _steps.size()), st);
			_remaining = queued ? _remaining + 1 : 1;
			if (!queued)
				_state = kWaitIdle;
		}
	} else {
		logf("unknown command");
	}
}

void Guide::pollControl() {
	if (_controlPath.empty())
		return;
	Common::File f;
	if (!f.open(Common::FSNode(Common::Path(_controlPath, '/'))))
		return;
	if (f.size() < (int64)_controlOffset)
		_controlOffset = 0;   // the file was truncated: start again
	if (f.size() == (int64)_controlOffset)
		return;
	f.seek(_controlOffset);
	Common::String text = f.readString(0, f.size() - _controlOffset);
	size_t end = text.findLastOf('\n');
	if (end == Common::String::npos)
		return;
	_controlOffset += end + 1;
	text = text.substr(0, end);
	Common::String line;
	for (uint i = 0; i <= text.size(); i++) {
		if (i == text.size() || text[i] == '\n') {
			runCommand(line);
			line.clear();
		} else {
			line += text[i];
		}
	}
}

} // End of namespace Grim
