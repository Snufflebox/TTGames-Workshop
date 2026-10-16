/* LEGO Star Wars: The Complete Saga (PC) - custom characters and the 4p_mods file
   overlay, hook code. Included at the end of hooks.s (inside .text2); see
   ../CUSTOM_CHARACTERS.md. The loader's C side is dll/custom_chars.c. */

/* ================= game files: 4p_mods overlay =================
   0x4fa3f0(name, mode, archive, wait) opens every game file (.DAT archive first, disk
   second). The loader's file_hook(name, out) logs the name and returns 1 with out =
   "4p_mods\<name>" when that file exists in the game folder; the open then goes to
   disk with that name (archive 0). Replaces sub esp,0x104 @4fa3f0. */
.set M_FILEFN, M_BASE+0x1f50
fopen_hook:
    cmp dword ptr [M_FILEFN], 0
    je 8f
    sub esp, 0x104                         /* out name buffer */
    mov eax, esp
    push eax
    push dword ptr [esp+0x10c]             /* name (arg 1) */
    call dword ptr [M_FILEFN]
    add esp, 8
    test eax, eax
    jz 7f
    mov eax, esp
    push dword ptr [esp+0x114]             /* wait (arg 4) */
    push 0                                 /* archive: none -> disk */
    push dword ptr [esp+0x114]             /* mode (arg 2) */
    push eax
    call fopen_orig
    add esp, 0x114
    ret
7:  add esp, 0x104
8:  sub esp, 0x104
    jmp 0x4fa3f6
fopen_orig:
    sub esp, 0x104
    jmp 0x4fa3f6

/* ================= icon table: 0x800 entries =================
   0x579e70(table, heap, heap, max, name bytes) registers the static icon table 0x7f44f8
   (315 entries, room for 750) and character .txt files add their icons to it. Custom
   characters add two each, and the table filled after about six of them (log 0.33:
   later ones had icon index -1). The table is copied to the mod area (+0x4000, 0x800
   entries; references patched in build.py). Replaces push 0x7f44f8 @4ca160. */
.set M_ICONTAB, M_BASE+0x4000
icon_reloc:
    push esi
    push edi
    push ecx
    mov esi, 0x7f44f8
    mov edi, M_ICONTAB
    mov ecx, 750*2
    rep movsd
    pop ecx
    pop edi
    pop esi
    push M_ICONTAB
    jmp 0x4ca165


/* ================= custom character icons kept loaded =====================================
   Right after stuff\things.gsc loads into the permanent heap (call 0x5ef7f0 @48aad4), the
   loader's icon_preload() loads every custom character's icon file into its own heap.
   0x55b6c0(char) ("icon file of character n") asks the loader's icon_for_char() first,
   so the Free Play grid, the Cantina and the level-load resolve (0x57a000) always find
   custom pictures. Replaces call 0x5ef7f0 @48aad4 and the start of 0x55b6c0. */
.set M_ICONPRE, M_BASE+0x1f54
.set M_ICONFOR, M_BASE+0x1f58
things_load:
    push dword ptr [esp+12]
    push dword ptr [esp+12]
    push dword ptr [esp+12]
    call 0x5ef7f0
    add esp, 12
    cmp dword ptr [M_ICONPRE], 0
    je 1f
    push eax
    call dword ptr [M_ICONPRE]
    pop eax
1:  ret

char_icon_scene:
    cmp dword ptr [M_ICONFOR], 0
    je 2f
    push ecx
    push edx
    push dword ptr [esp+12]
    call dword ptr [M_ICONFOR]
    add esp, 4
    pop edx
    pop ecx
    test eax, eax
    jnz 9f
2:  mov eax, dword ptr [esp+4]
    test eax, eax
    jl 8f
    cmp eax, dword ptr [0x93b278]
    jge 8f
    mov ecx, dword ptr [0x93b270]
    mov eax, dword ptr [ecx+eax*4]
    ret
8:  xor eax, eax
9:  ret
