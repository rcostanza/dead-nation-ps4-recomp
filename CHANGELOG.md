# Changelog

## v0.5

- Cheat menu: press L3+R3 (X+M on the keyboard) to pause the game and pick from invulnerability, instant dash recovery, unlock all weapons and items, infinite money, ammo and items, no reload, max weapon upgrades and unlock all armors. Triangle turns them all off. Choices are remembered between sessions
  - ⚠️ Warning: some cheats unlock permanent upgrades, like all armor and weapons. These are saved to your save file and cannot be undone unless a new save file is created

## v0.4

- Possible fix for the game crashing at the end of a level, when the PC can't hold the FPS cap (seen at 4K and 144 FPS)
- Added a `debug_speed` setting (settings.ini, 2 to 4) that runs the game faster, for testing

## v0.3

- No need to run the setup again after updating: the game prepares itself on its first start
- Starting the game before it's set up (or after its folders moved) opens the setup
- The 1.03 update is now required, and the setup refuses game files other than the CUSA00236 dump with its 1.03 update
- Local co-op support: the game should now recognize a second connected controller

## v0.2

- Retuned game speed logic according to the FPS cap; rate of fire should now be consistent regardless
- Quitting to the main menu no longer drops the game to 30 FPS in slow motion
- Added 360 FPS cap option. Note: you might need to reduce rendering resolution to achieve that, it's a taxing game
- The game no longer slows down when your PC can't keep up with the FPS cap (down to 30 FPS)
- Borderless window is now a real window: alt-tab no longer makes the screen flash or switch modes

## v0.1

- First release
