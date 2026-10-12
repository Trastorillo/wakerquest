// SwitchWaker's debug server commands (debug_server.h; docs/DEBUG_SERVER.md; client
// scripts/switch/switchwaker_debug.py). COS_DEBUG_SERVER (the options menu's Depuración > "Servidor de
// depuración": 1, port 6543; a port number in user/settings.ini) starts it right after the settings file is
// read, so a change applies at the next start; it is off by default, and without it the log text kept since
// start is let go.
//
//   info               build, game (disc ID and NRO), frame, stage, memory, applet type, address
//   warps, warp ...    the options menu's travel list (pc_menu.cpp), applied by the game thread once a file is played
//   event NAME         an event of the stage's event list (pc_menu.cpp)
//   give NAME|ITEM ... items through the game's own item-get functions (pc_menu.cpp), once a file is played
//   audio              what JAudio plays (pc_audio_state.cpp): registered sound effects, SE tracks, BGM, counters
//   shot [game]        PNG of the next frame (pc_shot.cpp): with the FPS panel and the menu, or the game alone
//   reload             restart: the forwarder loads the NRO again (cos_switch_restart)
//   quit               end the process (cos_switch_exit)
//   crash              a data abort on the server's connection thread: tests the crash report and the
//                      end of the process with the game and its audio running (cos_switch.cpp's reaper)
// Paths of get / put / ls are relative to /switch/switchwaker (the NRO; native/ holds user/settings.ini and logs/).
#include <arpa/inet.h>
#include <switch.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "cos_switch_internal.h"
#include "debug_server.h"

#ifndef COS_SWITCH_VERSION_STR
#define COS_SWITCH_VERSION_STR "dev"
#endif

extern "C" int pc_debug_warp_list(char* out, size_t size);                                         // pc_menu.cpp
extern "C" int pc_debug_warp(int index, const char* stage, int room, int point, int layer, char* done, size_t size);
extern "C" int pc_debug_event(const char* name);                                                    // pc_menu.cpp
extern "C" int pc_debug_give(const char* const* names, int count, char* done, size_t size);         // pc_menu.cpp
extern "C" int pc_debug_time(float hour);                                                         // pc_menu.cpp
extern "C" int pc_debug_take(const char* const* names, int count);                                 // pc_menu.cpp
extern "C" void pc_debug_weather_clear(void);                                                      // pc_menu.cpp
extern "C" unsigned int pc_debug_audio_request(void);
extern "C" int pc_debug_audio_done(unsigned int ticket, char* out, size_t size);
extern "C" int pc_debug_status(char* out, size_t size);
extern "C" unsigned int pc_debug_shot_request(const char* path, int overlay);                     // pc_shot.cpp
extern "C" unsigned int pc_debug_shot_done(int* ok);

void JKRPcBeginHostAlloc(); // JKRHeap.cpp host allocation scope (pc_jkr_heap.cpp)

