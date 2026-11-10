2D fighting game based on projectiles and portals.

A match consists of as many rounds as it takes for a player to get three kills.
A tie (players dying on the same frame) awards no kills.
When a player dies everything is reset. 

Portals
- Static map objects that can face in 4 directions.
- There are no map edges, if we want wrap-around, use a portal there.
- Momentum should be preserved through portals, though direction changes.
- Players should be able to re-enter quickly if they are holding towards the portal. 
- Portals are always linked in pairs.

Players 
- Players have 3 health.
- Can shoot when in the air.
- Separate inputs for moving and aiming. 
- 8 directional aim.
- When not aiming, movement direction is used, if not moving, facing direction is used.
- Aim is decided when the player presses fire, not when the startup is over.
- No invuln after getting hit.
- Players can pass through each other.
- Players have a maximum speed.
- Cannot act when in hitstun.
- Getting hit in startup does not cancel the shot.
- Players outside the map die. 
- If in hitstun while reloading, it continues but you cannot active reload until hitstun is over.

Gun 
- Shooting should have a short startup.
- Bullets disappear when hitting non-portal terrain. 
- Bullets should have some knockback and hitstun. 
- Fire rate will be slow, number of bullets in magazine low
- Reload happens automaitcally when you fire your last bullet. 
- Active reload where pressing fire at a fixed set of frames partway through the reload reloads instantly, missing it increases reload time. 
- Bullets should have a maximum lifetime.
- Bullets outside the map disappear.

Things to explore (none required, may never get to these)
- Mouse input aiming (may mess with rollback)
- Players placing portals
- Melee option?
- Parry?
- Limited ammo you need to pick back up?
- Movement options (double jump/dash)?
- Power ups?
- Bouncing bullets?
- Self-damage?
- 8-directional portals?
- Should bullets slightly track the player? Or be very large? Hitting the other player can be difficult with 8-directional inputs.
- Recoil moving the player who fires?
- Should projectiles have to be charged a bit? Could help with rollback.
- Should bullets have some gravity?