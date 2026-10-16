# Custom characters and the 4p_mods folder

Part of the [four-player patch](README.md) DLL (`dinput8.dll`), but independent of the
player-3/4 changes: it adds community characters to the Steam PC game without
changing `GAME.DAT` or any other game file. Player instructions are in
[MODS_README.txt](MODS_README.txt), which goes into the `4p_mods` folder.

Status: up to 21 custom characters played in game on one machine (Free Play
select, Cantina visitors, party, icons). Evidence labels as in the README.

## What a player does

1. Put each character in its own folder under `4p_mods\chars\` in the game folder,
   in the usual community layout: `NAME.TXT`, `NAME_PC.GHG`, `NAME_ICON_PC.GSC` and
   the `.AN3`/`.BSA` animations.
2. Optionally add `4p_char.ini` with `name=` (display name; the default is the folder
   name with spaces before capitals) and `price=` (sold in the Cantina shop instead of
   unlocked from the start).
3. Start the game. The characters appear in the collection, the Free Play grid, the
   Cantina and the shop.

Any other file placed under its game path in `4p_mods\` (for example
`4p_mods\stuff\text\english.txt`) is used instead of the game's copy.

## How it works

Everything happens at run time, in memory or in `4p_mods\_gen\`, which the loader
writes and which can be deleted at any time.

### 1. Every file open goes through the loader

`0x4FA3F0(name, mode, archive, wait)` opens every game file: the `.DAT` archives
first, the disk second. Its first instruction is replaced by `fopen_hook`
(`custom_chars.s`), which calls the loader's `file_hook(name, out)`. When that
returns 1, the file is opened from disk under the path in `out` instead
(archive 0); otherwise the game continues as before.

`file_hook` answers, in order:

| The game asks for | Served from |
| --- | --- |
| `chars\chars.txt` | `_gen` copy: the game's list plus one `char_start` / `dir` / `file` / `char_end` block per custom character |
| `chars\collection.txt` | `_gen` copy plus `collect "<file>"`, with `buy_in_shop <price>` when a price is set |
| `stuff\text\<language>.txt` | `_gen` copy plus `<id> "<name>"` for each character, on text ids from 352 up that the file does not use |
| `chars\<Folder>\<file>.txt` | `_gen` copy of the character's own `.txt` with `name_id=` set to that text id (and `icon=` when the icon was renamed, below) |
| `chars\<Folder>\<file>_pc.ghg` | the folder's model, or its `_gen\models` copy when it needed a fix (below) |
| `stuff\icons\<icon>_pc.gsc` | the folder's icon file, or its `_gen\icons` copy |
| any other `<name>` | `4p_mods\<name>` if that file exists |

The base files are read with the game's own file functions (`read_game`), so the
lists are always built from the installed game. `4p_mods` is listed once into an
in-memory index, so a file open costs a hash lookup instead of disk lookups. The game
opens files on two threads (the main one, and a job thread `0x4FECF0` that brings
Cantina visitors in); `file_hook` lets one thread through at a time, and the same
thread coming back in (`read_game`) gets the original file.

### 2. Folders are read once, at the first file open

`scan_customs` looks at every folder in `4p_mods\chars\` (folders starting with `_`
are skipped):

- **Character file.** A folder may hold several `.txt` files (`TEST.TXT`,
  leftovers). The one with a matching `<name>_PC.GHG`, named like the folder and not
  `TEST`, is used.
- **Model under another name.** Packs sometimes ship `T3-M4_PC.GHG` next to
  `T3_M4.TXT`. A model whose name matches when only letters and digits are compared is
  served under the name the game asks for.
- **Model fixes** (copy in `_gen\models`, written only when something changed):
  - Node names exported with Maya namespaces (`MaceWindu_SE_019:head`) are cut back to
    `head`, which is what the game looks for. Each name stays at its offset in the
    name table, so nothing else in the file moves.
  - The file starts with a texture table (u16 count at `+4`, then per texture a
    24-byte record `{width, height, levels, format, ?, size}` and its DDS data). The game
    passes the record's width and height to `D3DXCreateTextureFromFileInMemoryEx`
    (`0x6020B6`), which rescales the picture if they differ from the DDS. One pack
    (Jolee Bindo) gives a 2048x2048 texture a size of `0x08000800 x 0x08000000`: D3DX
    scaled it up to the device maximum, a 30 s freeze. Records whose size disagrees
    with their DDS header are corrected; records with a width of 0 or less (cube maps,
    other layouts) end the scan.
- **Icon fixes** (copy in `_gen\icons`). An icon `.GSC` names its two pictures inside
  (NTBL: `<name>1` locked silhouette, `<name>` colour) and the game finds them by the
  character's `icon=` name. Packs often copy another icon file without renaming it
  inside (a whole KotOR pack ships every icon as `anakin_jedi_icon`), so every
  character showed the same picture or none. Such a file gets every occurrence of the
  inside name replaced by a unique name of the same length (`m4p` + folder index +
  `x` padding), and the character's `icon=` is set to it.

### 3. Limits raised

| Limit | Where | Change |
| --- | --- | --- |
| Icon table, 750 entries (custom characters add two each; full after about six) | registered by `0x579E70` from `0x7F44F8` (`0x4CA14C`..`0x4CA160`) | copied into the mod area at `+0x4000` with 0x800 entries; name space 8000 → 0x6000 bytes; 4 references relocated |
| Animation-name table, 0x400 entries | `0x4CA48C` | `push 0x1000`. The Cantina stops loading visitors when this table is full; with custom characters it filled (log: 0x3FF) |
| Characters, 340 (319 used by the game) | `chars\chars.txt` limit, `0x4CA2EF` | not raised: at most 21 custom characters; extra folders are skipped and logged |

### 4. Icons stay loaded

Base characters' pictures live in `stuff\things.gsc`, loaded once at start-up into
the permanent heap. A custom character's picture lives in its own icon file, which
the game loads into the level heap only for characters in the party or walking
around (`0x55B6E0`), and the Free Play grid resolves its pictures when a level loads
(`0x57A000`) from whatever happens to be loaded then: custom pictures came and went.

`call 0x5EF7F0` at `0x48AAD4` (the start-up load of `things.gsc`) goes through
`things_load`, which then calls the loader's `icon_preload()`: every custom icon file
is loaded with `0x5EF7F0` into a heap the loader allocates and never resets. The
start of `0x55B6C0` (icon scene of character n) jumps to `char_icon_scene`, which
asks the loader's `icon_for_char(n)` first and falls back to the game's per-level
array `[0x93B270]`.

### 5. Unlocked from the start

The character flags `[0x93B288]` are reset whenever a save loads, so every frame
`custom_watch` sets custom characters without a price to owned and available (bits
1 and 2), and priced ones to available (bit 2), which puts them in the shop.

## Native notes

Found while building this (unpacked Steam executable, see [RESEARCH.md](RESEARCH.md)
for the rest):

- Character count with the base data: 0x13F (319) of 340.
- Character limits: `chars\chars.txt` loads into `[0x93B274]` with at most
  0x154 (340) entries (`0x4CA2EF`), count `[0x93B278]`; the saved character
  flags `0x86E5C0` are 340 bytes; Enter Code keeps 0x90 (144) hashes; the shop
  character list stops at 100 entries (`0x4C1880`), bought bits 128.
- Game files: `0x4FA3F0(name, mode, archive, wait)` opens a file (archive
  `[0x91EE88]` first via `0x4F9BD0`, then the disk device `[0x800478]`);
  size `0x4F6B40(h)`, read `0x4FB550(h, buf, n)`, close `0x4F99E0(h)`, exists
  `0x4FA610`. Text files: `0x4FC860(name)`. Lists: chars\chars.txt (max 340,
  clamp 500 in `0x6307C0`), chars\collection.txt, chars\customiser.txt,
  chars\specialmoves.txt, chars\charstxt.fpk (all character .txt files),
  stuff\text\ (language files).
- Dumped lists (0.32-research): chars\chars.txt is `char_start` /
  `dir "x"` / `file "y"` / `char_end` blocks, 319 of them; chars\collection.txt
  is `collect "name" [story | area_complete "area" | buy_in_shop N |
  cheat_code "abc123" | minikit]`, 185 lines, 88 buy_in_shop, 86 cheat codes;
  no keyword = unlocked from the start. stuff\text\english.txt: UTF-8 with
  BOM, `id "text"` lines, ids 20..2008 with gaps (352-379 free). Character
  .txt files are not opened one by one (they come from chars\charstxt.fpk)
  except ones missing there (chars\SanSweet\SanSweet.txt); icons are
  stuff\icons\<icon>_pc.gsc.
- Icon table: `0x579E70(table 0x7F44F8, heap, heap, 750, 8000)`; entries
  {type, name}; static 315, the rest added by character .txt files (icon=,
  `0x5875A0`: name and a second variant, type 3, only while `[0x9553C8]`);
  count `[0x951BBC]`, max `[0x951BC0]`. 741 used with the base game.
  Portraits resolve icons per level into world+0x2AC0 (`0x57A000`); type-3 icons
  come from the character's own icon file (`0x55B6E0`, stuff\icons\<name>.gsc).
- Custom character folder (community format, e.g. DINDJARIN): NAME.TXT
  (name_id = text index, icon, layers, weapon, anim_start blocks), NAME_PC.GHG
  model, NAME_ICON_PC.GSC icon, animations as .AN3 and .BSA.
- Icon files: `0x55B6E0(list, 1, &level_cur 0x924890, &level_end 0x924894)` loads
  per-character icon scenes into the level heap (40 MB, `[0x7EF020]`, reset per level);
  `stuff\things.gsc` (all base icons) is `[0x924880]`, loaded once into the permanent
  heap `0x924878/0x92487C` at `0x48AAD4`. `0x5EF7F0(&cur, end, name)` loads a scene.
  Locked portraits use icon index + 1 (`<name>1`) when the character is not owned.
- Icon .GSC: one DDS, NTBL chunk `NTBL, size, count, "<name>1\0<name>\0default_string"`;
  `<name>1` is the locked silhouette node, `<name>` the colour node; `0x5CF510`
  matches these node names (case-insensitive) against the icon table name.

## Hooks

| Name | Address | Original bytes | Replacement |
| --- | --- | --- | --- |
| File open | `0x4FA3F0` | `81 EC 04 01 00 00` | jmp `fopen_hook`; loader `file_hook` |
| Icon table name space | `0x4CA14C` | `68 40 1F 00 00` | `push 0x6000` |
| Icon table size | `0x4CA151` | `68 EE 02 00 00` | `push 0x800` |
| Icon table address | `0x4CA160` | `68 F8 44 7F 00` | jmp `icon_reloc` (copies the static entries to `+0x4000`) |
| Icon table references | `0x419F21`, `0x419F41`, `0x4C9E2F`, `0x4CA77C` | operands `0x7F44F8`/`0x7F44FC` | moved to `+0x4000` |
| Animation-name table | `0x4CA48C` | `68 00 04 00 00` | `push 0x1000` |
| things.gsc load | `0x48AAD4` | `E8 17 4D 16 00` | call `things_load`; loader `icon_preload` |
| Icon scene of character | `0x55B6C0` | `8B 44 24 04 85 C0` | jmp `char_icon_scene`; loader `icon_for_char` |

Mod area: `+0x1F50` `file_hook`, `+0x1F54` `icon_preload`, `+0x1F58` `icon_for_char`
(pointers set by the loader), `+0x4000` icon table.

## Source

- [src/custom_chars.s](src/custom_chars.s): the hook code above, included at the end of
  `hooks.s`.
- [src/dll/custom_chars.c](src/dll/custom_chars.c): `file_hook`, the folder scan, list
  and text generation, model and icon fixes, the file index, icon preloading and
  `custom_watch`; included by `loader.c`.

## Log

`4p_mod.log` lists the folders found, every fix (`icon renamed`, `texture sizes fixed`,
`model copy written`), the files served from `4p_mods`
(first 150, animations not listed), each custom character's table entry once the
game has built it, and the Cantina visitor limits (`spawn animsets ... objects`).

## Not done

- More than 21 custom characters (the 340-character table is not moved).
- Custom vehicles, levels or story characters.
- Files added to `4p_mods` while the game runs are not seen until a restart.