namespace {

int gPort = 0;

std::string ipText() {
    in_addr a;
    a.s_addr = (uint32_t)gethostid(); // libnx: the console's address on the network (nifm), network order
    return inet_ntoa(a);
}

debugsrv::Reply info(const debugsrv::Args&) {
    char status[128];
    pc_debug_status(status, sizeof(status));
    u64 total = 0, used = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    const AppletType type = appletGetAppletType();
    char b[704];
    // "game": the disc this NRO runs, whatever its file name on the card (the European build is installed
    // as switchwaker.nro too): deploy refuses to replace it with the other region's NRO
    snprintf(b, sizeof(b), "version %s\ngame %s (%s, build " COS_SWITCH_NRO_NAME ")\n%smemory used %llu MiB of %llu MiB\nrunning as %s\naddress %s:%d\n",
             COS_SWITCH_VERSION_STR, COS_GAME_ID_STR, PC_GAME_REGION_EN, status, (unsigned long long)(used >> 20), (unsigned long long)(total >> 20),
             type == AppletType_Application ? "application (the HOME menu forwarder: reload works)"
                                            : "applet or hbmenu (reload does not work: quit, then start it again)",
             ipText().c_str(), gPort);
    return debugsrv::ok(b);
}

debugsrv::Reply warps(const debugsrv::Args&) {
    static char list[32 << 10];
    const int n = pc_debug_warp_list(list, sizeof(list));
    return debugsrv::ok(std::string(list, (size_t)n));
}

debugsrv::Reply warp(const debugsrv::Args& a) {
    if (a.empty()) {
        return debugsrv::err("usage: warp <number from warps> | warp <stage> [room] [point] [layer]");
    }
    char* end;
    const long index = strtol(a[0].c_str(), &end, 10);
    const bool byIndex = *end == 0 && a.size() == 1;
    if (!byIndex && a[0].size() > 7) {
        return debugsrv::err("a stage name has 7 characters at most");
    }
    char done[80];
    if (!pc_debug_warp(byIndex ? (int)index : -1, a[0].c_str(), a.size() > 1 ? atoi(a[1].c_str()) : 0,
                       a.size() > 2 ? atoi(a[2].c_str()) : 0, a.size() > 3 ? atoi(a[3].c_str()) : -1, done,
                       sizeof(done))) {
        return debugsrv::err("no warp number " + a[0] + " (warps lists them)");
    }
    fprintf(stderr, "[cos] debug server: warp to %s requested\n", done);
    return debugsrv::ok(std::string("warp to ") + done +
                        " requested (it happens once a file is being played and no scene change is in progress)");
}

// "event NAME": an event of the stage's event list, ordered by the game thread (the log says if it ran)
debugsrv::Reply event(const debugsrv::Args& a) {
    if (a.size() != 1) {
        return debugsrv::err("usage: event <name from the stage's event list>, e.g. getperl_jab");
    }
    if (!pc_debug_event(a[0].c_str())) {
        return debugsrv::err("an event name has 31 characters at most");
    }
    fprintf(stderr, "[cos] debug server: event %s requested\n", a[0].c_str());
    return debugsrv::ok("event " + a[0] + " requested (the log says whether it started)");
}

// "give NAME|ITEM ...": queued for the game thread, which runs the game's item-get function of each (the log
// says what the game's own check reports after)
debugsrv::Reply give(const debugsrv::Args& a) {
    if (a.empty() || a.size() > 16) {
        return debugsrv::err("usage: give <name or item number> ... (up to 16): grapple, hookshot, baton, ballad, "
                             "requiem, bombs, sail, telescope, bow, map, compass, or 0x25, 37, ...");
    }
    const char* names[16];
    for (size_t i = 0; i < a.size(); i++) {
        names[i] = a[i].c_str();
    }
    char done[512];
    if (!pc_debug_give(names, (int)a.size(), done, sizeof(done))) {
        return debugsrv::err(std::string("unknown item ") + done + " (a name from 'give' without arguments, or a number 0-254)");
    }
    fprintf(stderr, "[cos] debug server: give %s requested\n", done);
    return debugsrv::ok(std::string("give ") + done +
                        " requested (given once a file is being played; the log says what the game's check reports)");
}

// "time HOUR": the time of day (0-24, e.g. 17.5), set by the game thread once a file is being played
debugsrv::Reply timeCmd(const debugsrv::Args& a) {
    char* end = nullptr;
    const float hour = a.size() == 1 ? strtof(a[0].c_str(), &end) : -1.0f;
    if (a.size() != 1 || end == a[0].c_str() || *end != '\0' || !pc_debug_time(hour)) {
        return debugsrv::err("usage: time <hour 0-24>, e.g. time 17.5 (the sun's glare shows from 8 to 18)");
    }
    fprintf(stderr, "[cos] debug server: time %s requested\n", a[0].c_str());
    return debugsrv::ok("time " + a[0] + " requested (set once a file is being played)");
}

// "weather clear": the sea's random weather back to sunny (the next roll about 12 game hours later)
debugsrv::Reply weatherCmd(const debugsrv::Args& a) {
    if (a.size() != 1 || a[0] != "clear") {
        return debugsrv::err("usage: weather clear");
    }
    pc_debug_weather_clear();
    fprintf(stderr, "[cos] debug server: weather clear requested\n");
    return debugsrv::ok("weather clear requested (the clouds fade over a few seconds)");
}

// "take map|compass ...": the current stage's dungeon map / compass taken away (what 'give' set)
debugsrv::Reply takeCmd(const debugsrv::Args& a) {
    const char* names[4];
    if (a.empty() || a.size() > 4) {
        return debugsrv::err("usage: take map|compass ...");
    }
    for (size_t i = 0; i < a.size(); i++) {
        names[i] = a[i].c_str();
    }
    if (!pc_debug_take(names, (int)a.size())) {
        return debugsrv::err("usage: take map|compass ... (mapa, brujula)");
    }
    fprintf(stderr, "[cos] debug server: take requested\n");
    return debugsrv::ok("take requested (the log says what is left)");
}

// "audio": a line the game thread writes at its next frame end
debugsrv::Reply audio(const debugsrv::Args&) {
    const unsigned int ticket = pc_debug_audio_request();
    char line[1536];
    for (int i = 0; i < 40; i++) { // 2 s
        if (pc_debug_audio_done(ticket, line, sizeof(line))) {
            return debugsrv::ok(std::string(line) + "\n");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return debugsrv::err("the game thread did not answer within 2 s (is it running?)");
}

// "shot [game]": the next frame end writes the PNG (pc_shot.cpp); sent, then deleted
debugsrv::Reply shot(const debugsrv::Args& a) {
    const bool game = !a.empty() && a[0] == "game";
    const char* path = COS_SWITCH_ROOT "/debug_shot.png";
    const unsigned int ticket = pc_debug_shot_request(path, game ? 0 : 1);
    for (int i = 0; i < 400; i++) { // 20 s
        int ok = 0;
        if (pc_debug_shot_done(&ok) >= ticket) {
            if (!ok) {
                return debugsrv::err("the frame could not be read back or written (the log says why)");
            }
            debugsrv::Reply r;
            r.file = path;
            r.removeFile = true;
            return r;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return debugsrv::err("no frame within 20 s (is the game presenting frames?)");
}

debugsrv::Reply reload(const debugsrv::Args&) {
    if (appletGetAppletType() != AppletType_Application) {
        return debugsrv::err("reload needs the HOME menu forwarder (from hbmenu: quit, then start it again)");
    }
    debugsrv::Reply r = debugsrv::ok("restarting: the forwarder loads the NRO from the SD card again");
    r.after = [] {
        debugsrv::stop();
        cos_switch_restart();
    };
    return r;
}

debugsrv::Reply quit(const debugsrv::Args&) {
    debugsrv::Reply r = debugsrv::ok("quitting");
    r.after = [] {
        fprintf(stderr, "[cos] debug server: quit\n");
        debugsrv::stop();
        cos_switch_exit(0);
    };
    return r;
}

debugsrv::Reply crash(const debugsrv::Args&) {
    debugsrv::Reply r = debugsrv::ok("crashing: the log has the report; the process ends within seconds");
    r.after = [] {
        fprintf(stderr, "[cos] debug server: crash requested, writing through a null pointer\n");
        *(volatile u32*)(uintptr_t)0x10 = 0x7777;
    };
    return r;
}

} // namespace

extern "C" void cos_switch_debug_start(void) {
    const char* e = getenv("COS_DEBUG_SERVER");
    if (e == nullptr || *e == '\0' || strcmp(e, "0") == 0) {
        debugsrv::drop_log();
        return;
    }
    gPort = atoi(e) > 1 ? atoi(e) : 6543;
    debugsrv::add_command("info", "info                   build, frame, stage, memory, address", info);
    debugsrv::add_command("warps", "warps                  the travel destinations, numbered", warps);
    debugsrv::add_command("warp", "warp <n> | warp <stage> [room] [point] [layer]", warp);
    debugsrv::add_command("event", "event <name>           order an event of the stage's event list", event);
    debugsrv::add_command("give", "give <name|item> ...   items through the game's item-get (grapple, baton, ballad, bombs, ...)", give);
    debugsrv::add_command("time", "time <hour>            the time of day, 0-24 (e.g. 17.5)", timeCmd);
    debugsrv::add_command("weather", "weather clear          the sea's random weather back to sunny", weatherCmd);
    debugsrv::add_command("take", "take map|compass ...   the stage's dungeon map / compass taken away", takeCmd);
    debugsrv::add_command("audio", "audio                  what JAudio plays: sound effects, SE tracks, BGM, audio thread", audio);
    debugsrv::add_command("shot", "shot [game]            PNG of the next frame (game: without the panel and menu)", shot);
    debugsrv::add_command("reload", "reload                 restart: the forwarder loads the NRO again (put it first)", reload);
    debugsrv::add_command("quit", "quit                   end the program", quit);
    debugsrv::add_command("crash", "crash                  a deliberate crash (tests the crash report)", crash);
    debugsrv::Config c;
    c.port = gPort;
    c.root = "/switch/switchwaker";
    c.log = [](const char* line) { fprintf(stderr, "%s\n", line); };
    // above the game's threads, off its core (core 0): the server answers while the game is busy
    c.threadStart = [] {
        // host memory, never the game's current JKRHeap (its buffers and strings filled a game
        // heap: the server stopped answering after a 35 MB deploy, the game ran on)
        JKRPcBeginHostAlloc();
        svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2C);
        svcSetThreadCoreMask(CUR_THREAD_HANDLE, -1, 0x6);
    };
    if (debugsrv::start(c)) {
        fprintf(stderr,
                "[switch] debug server listening on %s:%d (COS_DEBUG_SERVER; client scripts/switch/switchwaker_debug.py; "
                "local network only, no password)\n",
                ipText().c_str(), gPort);
        // A testing session: no auto-sleep or screen dimming while the game runs (sleep also drops the
        // server's socket). COS_SWITCH_NO_SLEEP=0 in [dev] keeps the console's own timers.
        const char* ns = getenv("COS_SWITCH_NO_SLEEP");
        if (ns == nullptr || ns[0] != '0') {
            const Result r1 = appletSetAutoSleepDisabled(true);
            const Result r2 = appletSetMediaPlaybackState(true);
            fprintf(stderr, "[switch] auto-sleep and dimming off while the debug server runs (rc 0x%x, 0x%x; "
                            "COS_SWITCH_NO_SLEEP=0 keeps them)\n", (unsigned)r1, (unsigned)r2);
        }
    } else {
        debugsrv::drop_log();
    }
}

extern "C" void cos_switch_host_alloc_thread(void) {
    static thread_local bool done = false;
    if (!done) {
        done = true;
        JKRPcBeginHostAlloc();
    }
}

extern "C" const char* cos_switch_version(void) { return COS_SWITCH_VERSION_STR; }

extern "C" int cos_switch_debug_input(uint64_t* buttons, int32_t sticks[4]) {
    if (!debugsrv::running()) {
        return 0;
    }
    const debugsrv::Injected in = debugsrv::injected();
    *buttons |= in.buttons;
    for (int s = 0; s < 2; s++) {
        if (in.stick[s]) {
            sticks[2 * s] = (int32_t)(in.x[s] * JOYSTICK_MAX);
            sticks[2 * s + 1] = (int32_t)(in.y[s] * JOYSTICK_MAX);
        }
    }
    return 1;
}
