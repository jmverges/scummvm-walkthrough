# ScummVM with a walkthrough player

This is a fork of [ScummVM](https://github.com/scummvm/scummvm) that adds a walkthrough player to the SCUMM
engine (the LucasArts adventures and others that run on it):

- **Easy mode**: the game does the next action of the walkthrough for you, then you carry on.
- **Movie mode**: the game plays itself, from where you are to the end.
- **Recording**: play a game once with recording on, and the walkthrough writes itself.

It works wherever ScummVM does: tested on Linux, and it builds for the PS Vita.

## Using it

Put a `walkthrough.txt` in the game's folder (there is one for the Sam & Max demo in
[dists/walkthroughs](dists/walkthroughs)). In the game, open ScummVM's menu (Ctrl+F5, or Start on a
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

Other keys: `guide_file` (a walkthrough somewhere other than the game folder), `guide_step` (where you
are), and for testing without a screen `guide_test` (a script of timed input, screenshots, saves and
loads) and `guide_shots`. The code is in `engines/scumm/guide.cpp`.

## Building

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

Early. The Sam & Max DOS CD demo is played from the office to the car by its walkthrough. Walkthroughs
for full games are next.
