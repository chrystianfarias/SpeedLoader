#include "ModHost.h"

#include "JsRuntime.h"
#include "core/Log.h"

#include <windows.h>
#include <stdio.h>
#include <string>
#include <vector>

namespace
{
    // file:///D:/.../ui/index.html - the shell fetches each mod's UI fragment,
    // and for that it needs a URL, not a Windows path.
    std::string ToFileUrl(const std::string& path)
    {
        std::string url = "file:///";
        for (size_t i = 0; i < path.size(); i++)
        {
            char c = path[i];
            if (c == '\\')      url += '/';
            else if (c == ' ')  url += "%20";
            else if (c == '#')  url += "%23";
            else if (c == '?')  url += "%3F";
            else                url += c;
        }
        return url;
    }

    std::string JsonEscape(const std::string& s)
    {
        std::string out;
        for (size_t i = 0; i < s.size(); i++)
        {
            char c = s[i];
            if (c == '"' || c == '\\') { out += '\\'; out += c; }
            else if (c == '\n')        out += "\\n";
            else                       out += c;
        }
        return out;
    }
}

namespace ModHost
{
    int LoadAll(const char* modsDir, const char* uiDir)
    {
        std::string pattern = std::string(modsDir) + "\\*";
        WIN32_FIND_DATAA fd;
        HANDLE find = FindFirstFileA(pattern.c_str(), &fd);
        if (find == INVALID_HANDLE_VALUE)
        {
            Log("no mod folders in %s", modsDir);
            return 0;
        }

        std::vector<std::string> entries;   // mods.json lines
        int count = 0;

        do
        {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (fd.cFileName[0] == '.') continue;

            std::string dir = std::string(modsDir) + "\\" + fd.cFileName;
            Js::Mod* mod = Js::Load(dir);
            if (!mod) continue;

            count++;
            if (Js::ModUi(mod).empty()) continue;

            std::string uiPath = dir + "\\" + Js::ModUi(mod);
            entries.push_back(
                "  {\"id\": \"" + JsonEscape(Js::ModId(mod)) +
                "\", \"name\": \"" + JsonEscape(Js::ModName(mod)) +
                "\", \"url\": \"" + JsonEscape(ToFileUrl(uiPath)) + "\"}");
        }
        while (FindNextFileA(find, &fd));
        FindClose(find);

        // The index sits next to shell.html, which fetches it by relative path.
        std::string indexPath = std::string(uiDir) + "\\mods.json";
        FILE* f = fopen(indexPath.c_str(), "w");
        if (f)
        {
            fputs("[\n", f);
            for (size_t i = 0; i < entries.size(); i++)
            {
                fputs(entries[i].c_str(), f);
                fputs(i + 1 < entries.size() ? ",\n" : "\n", f);
            }
            fputs("]\n", f);
            fclose(f);
        }
        else
        {
            Log("could not write %s", indexPath.c_str());
        }

        Log("%d mod(s) loaded, %d with a UI", count, (int)entries.size());
        return count;
    }
}
