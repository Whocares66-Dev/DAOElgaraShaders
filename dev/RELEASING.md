# Releasing

A release is a tag, `v<version>`, and on GitHub the zip the mod is installed from: `DAOElgaraShaders-<version>.zip`, the plugin's folder laid out as the game's (`tools\package.ps1`). The guide's site builds that version's pages from the tag. The process is DAOTurnBased's and DAOPhotoCam's (`tools/release.ps1`, `tools/package.ps1` and `tools/daolib_release.ps1`, copied 2026-10-16 after 0.1.1, whose release was made by hand).

## The version

`project(DAOElgaraShaders VERSION x.y.z)` in `CMakeLists.txt` is the one place it is written, with `vcpkg.json`'s `version-string` beside it; the zip's name takes it from there. It is set on the work branch, with the rest of a release's preparation; `release.ps1` does not bump it.

- **patch**: fixes and better looks, with the options and their file as they were.
- **minor**: something a player would call new: an option, a feature.
- **major**: something a player must do something for: an option renamed or gone.

## DAOLib

A release holds `extern\DAOLib` at one of DAOLib's own release tags, and `release.ps1` refuses at any other commit. A game has a DAOLib release's loader and address files, and nothing newer: a plugin built on a later commit may ask for an ID those files lack, or be of a plugin API that loader refuses. Held at the tag, the mod needs "DAOLib x.y.z or later", which the changelog's section and the zip's `VERSION.txt` name. After moving the submodule, `git submodule update --init --recursive`: DAOLib's own submodule is otherwise left at the old commit.

## What the zip holds

The plugin's folder, `bin_ship\daolib\plugins\elgarashaders\`, as the release build makes it:

- `elgarashaders.dll` and `elgarashaders.pdb`: the PDB lets a crash log name the plugin's frames.
- `materials.answers` and `materials.names`: what each of the game's materials is given, and the tables a mod's are classed by.
- `override\`: the game's shaders rebuilt from the installed game, SMAA's lookup textures, and the metal maps.
- `README.md`, `LICENSE.txt`, `SMAA-LICENSE.txt` and `VERSION.txt`: the release, and the DAOLib it was built on with its plugin API.

**The add-ins' materials come from a dump outside git**: `build/addins`, which `tools/dump_addins.py` writes from a running game (the add-ins' archives are enciphered). The materials step reads it at `<checkout>/build/addins`, and in a checkout without it (a new worktree) the build silently leaves out the add-ins' answers and metal maps: 89 answers and 21 maps fewer than 0.1.1's, the Blood Dragon armor among them (seen 2026-10-16). Release from a checkout that has it, or copy it in: `release.ps1` refuses without it, and the materials step reruns when the dump's files change. Before tagging, `release.ps1` compares the zip with the last release's (`dist\` or the GitHub release), file by file, and lists what is new, changed and gone; anything gone refuses the release unless `-AllowDropped` says it is meant. `-DryRun` shows the same comparison against the build tree as it stands.

Not `graphics.chosen`: the options are the player's, kept across an update. Nothing in the zip may name the user's folder; `package.ps1` refuses one that does.

## Steps

1. On a work branch: the version in `CMakeLists.txt` and `vcpkg.json`, and a `## x.y.z` section in `CHANGELOG.md`, which becomes the release's notes. `extern/DAOLib` at a DAOLib release tag.
2. The checks (`CLAUDE.md`: the build with its tests, the formatters and the linters, and the change seen in the game), then merge to master.
3. `.\tools\release.ps1 -DryRun`, then `.\tools\release.ps1`. It builds the release preset with its tests, makes `dist\DAOElgaraShaders-x.y.z.zip`, tags `vx.y.z`, pushes master and the tag together (the tag's push adds x.y.z to the guide's version selector), and creates the GitHub release `DAOElgaraShaders x.y.z` with the zip. Below 1.0 it is a prerelease.
4. The Nexus page (https://www.nexusmods.com/dragonage/mods/7061): the zip uploaded as the new version, and `MOD_PAGE.bbcode` if the description changed.

If `gh release create` fails after the push, run it alone with `--notes-file build\release-notes-x.y.z.md` and the zip.
