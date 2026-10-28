#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <deque>

using namespace geode::prelude;

// How it works:
//  1. pushButton / releaseButton are intercepted and put in a queue instead of reaching the game.
//  2. Every physics tick (processCommands), the oldest queued input is tested:
//     save a checkpoint, apply the input, step the game a few ticks, and see if the player dies.
//     Then the checkpoint is loaded again, so the test leaves no trace.
//  3. If the test survives, the input is sent to the game for real. If not, it waits one more tick.
//  4. After the max delay it is sent anyway (or dropped, if that setting is on).
// During a test, PlayLayer::destroyPlayer only records the death and does not kill the player.

namespace {

struct Pending {
    bool down;
    int button;
    bool isP1;
    int waited;
};

std::deque<Pending> g_queue;
bool g_probing = false;   // a test is running
bool g_injecting = false; // we are sending an input to the game ourselves
bool g_probeDied = false;

bool on(char const* key) {
    return Mod::get()->getSettingValue<bool>(key);
}

int num(char const* key) {
    return static_cast<int>(Mod::get()->getSettingValue<int64_t>(key));
}

bool active(GJBaseGameLayer* layer) {
    if (!on("enabled")) return false;
    auto pl = typeinfo_cast<PlayLayer*>(layer);
    if (!pl || pl != PlayLayer::get()) return false;
    if (!pl->m_player1 || pl->m_player1->m_isDead) return false;
    return true;
}

void inject(PlayLayer* pl, Pending const& ev) {
    g_injecting = true;
    if (ev.down) pl->pushButton(ev.button, ev.isP1);
    else pl->releaseButton(ev.button, ev.isP1);
    g_injecting = false;
}

// true = applying this input now does not kill the player within the lookahead
bool probeSafe(PlayLayer* pl, Pending const& ev, float dt) {
    g_probing = true;
    g_probeDied = false;

    auto cp = pl->createCheckpoint();
    if (!cp) {
        g_probing = false;
        return true; // cannot test, do not block the input
    }
    cp->retain();

    inject(pl, ev);

    int horizon = num("horizon-ticks");
    for (int i = 0; i < horizon && !g_probeDied; i++) {
        pl->processCommands(dt);
    }
    bool safe = !g_probeDied;

    pl->loadFromCheckpoint(cp);
    cp->release();

    g_probing = false;
    g_probeDied = false;
    return safe;
}

void pump(PlayLayer* pl, float dt) {
    if (g_queue.empty()) return;

    Pending ev = g_queue.front();
    bool timedOut = ev.waited >= num("max-delay-ticks");

    if (!timedOut && !probeSafe(pl, ev, dt)) {
        g_queue.front().waited++;
        return;
    }

    g_queue.pop_front();

    if (timedOut && on("drop-on-timeout")) return;

    inject(pl, ev);
    if (on("debug-log")) {
        log::info("{} button {} (p1={}) released after {} ticks{}",
            ev.down ? "press" : "release", ev.button, ev.isP1, ev.waited,
            timedOut ? " (timed out)" : "");
    }
}

} // namespace

class $modify(TickBufferBase, GJBaseGameLayer) {
    void pushButton(int button, bool isP1) {
        if (!g_injecting && !g_probing && active(this)) {
            g_queue.push_back({true, button, isP1, 0});
            return;
        }
        GJBaseGameLayer::pushButton(button, isP1);
    }

    void releaseButton(int button, bool isP1) {
        if (!g_injecting && !g_probing && active(this)) {
            // Keep order: if presses are waiting, the release must wait behind them.
            if (on("buffer-releases") || !g_queue.empty()) {
                g_queue.push_back({false, button, isP1, 0});
                return;
            }
        }
        GJBaseGameLayer::releaseButton(button, isP1);
    }

    void processCommands(float dt) {
        if (!g_probing && !g_queue.empty()) {
            if (active(this)) pump(typeinfo_cast<PlayLayer*>(this), dt);
            else g_queue.clear();
        }
        GJBaseGameLayer::processCommands(dt);
    }
};

class $modify(TickBufferPlay, PlayLayer) {
    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (g_probing) {
            g_probeDied = true;
            return; // only a test, do not actually kill the player
        }
        PlayLayer::destroyPlayer(player, object);
    }

    void resetLevel() {
        g_queue.clear();
        g_probing = false;
        g_probeDied = false;
        PlayLayer::resetLevel();
    }

    void onQuit() {
        g_queue.clear();
        g_probing = false;
        PlayLayer::onQuit();
    }
};
