# Outlast VR takeover baseline

This package is the last confirmed working base:

- **Test28** renderer, OpenXR startup, stereo, menus, body anchoring, and controls.
- **Test16** calibrated visible upper-body IK. Both arms follow Quest controller poses during ordinary gameplay.

The separately supplied Test28 build and the currently installed working DLL share this SHA-256:

`A329AEAD137F7830CBE2B090FD92D4CEEC84985ADE9B15FE8F05D1F0738EF607`

## Build

Install Visual Studio 2022 or Build Tools with **Desktop development with C++**, then run:

```text
scripts\build.bat
```

The output is `builds\d3d9.dll`. End-user deployment files belong in the separate build ZIP.

## Important scope

Later experimental work after Test28 is deliberately **not included**. It introduced a generated low-poly camcorder model, failed physical-interaction hooks, and a cinematic detector that could disable gameplay IK. Do not use those experiments as a starting point.

The next developer should preserve the renderer and visible-IK hooks unchanged while implementing physical interactions one mechanic at a time, validating each against the exact `OLGame.exe` build before merging it.

## Known baseline behavior

- Main menu and authored presentation use the configured comfort/cinema screen.
- Normal gameplay uses the VR projection path with upper-body controller IK.
- Native animations remain necessary for authored scenes and interactions.
- Button mappings remain the compatibility layer until a physical mechanic is individually validated.
