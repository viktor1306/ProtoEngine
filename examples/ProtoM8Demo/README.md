# Proto M8 Demo

This compact scene uses the original deterministic M2 glTF/GLB fixtures.
The imported PBR and MASK materials are arranged beside a ground plane,
a warm moving lamp, a cool shadowed lamp, and a SunCaster.

## C++ controls

The code in `Code/Behaviors.cpp` demonstrates Spin, MoveLight, and KeyboardMove.
W/A/S/D move the free camera; Q/E move it vertically; arrow keys look around;
Shift increases camera speed. Space raises the green cube by 0.5 m.
Play uses the current scene snapshot and Stop restores the editor state.
