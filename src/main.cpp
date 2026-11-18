#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <algorithm>
#include <cmath>
#include <deque>
#include <vector>

using namespace geode::prelude;

struct HistEntry {
    Ref<CheckpointObject> cp;
    double clock; // time in physics ticks (240 per second)
};

static std::deque<HistEntry> g_hist;   // rolling per-frame state history
static double g_clock = 0;             // tick clock, advanced every real frame
static bool g_sim = false;             // true while the window test is running
static bool g_simDied = false;
static bool g_simDone = false;
static Ref<CheckpointObject> g_testCp; // checkpoint used for the test
static Ref<CheckpointObject> g_lastAuto;
static double g_testAge = 0;           // ticks between checkpoint and your click
static double g_downClock = -1;
static int g_holdTicks = 10;           // how long you held your last click

// Places an auto checkpoint from a few ticks BEFORE each click.
class $modify(FWBaseLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);

        if (g_sim || button != 1 || !isPlayer1) return;
        auto pl = PlayLayer::get();
        if (!pl || !pl->m_isPracticeMode) return;
        if (!Mod::get()->getSettingValue<bool>("enabled")) return;

        if (!down) {
            if (g_downClock >= 0) {
                g_holdTicks = std::clamp(static_cast<int>(std::lround(g_clock - g_downClock)), 1, 600);
                g_downClock = -1;
            }
            return;
        }
        g_downClock = g_clock;

        double lookback = static_cast<double>(Mod::get()->getSettingValue<int64_t>("lookback"));
        double target = g_clock - lookback;
        HistEntry* pick = nullptr;
        for (auto& e : g_hist) {
            if (e.clock <= target) pick = &e;
        }
        if (!pick && !g_hist.empty()) pick = &g_hist.front();
        if (!pick) return;
        if (pick->cp.data() == g_testCp.data()) return;

        // Replace our previous auto checkpoint; manual ones are never touched.
        auto arr = pl->m_checkpointArray;
        if (g_lastAuto && arr && arr->count() > 0 &&
            arr->lastObject() == static_cast<CCObject*>(g_lastAuto.data())) {
            arr->removeLastObject();
        }
        pl->storeCheckpoint(pick->cp);
        g_lastAuto = pick->cp;
        g_testCp = pick->cp;
        g_testAge = g_clock - pick->clock;
    }
};

class $modify(FWPlayLayer, PlayLayer) {
    struct Fields {
        CCMenu* menu = nullptr;
        CCLabelBMFont* label = nullptr;
    };

    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();

        g_hist.clear();
        g_testCp = nullptr;
        g_lastAuto = nullptr;
        g_clock = 0;
        g_downClock = -1;
        g_sim = false;

        auto win = CCDirector::get()->getWinSize();

        auto spr = ButtonSprite::create("Window");
        spr->setScale(0.6f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(FWPlayLayer::onTest));
        auto menu = CCMenu::create();
        menu->addChild(btn);
        menu->setPosition({win.width - 55.f, win.height - 75.f});
        m_uiLayer->addChild(menu, 100);

        auto label = CCLabelBMFont::create("", "bigFont.fnt");
        label->setScale(0.45f);
        label->setPosition({win.width / 2.f, win.height - 18.f});
        m_uiLayer->addChild(label, 100);

        m_fields->menu = menu;
        m_fields->label = label;
    }

    void update(float dt) {
        bool enabled = Mod::get()->getSettingValue<bool>("enabled");
        if (m_fields->menu) m_fields->menu->setVisible(m_isPracticeMode && enabled);

        // Remember the player state at the start of every frame (practice mode only).
        if (!g_sim && enabled && m_isPracticeMode && m_player1 && !m_player1->m_isDead) {
            if (auto cp = this->createCheckpoint()) {
                g_hist.push_back(HistEntry{Ref<CheckpointObject>(cp), g_clock});
                double keep = Mod::get()->getSettingValue<int64_t>("lookback") * 3.0 + 30.0;
                while (g_hist.size() > 1 && g_clock - g_hist.front().clock > keep) {
                    g_hist.pop_front();
                }
            }
        }

        PlayLayer::update(dt);
        if (!g_sim) g_clock += dt * 240.0;
    }

    // While testing, dying/finishing is only recorded, not played out.
    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        if (g_sim) {
            g_simDied = true;
            return;
        }
        PlayLayer::destroyPlayer(player, obj);
    }

    void levelComplete() {
        if (g_sim) {
            g_simDone = true;
            return;
        }
        PlayLayer::levelComplete();
    }

    void onQuit() {
        g_hist.clear();
        g_testCp = nullptr;
        g_lastAuto = nullptr;
        g_sim = false;
        PlayLayer::onQuit();
    }

    void onTest(CCObject*) {
        if (!m_isPracticeMode) return;
        if (!g_testCp) {
            m_fields->label->setString("Click once in practice mode first");
            return;
        }
        runWindowTest();
    }

    void runWindowTest() {
        int A = static_cast<int>(std::lround(g_testAge));
        int T = std::max(2, 2 * A);
        int hold = g_holdTicks;
        int after = static_cast<int>(Mod::get()->getSettingValue<int64_t>("after-release"));
        std::vector<bool> pass(T + 1, false);

        g_sim = true;
        for (int t = 0; t <= T; t++) {
            this->loadFromCheckpoint(g_testCp.data());
            g_simDied = false;
            g_simDone = false;
            this->handleButton(false, 1, true);

            int horizon = t + hold + after;
            for (int k = 0; k < horizon && !g_simDied && !g_simDone; k++) {
                if (k == t) this->handleButton(true, 1, true);
                if (k == t + hold) this->handleButton(false, 1, true);
                PlayLayer::update((1.f / 240.f) * 1.0001f);
            }
            pass[t] = !g_simDied;
        }
        this->loadFromCheckpoint(g_testCp.data());
        this->handleButton(false, 1, true);
        g_sim = false;

        std::string text;
        if (A <= T && pass[A]) {
            int lo = A, hi = A;
            while (lo > 0 && pass[lo - 1]) lo--;
            while (hi < T && pass[hi + 1]) hi++;
            int window = hi - lo + 1;
            text = fmt::format(
                "Window: {}{} ticks ({:.1f} ms)  early {} / late {}",
                window, (lo == 0 || hi == T) ? "+" : "", window * 1000.0 / 240.0, A - lo, hi - A
            );
        } else {
            text = "Window: 0 (test says your click fails, may be inaccurate)";
        }
        m_fields->label->setString(text.c_str());
    }
};
