# Nightreign Invasions

> **Very early stage.** For now this is mostly a proof of concept. Invasions should work, but
> things break, the game can hang or crash, and many features are still missing. Play with it only
> if you're fine with that.
>
> **Testers welcome!** Try it with your friends and
> [report the bugs you find](https://github.com/oisis/nightreign-pvp/issues). Every report helps.

A fan-made mod for **ELDEN RING NIGHTREIGN** (PC, Steam) that brings back what Elden Ring had and
Nightreign doesn't: **other players invading your expedition**.

You set out with your squad as usual. Somewhere in the world, another player may arrive as a red
invader with one goal: to bring your expedition down. Or you can be the one who invades.

<!-- Screenshot: the Invasion entry in the expedition menu -->

## How it works

- **Start from the Roundtable Hold.** In the expedition menu, the Target list gets a new first
  entry: **Invasion**. Pick it and press **Begin invasion**. The mod looks for an expedition you
  can invade and puts you into the host's world.
- **You are the red invader.** You arrive on the hawk like everyone else, but you are hostile to
  the host's squad from the first second. You see the same world, enemies and bosses they do.
- **Downed is not dead.** As in normal Nightreign, a downed player can be brought back by their
  teammates. An invader who keeps hitting a downed player finishes them off.
- **When the host falls, the expedition is over.** The invader wins and goes home.
- **When the invader falls,** they return to their own Roundtable Hold and can start another
  invasion.

<!-- Screenshot: an invader arriving in the host's world -->

## Planned

These are the rules the mod is built towards. Not all of them work yet:

- **Your choice, set once.** A setting decides whether your expeditions can be invaded. Nobody asks
  you mid-game, and it can't be switched off halfway through a run.
- **Fair line-ups.** How many invaders can join depends on the size of your squad: 1 invader
  against 1 or 2 players, up to 2 invaders against 3 or 4 players.
- **Squads of four.** "Number of Players" gets a fourth option.
- **Invader progression.** An invader starts at a level between the squad's lowest and highest.
  Enemies ignore the invader, and the invader can't hurt them. Runes and levels come only from
  defeating squad members, though the invader may still loot chests and items.

## Is it safe?

- **The mod never connects to FromSoftware's servers.** It runs the game offline, without
  Easy Anti-Cheat, like every other Nightreign mod. A guard built into the mod blocks the game's
  own attempts to reach official servers. Players find each other through Steam only. You never
  meet players of the unmodded game.
- **Your normal save is not touched.** The mod uses its own save file. The launcher also makes a
  backup of your save before it starts.
- **Nothing in the game's folder is changed.** The launcher builds the mod's files from your own
  copy of the game and keeps them in its own folder. To remove the mod, press **Uninstall** in the
  launcher and delete its folder.

## What you need

- ELDEN RING NIGHTREIGN on Steam, **game version 1.3.3**. After a game update the mod stays inactive
  until it is updated as well.
- Windows, or Linux (Steam Deck included) with the game running through Proton.
- Steam running.
- Everyone you play with must use **the same version** of the mod.

## How to play

1. Download the package for your system from the
   [Releases](https://github.com/oisis/nightreign-pvp/releases) page:
   `NightreignInvasions-<version>-windows.zip` or `NightreignInvasions-<version>-linux.zip`.
2. Unpack it to any folder.
3. Start the launcher: `NightreignInvasions.exe` on Windows, `nightreign-invasions` on Linux.
4. Press **Play with the mod**. The first start takes a moment while the mod's files are built.

<!-- Screenshot: the launcher window -->

## Something went wrong?

Please [open an issue](https://github.com/oisis/nightreign-pvp/issues) and describe what happened
and what you were doing at the time. The mod writes a log to `work/native/invasion.log` inside the
launcher's folder. Attach that file to your report.

---

*Nightreign Invasions is an unofficial fan project. It is not made or supported by FromSoftware
or Bandai Namco. It contains no game files: everything it needs is built from your own copy of
the game. It includes the [me3](https://github.com/garyttierney/me3) mod loader (MIT /
Apache-2.0).*
