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

## The rules of a run

### Session types

Pick one in **Number of Players** in the expedition menu at the Roundtable Hold.

| Type | Who can join |
|---|---|
| **Solo** | Nobody. A plain solo run. |
| **Solo + invaders** | One invader (1v1). No summons. |
| **Team** | Up to 2 summons (a squad of 3) and up to 2 invaders: 1v1, 2v1, 2v2, 3v1 or 3v2. Invaders can't be turned off. |

### Changes to the normal game

| Rule | Applies to | How it works |
|---|---|---|
| The map | Everyone | Every map is two special regions out of four: Rotted Woods, Mountaintop, Crater and Noklateo. |
| Your own pace | Everyone | No rain circle closes in. The day clock stops at 18:00 and night only comes when the squad challenges the night boss. |
| Looting | Everyone | Items, chests and merchants work as in the normal game. |
| The challenge, day 1 | Host and summons | Defeat 6 field bosses (bosses with a large health bar, in castles, camps and the like) and at least one region special boss. |
| The challenge, day 2 | Host and summons | Defeat 6 more field bosses, or as many as are left on the map if fewer remain, and both region special bosses must be dead. |
| Region special bosses | Everyone | Rotted Woods: Lordsworn Captain. Mountaintop: the dragon. Crater: Magma Wyrm. Noklateo: Astel, Naturalborn of the Void. |
| Challenge the Night | Host and summons | Buy the item from the field merchant. Once the condition is met, using it takes the whole squad straight to the night boss's arena and night falls. Used too early, it stays in the pouch. The merchant sells it again on day 2. |
| The night boss fight | Everyone | From the challenge on, every other boss and world event is closed. A ring of fire surrounds the arena. |
| Stronger enemies | Everyone | Extra bosses make all enemies stronger for the rest of the expedition, night bosses and the final boss included. Day 1: every field boss past the 6th adds +10 % HP and damage. Day 2: every field boss past the day's requirement adds +15 %, on top of day 1. Region special bosses add nothing. |
| Enemy scaling | Everyone | Only the host and the summons count as players for the game's enemy scaling. Invaders don't. |
| Extra lives | Host | The game's own extra lives work for a solo host. Without one, the host's death ends the expedition as lost. |
| Late summons | Summons | A summon who joins a running expedition appears next to the host. If the host is level 4 or higher, the summon gets 80 % of the host's level and fewer HP flasks (host 1–9: 1, 2, 3, 3, 4, 4, 5, 6, 7). |
| A summon's death | Summons | A dead summon goes home. It can be summoned again, and then joins as a late summon. |
| Invading | Invader | Pick **Invasion** in the expedition menu's Target list and press **Begin invasion**. The mod finds an expedition to invade. |
| Arrival | Invader | The invader arrives in the host's world on the hawk, hostile from the first second, and can attack at once or loot first like the host. |
| Level | Invader | The invader takes the host's level. |
| Winning | Invader | The host's final death ends the expedition and the invader wins. A host who gets up from an extra life is not a win yet. |
| Killing a summon | Invader | The invader who finishes a summon gets 10,000 runes. |
| Downed players | Everyone | A downed player can be revived by their team. An invader who keeps hitting a downed player finishes them off. |
| Loot after a death | Invader and summons | A dead invader's or summon's weapons (with skill and passives), talismans and quick-slot items fall to the ground where they died. Anyone can pick them up. |
| Runes after a death | Invader and summons | A dead invader's or summon's runes are lost for good. No bloodstain is left. |

### Proximity voice chat (new, off by default)

Turn it on in the launcher's **Settings**. Every player in the session must have it on to hear and be heard.

| Key | Who hears you |
|---|---|
| **V** (hold to talk) | Every player near you, invaders included: full volume up to 5 m, fading out at 40 m. The voice comes from the speaker's side. |
| **B** (hold to talk) | Your team at any distance: the host and the summons. Invaders can't use or hear it. |

- Range, volume, both keys and a list of muted players can be changed in **Settings**.
- On the Steam Deck, map a back button to V (and B) in Steam Input.
- The microphone is the one chosen in Steam's own voice settings.
- The mod records only while a key is held. Voice goes only to the players in your session, over Steam, and is never saved.

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

## Links

- [Nexus Mods](https://www.nexusmods.com/eldenringnightreign/mods/967)
- [Discord](https://discord.gg/nGDhbA2aNV)
