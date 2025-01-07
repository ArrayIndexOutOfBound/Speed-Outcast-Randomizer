# Speed Outcast - Randomizer

Fork of SpeedOutcast, a source port of the singleplayer of Star Wars Jedi Knight II: Jedi Outcast.
Focuses on staying true to the original game and SpeedOutcast / speedrun.com rules to be used for speedrun purposes.
See the [speedrun.com leaderboard](https://www.speedrun.com/jk2) for more information.

Based on the original source code released by Raven, see `README.txt`.
Based on the source port of Star Wars Jedi Knight II: Jedi Outcast named SpeedOucast, see `README - SpeedOutcast.md`.


## New Console Variables

### Randomizer

#### `cg_enableRandomizer` (0 or 1)

Enable or disable the NPC and items randomizer.
Default: `0`.

#### `cg_enableRandomizerEnhancements` (0 or 1)

Enable or disable the other more 'exotic' randomized elements in the game (check list below for more details).
Need `cg_enableRandomizer` to be 1 to work.
Default: `0`.

#### `cg_setSeed` 10 digits string

Variable that store the current seed. Can be edited before starting a run and used if `cg_useSetSeed` is equal to 1.
Default: `EmptyString`.

#### `cg_useSetSeed` (0 or 1)

If 0, generate a random seed during the start of a new game.
If 1, will use the seed stored in `cg_setSeed`.
Default: `0`.

### HUD

#### `cg_drawSeed` (0 or 1)

Draw the seed used during a run. Will draw "No Rand" is `cg_enableRandomizer` is 0
Default: `0`.


## List of addition for the base Randomizer

#### Gameplay

- Every NPC will be randomized during spawn, with exceptions listed below.
- Every items spawned on the map such as items on the ground or in gun racks will be randomized.
- The lightsaber can be obtained early and you will receive a random saber style on pickup.
- Every Force powers (except Saber attack and Saber defense) can be obtained via holocrons.
- NPCs replacing NPCs with keys or special trigger for script will inherit said keys and/or script.
- NPCs replacing normal NPCs will inherit their AI as best as possible (droids can't pathfind exactly like a humanoid).
- Easter egg NPC called `The Death Wiggle`.
- The NPC you kill at the end of `yavin_trial` will trigger the end of the timer like in SpeedOutcast

#### Exceptions

- Lando, Jan, Luke, MonMothman, Reelo, Desann, Ugnaught, 
- ATSTs, Seekers, MouseDroids, R5D2 (bespin) and 'Chicken Robots' will always spawn as themselves.
- The warden in `artus_detention` will always be an humanoid.
- The stormtrooper shooting at the prisoners in `artus_topside` will always be humanoids.
- Rodian, Gran, Weequay at the end of `ns_starpad` (with Reelo) and the waves when Lando will go inside the Lady Luck will always be humanoids.
- The two Imperial Officers in `cairn_assembly` and `doom_detention` we have to mindtrick will always be humanoids.
- Kyle as in player controlled Kyle will always be Kyle. And randomly spawned Kyle will always be considered allies.

### List of addition for the 'exotic' Randomizer

- Randomized saber lenght every time the saber is chosen.
- Randomized saber color every time the saber is chosen.
- TODO : Random jump height 
- TODO : Random sounds
- TODO : Random NPC size (may not be possible because of insane hitbox and things)
- TODO : Random projectiles every shot


