#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <deque>

using namespace geode::prelude;

// How it works:
//  1. queueButton (where touches enter the game) is intercepted and the input is kept in our own
//     queue instead of reaching the game.
//  2. At the start of every physics tick (processCommands), the oldest queued input is tested:
//     save a checkpoint, apply the input, step the game a few ticks, and see if the player dies.
//     Then the checkpoint is loaded again, so the test leaves no trace.
//  3. If the test survives, the input is applied for real (handleButton). If not, it waits one
//     more tick and is tested again.
//  4. After the max delay it is applied anyway (or dropped, if that setting is on).
// During a test, PlayLayer::destroyPlayer only records the death and does not kill the player.

namespace {

struct Pending {
    int button;
    bool push;      // true = press, false = release
    bool isPlayer2;
    double timestamp;
    int waited;
};

std::deque<Pending> g_queue;
bool g_probing = false;   // a test is running
bool g_injecting = false; // we are applying an input ourselves
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
    // handleButton takes isPlayer1, queueButton gave us isPlayer2
    pl->handleButton(ev.push, ev.button, !ev.isPlayer2);
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
        pl->processCommands(dt, false, false);
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
        log::info("{} button {} (p2={}) applied after {} ticks{}",
            ev.push ? "press" : "release", ev.button, ev.isPlayer2, ev.waited,
            timedOut ? " (timed out)" : "");
    }
}

} // namespace

class $modify(TickBufferBase, GJBaseGameLayer) {
    void queueButton(int button, bool push, bool isPlayer2, double timestamp) {
        if (!g_injecting && !g_probing && active(this)) {
            // Keep order: if inputs are waiting, a release must wait behind them.
            if (push || on("buffer-releases") || !g_queue.empty()) {
                g_queue.push_back({button, push, isPlayer2, timestamp, 0});
                return;
            }
        }
        GJBaseGameLayer::queueButton(button, push, isPlayer2, timestamp);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (!g_probing && !g_queue.empty()) {
            if (active(this)) pump(typeinfo_cast<PlayLayer*>(this), dt);
            else g_queue.clear();
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
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
