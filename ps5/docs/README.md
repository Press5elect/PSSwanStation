# PSSwanStation documentation

What exists, and where. The repository is SwanStation's tree with the PS5 title in `ps5/`;
everything about the title is under `ps5/`.

## For players

| Document | What it holds |
| --- | --- |
| [`../MANUAL.md`](../MANUAL.md) | The manual: every screen and setting, troubleshooting |
| [`../README.txt`](../README.txt) | The short guide that ships in the title folder: installing, controls, the library, the main features |
| [`../CHANGELOG.txt`](../CHANGELOG.txt) | What changed in each build (ships in the title folder) |
| [`../releases/`](../releases/) | Each release's notes, as published |
| [`../PRIVACY.md`](../PRIVACY.md), [`../LEGAL.txt`](../LEGAL.txt) | What leaves the console; the legal notes (both ship in the title folder) |
| [`../licenses/README.txt`](../licenses/README.txt) | Every part's licence |

## For people and agents working on it

| Document | Read it when |
| --- | --- |
| [`dev/workflow.md`](dev/workflow.md) | Before any change: the owner's rules, the repositories, the loop for a change, handing over, lessons |
| [`dev/building.md`](dev/building.md) | Setting up the build from nothing, the pins, a build and a test build |
| [`dev/testing.md`](dev/testing.md) | Checking a change on the PC: the host build, the screenshot run, `ps5/tests` |
| [`dev/releasing.md`](dev/releasing.md) | Making a release |
| [`dev/open-work.md`](dev/open-work.md) | What is untested on the console, ideas not taken up, decisions on record |

## Elsewhere in the tree

- [`../patches/README.md`](../patches/README.md): our commits on PS5_Mesa, PS5_Vulkan and
  PS5_VKHomebrewUI, as patches on revisions GitHub has.
- [`../elevator/README.txt`](../elevator/README.txt): the sandbox elevator (USB disc drives).
- `../tools/*.sh`, `../tools/*.py`: each tool's use is in its head (build, package, release,
  shaders, notices, test discs, the game database).
- `../tests/*/run.sh`: each PC test's use is in its head.
- Code comments say what a behaviour is for and, often, the build it came with ("build 27: ...").
