#pragma once

// Discovers and loads the mods, and writes the index the UI page reads.
namespace ModHost
{
    // `modsDir` = folder with one subdirectory per mod.
    // `uiDir`   = where shell.html lives; that is where mods.json is written.
    int LoadAll(const char* modsDir, const char* uiDir);
}
