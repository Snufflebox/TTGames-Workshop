LEGO Star Wars TCS 4-player mod - mod folder
=============================================

Custom characters
-----------------
Put each character in its own folder under 4p_mods\chars\, for example:

    4p_mods\chars\DinDjarin\DINDJARIN.TXT
    4p_mods\chars\DinDjarin\DINDJARIN_PC.GHG
    4p_mods\chars\DinDjarin\DINDJARIN_ICON_PC.GSC
    4p_mods\chars\DinDjarin\*.AN3, *.BSA   (animations)

File names may vary: the mod uses the folder's character .txt (the one with
a matching *_PC.GHG; TEST.TXT is used only if nothing else fits) and the
folder's .GSC as icon. That is all. When the game starts, the mod adds every folder here to the
character list, the collection (Free Play) and the language file. Nothing in
GAME.DAT is changed.

The name shown in game is the folder name with spaces added
("DinDjarin" -> "Din Djarin"). To change it, or to sell the character in the
Cantina shop instead of having it unlocked from the start, add a file
4p_char.ini to the character's folder:

    name=Din Djarin
    price=50000

Limits for now: the game holds 340 characters and uses 319 itself, so up to 21
custom characters; up to 12 of them can be shop items. Folders starting with
"_" are ignored.

Replacing game files
--------------------
Any file placed in 4p_mods\ under its game path is used instead of the game's
copy, e.g. 4p_mods\stuff\text\english.txt.

4p_mods\_gen\ is written by the mod (merged lists) and can be deleted at any
time. Messages go to 4p_mod.log in the game folder.
