## This is maintenance branch of the [Zelda3 PC Port by SNESREV](https://github.com/snesrev/zelda3)

The original project was abounded by the developer ever since so this branch is an attempt to add unfinsished changes and continue the project.

Original snesrev repo that has been fully commented, refer to the [base](https://github.com/xander-haj/z3c) for this new repo.

Old Readme from the SNESREV repo is available [here](https://github.com/xander-haj/Z3R/blob/main/OLD_README.md)

Builds still compile and run properly, for more information on building instructions and FAQs, refer to [this wiki](https://github.com/snesrev/zelda3/wiki)

Some features might be broken, you can check the [beta builds](https://github.com/xander-haj/Z3R-Beta) if you have issues with the stable builds.

## Current changes    
      
- Fixed windscreen mode.

- You can now re-arrange the in game HUD while `ExtendedAspectRatio` is higher than 4:3, allowing HUD elements to be placed anywhere.

- Y,X,L,R Buttons now have individual Item Boxes in the HUD with button labels applied.

- New in-game settings menu (Press Q on your keyboard to bring it up), allows anything, except for aspect ratio, to be modified in real time, aspect ratio changes still require restart for now.

## Compromises

- When re-arrange HUD is enabled, the Magic Meter uses the same base outline for both the base and powered up state, this is because the `2` in the 1/2 above the bar shares a sprite tile with the left corner of the item box, will hopefully fix to use proper fixed graphic soon.
