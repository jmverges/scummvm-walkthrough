# ScummVM with a walkthrough player

This is a fork of [ScummVM](https://github.com/scummvm/scummvm) that adds a walkthrough player to the SCUMM
engine (the LucasArts adventures and others that run on it):

- **Easy mode**: the game does the next action of the walkthrough for you, then you carry on.
- **Movie mode**: the game plays itself, from where you are to the end.
- **Recording**: play a game once with recording on, and the walkthrough writes itself.

It works wherever ScummVM does: tested on Linux, and it builds for the PS Vita.

**Watch it play:** the whole of Sam & Max Hit the Road (Spanish CD), played by this fork from the start to the end
credits, is on the [AI Walkthroughs](https://www.youtube.com/@aiwalkthroughs) YouTube channel:
[the playlist](https://www.youtube.com/playlist?list=PLXl-xJGHwb8E), 9 episodes, no commentary.

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

The minigames are Sam & Max's: `wakarat`, `gatorgolf`, `frogrock`, `wires` and `vortex`. The full Sam & Max
walkthrough, [dists/walkthroughs/samnmax.txt](dists/walkthroughs/samnmax.txt), uses all of them.

Other keys: `guide_file` (a walkthrough somewhere other than the game folder), `guide_step` (where you
are), and for testing without a screen `guide_test` (a script of timed input, screenshots, saves and
loads) and `guide_shots`. For recording videos: `guide_hide_cursor` (the game's cursor is not drawn while
the guide plays) and `guide_save_sections=<slot>` (a save and a log line as each section starts, so a
recording can be picked up again and cut there). The code is in `engines/scumm/guide.cpp`.

## Building

The Flatpak: [dists/flatpak-walkthrough](dists/flatpak-walkthrough) is the Flathub recipe for ScummVM
with this fork as its source (it needs Flathub's `shared-modules` next to it).

Linux, with SDL2 and the usual ScummVM dependencies:

```sh
./configure --disable-all-engines --enable-engine=scumm
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

Other SCUMM games are next. The sister project, the [Hollywood Monsters port](https://github.com/jmverges/hollywood-monsters-port),
has its own walkthrough player; its videos are on the same channel
([playlist](https://www.youtube.com/playlist?list=PLI5UB10ipMHw)).
