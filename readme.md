## This is a branch of the Zelda3 pc port by snesrev: https://github.com/snesrev/zelda3    

This branch is an attempt to add unfinsished changed to the Zelda 3 project

Tested and still builds acording to the wiki: https://github.com/snesrev/zelda3/wiki    

Original snesrev repo that has been fully commented, the base for this new repo: https://github.com/xander-haj/z3c


## Changes    
- old README.md renamed to `BUILD.md`    
    
- fixed windscreen mode

- can now re-arrange the in game HUD while `ExtendedAspectRatio` is higher than 4:3, allowing HUD elements to be placed anywhere

- Y,X,L,R Buttons now have individual Item Boxes in the HUD with button labels applied

- New in game settings menu, allows anything, except for aspect ratio, to be modified in real time, aspect ratio changes still require restart for now

## Compromises

- when re-arrange HUD is enabled, the Magic Meter uses the same base outline for botht he base and powered up state, this is because the `2` in the 1/2 above the bar sahres a sprite tile with the left corner of the item box, will hopefully fix to use proper fixed graphic soon
