#include "Bridge.h"

#include "core/Log.h"
#include "host/HostApi.h"
#include "js/JsRuntime.h"
#include "ui/CefHost.h"

#include <windows.h>
#include <deque>
#include <string>

namespace
{
    struct Message
    {
        std::string channel;
        std::string json;
    };

    CRITICAL_SECTION g_lock;
    std::deque<Message> g_queue;
    bool g_ready = false;

    // A buggy mod can flood this in a loop; dropping with a warning beats
    // growing without bound until the game chokes.
    const size_t kMaxQueue = 512;

    void OnMessage(const std::string& channel, const std::string& json)
    {
        EnterCriticalSection(&g_lock);
        if (g_queue.size() >= kMaxQueue)
        {
            g_queue.pop_front();
            static DWORD lastWarn = 0;
            DWORD now = GetTickCount();
            if (now - lastWarn > 1000)
            {
                lastWarn = now;
                LogJs("UI queue full: dropping the oldest messages");
            }
        }
        Message m;
        m.channel = channel;
        m.json = json;
        g_queue.push_back(m);
        LeaveCriticalSection(&g_lock);
    }
}

namespace Bridge
{
    void Install()
    {
        InitializeCriticalSection(&g_lock);
        g_ready = true;
        CefHost::SetMessageHandler(OnMessage);
    }

    void Drain()
    {
        if (!g_ready) return;

        for (;;)
        {
            Message m;
            EnterCriticalSection(&g_lock);
            bool has = !g_queue.empty();
            if (has) { m = g_queue.front(); g_queue.pop_front(); }
            LeaveCriticalSection(&g_lock);
            if (!has) break;

            // Native panels first: their channels belong to no mod, and
            // JsRuntime would only log them as addressed to nobody.
            if (Host::HandleUiMessage(m.channel, m.json)) continue;

            Js::DispatchUiMessage(m.channel, m.json);
        }
    }

    void Shutdown()
    {
        if (!g_ready) return;
        g_ready = false;
        Host::Shutdown();
        CefHost::SetMessageHandler(nullptr);
        EnterCriticalSection(&g_lock);
        g_queue.clear();
        LeaveCriticalSection(&g_lock);
    }
}
