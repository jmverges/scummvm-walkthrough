# ScummVM with a walkthrough player

This is a fork of [ScummVM](https://github.com/scummvm/scummvm) that adds a walkthrough player to the SCUMM
engine (the LucasArts adventures and others that run on it) and to the Tinsel engine (Discworld):

- **Easy mode**: the game does the next action of the walkthrough for you, then you carry on.
- **Movie mode**: the game plays itself, from where you are to the end.
- **Recording**: play a game once with recording on, and the walkthrough writes itself.

It works wherever ScummVM does: tested on Linux, and it builds for the PS Vita.

**Watch it play:** whole games played by this fork from the start to the end credits, no commentary, on the
[AI Walkthroughs](https://www.youtube.com/@aiwalkthroughs) YouTube channel:

- [Sam & Max Hit the Road](https://www.youtube.com/playlist?list=PLXl-xJGHwb8E) (Spanish CD), 9 episodes.
- [Day of the Tentacle](https://www.youtube.com/playlist?list=PLO2HvQFzSNW8) (Spanish CD), 11 episodes, going up
  from 11 November 2026.

## Installing it on Linux

Download `ScummVM-Walkthrough.flatpak` from the [releases](https://github.com/jmverges/scummvm-walkthrough/releases).
It is the whole of ScummVM (every engine) plus the walkthrough player, and installs next to an official
ScummVM without touching it.

- **Bazzite, Fedora, SteamOS and other distributions with Flathub:** open the file, or
  `flatpak install --user ScummVM-Walkthrough.flatpak`
- **Ubuntu** (no Flatpak by default):
  ```sh
  sudo apt install flatpak
  flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
  flatpak install --user ScummVM-Walkthrough.flatpak
  ```

It shows up as **ScummVM Walkthrough** in the applications menu, or run
`flatpak run io.github.jmverges.ScummVMWalkthrough`. The walkthroughs in
[dists/walkthroughs](dists/walkthroughs) come with it.

## Using it

The walkthroughs in [dists/walkthroughs](dists/walkthroughs) come with ScummVM, named
`<gameid>[-demo].txt`; a `walkthrough.txt` in the game's folder takes precedence. In the game, open ScummVM's menu (Ctrl+F5, or Start on a
controller): its **Help** button becomes **Walkthrough** when the game has one.

| Button | Does |
|---|---|
| Do the next step | Easy mode: the next action, done for you |
| Play to the end | Movie mode: the game plays itself; open the menu to stop |
| Normal mode | You play again |
| Step back / Step on | If the walkthrough is behind or ahead of your game |
| Controls help | The game's usual help |

Where you are in the walkthrough is kept per game.

## How it works

When you click in a SCUMM game, the game's own input script turns the click into a *sentence*: a verb
and one or two objects ("pick up / mail", "use / key / door"). The engine then walks the actor there and
runs the object's code for that verb. A walkthrough is a list of those sentences, by number, and the
player gives each one to the engine the same way, only when the game is waiting for the player (no
cutscene, nobody talking, the actor standing still). Dialogue choices are verbs, so they are clicks on a
verb.

## Writing a walkthrough

Play the game with recording on, in its section of `scummvm.ini`:

```ini
[samnmax]
guide_record=/home/me/samnmax-walkthrough.txt
```

Every sentence and verb click you make is appended, with the room and the objects' names:

```
s 5 92 0 ; room 9: verb 5 'pile of mail'
```

Then tidy it up: drop the dead ends, add `== Section` lines and readable comments. The format:

```
# comment
== Section title
s <verb> <objectA> [<objectB>] ; what the player sees in the menu
v <verbid> ; a click on a verb (a dialogue choice)
```

Games whose interface is not a verb bar (Sam & Max) need a few more kinds of step:

```
t <actor>                 talk to someone
c <room> <x> <y>          a click on the screen in that room: dialogue icons, the map
i <object>                a click on an inventory or dialogue icon
x <script> <room> [args]  start a room script, as the game's input does
g <object>                walk to an object without using it
w <seconds>               wait
u <actor> <xmin> <xmax>   wait until the actor is in that stretch
o <object> <state> [now]  wait until the object is in that state (a sky car at its stop)
q <object> <class>        wait until the object has that class
a <actor>                 wait until the actor is in the room and standing still
b <bitvar> <value>        wait for a script flag (something the game does at random)
m <name>                  a minigame or a puzzle that changes every game
```

Day of the Tentacle needs one more:

```
n <var> <value>           wait for a game variable (a kite that has to be in the air)
```

The Dig (SCUMM v7) has no verb bar either: a click on something does the default action, a right click looks,
and an item in hand is used on what you click. Its steps are the same `s` sentences plus:

```
c <room> <x> <y> [r|h]    a click (r: right click; h: press and hold, for the light bridge switches)
V <var> <value>           set a game variable (the item in hand, before a click that must not use it)
```

and the minigames `rodlock <firstButton> <shapeVar> <targetVar>` (the rod doors and the map room panel, whose
combinations are random each game), `clickto <object> <var> <value>` (click until something is in that state:
the prisms, the light bridge lenses) and `sliders` (the crystal machine in the lab).

The Curse of Monkey Island (SCUMM v8) uses the same steps (verbs: 6 look, 7 hand, 8 talk, 5 use with; dialogue
lines are verbs 500 and up) and its own minigames: `cannon`, `banjo`, `piracy` (part III: sailing, sea battles
and insult sword fighting until Rottingham is beaten), `waitobj <object>` (the roller coaster scenes) and
`lechuck` (the end).

The minigames are Sam & Max's `wakarat`, `gatorgolf`, `frogrock`, `wires` and `vortex`, and Day of the Tentacle's
`teeth` (the chattering teeth into the grating). The full walkthroughs,
[dists/walkthroughs/samnmax.txt](dists/walkthroughs/samnmax.txt),
[dists/walkthroughs/tentacle.txt](dists/walkthroughs/tentacle.txt) and
[dists/walkthroughs/dig.txt](dists/walkthroughs/dig.txt), use all of them.

Other keys: `guide_file` (a walkthrough somewhere other than the game folder), `guide_step` (where you
are), and for testing without a screen `guide_test` (a script of timed input, screenshots, saves and
loads) and `guide_shots`. For recording videos: `guide_hide_cursor` (the game's cursor is not drawn while
the guide plays) and `guide_save_sections=<slot>` (a save and a log line as each section starts, so a
recording can be picked up again and cut there). The code is in `engines/scumm/guide.cpp`.

## Discworld (Tinsel)

The same idea for the Tinsel engine (`engines/tinsel/guide.cpp`), so far for Discworld (the first game). A Tinsel
game has no verbs: what a player does is a click (walk), a double click (act) or a right click (look) on a
hotspot polygon or an actor, with or without an item in hand, plus inventory and conversation icons. The guide
gives the engine those same events, one at a time when the game is waiting for the player; a step is done when
the game code it started has finished. Polygons (`P12`) and actors (`A3`) are numbered per scene; recording
(`guide_record`) writes the steps with the hotspot names, as for SCUMM.

```
e <walk|act|look> <P<n>|A<n>> [<item>]   an event on a hotspot or actor, with that item in hand
i <walk|act|look> <item> [<held item>]   an event on an item in the inventory
k <icon>                                 a topic in the conversation window
h <item>                                 take an item in hand (0: put it away)
p <1|2>                                  put the held item into the Luggage (1) or Rincewind's pockets (2)
g <x> <y>                                walk there (a click on the floor)
c <x> <y> / r <x> <y>                    a left or right click on the screen
a <scene>                                wait until the game is in that scene
n <global> <value>                       wait for a game variable (someone who comes and goes)
w <seconds>                              wait
?<step>                                  only if its target is here (a walk an event may cut short)
!<step>                                  done as soon as the player has control back (a short chance)
```

The whole game, from a new game to the end credits, is in [dists/walkthroughs/dw.txt](dists/walkthroughs/dw.txt).
There is no menu button for Tinsel yet: the walkthrough is set with `guide_file` and played with the commands in
the file named by `guide_control` (`play`, `stop`, `goto`, `save`, `load`, `objects`...). The fork also fixes two
places where Discworld got stuck in ScummVM: Rincewind back up from the palace dungeon in act 3 stood inside a
blocking polygon and could not move, and Mambo's walk in Lady Ramkin's cage found no path at the cage's edge.

## Broken Sword (sword1)

Broken Sword: The Shadow of the Templars has its own player too (`engines/sword1/guide.cpp`). The game has no verbs
either: a left click on something walks there and uses it or talks, a right click looks, an item from the bar at
the top is used on what you click, and conversations are icons in the bar at the bottom. The guide moves the mouse
and clicks the way a player would, one step at a time when George is waiting; a click on something that is off a
scrolling screen first walks George toward it, and a click on someone behind someone else finds a point where only
that person is under the mouse.

```
e <object> / l <object>       a left click (walk, use, talk) / a right click (look)
i <item>                      take an item from the bar at the top
u <item> <object>             use an item on something
t <item> <item>               use an item on another item
k <subject>                   a subject in a conversation (skipped if the conversation went another way)
s <item>                      in a conversation, show an item
g <x> <y>                     a click on the screen (walk there, or a spot inside something)
a <screen> / n <var> <value>  wait until the game is on that screen / a script variable has that value
y <object> <anim>             wait until someone plays that animation (Ron sneezing in the pub)
z <object> <dirs>             wait until someone stands facing one of those directions (the museum attendant)
w <seconds>                   wait
E, U, S, G, I                 the same as e, u, s, g, i, as soon as the player has control: the few places where
                              the game gives a couple of seconds to act (Khan's handshake, the lion's fang,
                              the brake in the train, the torch at the end)
```

The whole game is in [dists/walkthroughs/bs1.txt](dists/walkthroughs/bs1.txt) (Spanish CD, Windows version).

## Broken Sword II (sword2)

Broken Sword II: The Smoking Mirror plays the same way, with the inventory at the bottom of the screen
(`engines/sword2/guide.cpp`). The ids are the objects' resource ids; the `objects` control command lists what the mouse
can touch on the screen, the items and a conversation's subjects. The same steps as for Broken Sword (`e`, `E`, `l`,
`i`, `u`, `t`, `k`, `g`, `a`, `n`, `w`), and:

```
h <item>                      take an item and hold it: the next click uses it (pouring absinthe at the right moment)
x <object> <min> <max>        wait until the left edge of someone's mouse box is in that range: a pose (the critic
                              holding his glass away, the guard on the boat at one end of his round)
v <object> <ms>               wait until someone has been out of sight that long
```

If George or Nico dies, the guide loads the save made as the section started and plays it again (the recording
script leaves the failed try out). The whole game is in [dists/walkthroughs/bs2.txt](dists/walkthroughs/bs2.txt)
(Spanish CD, Windows version).
## Simon the Sorcerer II (AGOS)

Simon the Sorcerer II has its own player too (`engines/agos/guide.cpp`). The game has verb icons in the panel at the
bottom, the inventory next to them, and conversations as numbered lines in the panel; the guide clicks the verb, the
item, and for "use with" and "give to" the second item once the game asks for it, the way a player would, one step at
a time when the game waits for a click. Exits are spots on the rooms' walking paths that each room's script checks, so
a step can click a path point by its number; an item off a scrolling room first walks Simon toward it, and an item in
the inventory but out of its window scrolls the inventory.

```
e <verb> <item> [<item2>]     a verb (102 look, 103 pick up, 104 give, 105 talk, 106 use, 107 wear, 108 move,
                              109 open; 0 for none: walk), then the item and the second one; an item is its
                              number, or minus a box id for people and things the game draws as named spots
h <box>                       a click on a box (the places on the maps)
k <n> / c <words>             a line of a conversation: the n-th from the top / the one with those words
C <words>                     the same as c, skipped if that line is not offered (the witches do not always talk)
g <x> <y>                     a click on the room (walk there)
p <path> <point>              a click on a point of the room's paths; x <path> <point> for an exit: clicked
                              again until Simon is in another room
@<room> <step>                that step only if Simon is in that room
b <bit> <value>, n <var> <value>, r <room>, w <seconds>   waits
E                             the same as e, as soon as the game takes clicks (the joke book while the seller
                              looks away, the letter while the manager looks for a form, the goblins' drink)
```

The whole game is in [dists/walkthroughs/simon2.txt](dists/walkthroughs/simon2.txt) (Spanish CD, DOS talkie). As
for Tinsel, there is no menu button yet: the walkthrough is set with `guide_file` and played with the commands in
the file named by `guide_control` (`play`, `stop`, `goto`, `save`, `load`, `objects`...); `guide_save_sections`
saves the game at the start of each section, from that slot on.

## Building

The Flatpak: [dists/flatpak-walkthrough](dists/flatpak-walkthrough) is the Flathub recipe for ScummVM
with this fork as its source (it needs Flathub's `shared-modules` next to it).

Linux, with SDL2 and the usual ScummVM dependencies:

```sh
./configure --disable-all-engines --enable-engine=scumm,tinsel
make
```

PS Vita, with the [VitaSDK](https://vitasdk.org) Docker image:

```sh
mkdir build-vita && cd build-vita
../configure --host=psp2 --disable-all-engines --enable-engine=scumm --enable-release
# vitasdk's SDL2 is built on vitaGL:
echo 'LIBS += -lvitaGL -lvitashark -lSceShaccCgExt -lmathneon -ltaihen_stub -lSceShaccCg_stub -lSceKernelDmacMgr_stub -lSceIme_stub -lSceAppMgr_stub -lSceCommonDialog_stub -lSceGxm_stub -lSceDisplay_stub -lstdc++' >> config.mk
make && make psp2vpk
```

On the Vita it needs `ur0:data/libshacccg.suprx`, as every vitaGL app does.

## Status

- **Sam & Max Hit the Road** (CD, Spanish): the whole game, from a new game to the end credits, in one run
  (about 1 h 40 min). [Videos](https://www.youtube.com/playlist?list=PLXl-xJGHwb8E).
- The Sam & Max DOS CD demo: from the office to the car.
- **Day of the Tentacle** (CD, Spanish): the whole game in one run (about 2 h).
  [Videos](https://www.youtube.com/playlist?list=PLO2HvQFzSNW8), from 11 November 2026.
- **Discworld** (CD, Spanish text, English voices): the whole game to the end credits.
  [Videos](https://www.youtube.com/playlist?list=PLNyWh5U81jok), from 22 November 2026.
- **The Dig** (CD, Spanish voices and text): the whole game to the good ending in one run (2 h 33 min).
  [Videos](https://www.youtube.com/playlist?list=PLdaVV4BLBwS8), from 3 December 2026.
- **The Curse of Monkey Island** (CD, Spanish voices and text, Mega Monkey): the whole game to the end credits
  ([dists/walkthroughs/comi.txt](dists/walkthroughs/comi.txt)).
  [Videos](https://www.youtube.com/playlist?list=PLUmwk1Mn6GFY), from 7 December 2026.
- **Broken Sword: The Shadow of the Templars** (CD, Spanish voices and text): the whole game to the end credits
  ([dists/walkthroughs/bs1.txt](dists/walkthroughs/bs1.txt)).
- **Broken Sword II: The Smoking Mirror** (CD, Spanish voices and text): the whole game to the end credits
  ([dists/walkthroughs/bs2.txt](dists/walkthroughs/bs2.txt)).
- **Simon the Sorcerer II** (CD, Spanish voices and text): the whole game to the end credits in one run
  (3 h 40 min) ([dists/walkthroughs/simon2.txt](dists/walkthroughs/simon2.txt)).

Other games are next. The sister project, the [Hollywood Monsters port](https://github.com/jmverges/hollywood-monsters-port),
has its own walkthrough player; its videos are on the same channel
([playlist](https://www.youtube.com/playlist?list=PLI5UB10ipMHw)).

## About ScummVM

Everything else is [ScummVM](https://www.scummvm.org/), which plays classic adventure games and RPGs on almost
any device, as long as you have the game data files. Its own README is [README.scummvm.md](README.scummvm.md).
