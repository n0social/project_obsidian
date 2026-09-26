project_obsidian UI overlay
===================

Files in this APK folder (and in the tablet's files/obsidian_ui/ directory)
are copied onto extracted Vanilla data after every extract or import.

They live under Data/override and Data/expansions/classic/override. The engine
loads override first, so extract cannot revert in-game Interface art to stock
WoWee/Vanilla copies.

Tablet login / realm / character screens are also hardcoded in the engine
(C++ ImGui). Extracting MPQs does not replace those layouts.

To add your own replacements, copy files into files/obsidian_ui/ using the
same relative paths as the extract, for example:

  Interface/Buttons/UI-Quickslot2.blp
  Interface/Glues/loading.blp
