#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

using namespace geode::prelude;

struct Ev { int tick; bool down; };
struct Click { int pi; int ri; int press; int release; }; // indices into macro, ri = -1 if never released
struct WindowResult { int click; int press; int window; int early; int late; bool plus; bool desync; };

static std::vector<Ev> g_rec;    // macro being recorded
static std::vector<Ev> g_macro;  // saved macro
static int g_macroLevel = 0;
static bool g_recording = false;
static bool g_sim = false;
static bool g_simDied = false;
static bool g_simDone = false;
static int g_tick = 0;           // physics tick counter, reset on every attempt

static std::vector<int> g_clickBucket;  // HUD bucket per click, -1 = not measured
static std::vector<float> g_clickX;    // x position of each click, -1 = unknown

static int bucketOf(int w) {
    if (w >= 11) return 0;
    if (w >= 9) return 1;
    if (w >= 7) return 2;
    if (w == 6) return 3;
    if (w == 5) return 4;
    if (w == 4) return 5;
    if (w == 3) return 6;
    if (w == 2) return 7;
    return 8;
}
static char const* bucketName(int b) {
    static char const* names[9] = {"11+", "9-10", "7-8", "6", "5", "4", "3", "2", "1"};
    return names[b];
}
static ccColor3B bucketColor(int b) {
    static ccColor3B cols[9] = {
        {181, 125, 255}, {140, 125, 255}, {90, 165, 255}, {90, 215, 255}, {110, 225, 110},
        {255, 225, 100}, {255, 165, 80},  {255, 90, 80},  {255, 95, 140}
    };
    return cols[b];
}

struct Job {
    bool active = false;
    int phase = 0; // 0 = replay macro and take snapshots, 1 = test shifts
    int K = 6;
    int after = 20;
    std::vector<Click> clicks;
    std::vector<int> snapTick;
    std::vector<Ref<CheckpointObject>> snaps;
    std::vector<std::vector<char>> pass; // 0 fail, 1 pass, 2 untested
    // phase 0
    int tick = 0;
    size_t evPtr = 0;
    size_t nextSnap = 0;
    int diedAt = -1;
    // phase 1
    size_t ci = 0;
    int shift = 0;
    bool loaded = false;
    int endTick = 0;
    std::vector<Ev> evs;
    int total = 0;
    int done = 0;
    std::vector<CCPoint> clickPos;
    std::vector<char> havePos;
};
static Job g_job;

static std::vector<Click> buildClicks(std::vector<Ev> const& m) {
    std::vector<Click> out;
    bool open = false;
    for (int i = 0; i < static_cast<int>(m.size()); i++) {
        if (m[i].down) {
            if (!open) {
                out.push_back(Click{i, -1, m[i].tick, -1});
                open = true;
            }
        } else if (open) {
            out.back().ri = i;
            out.back().release = m[i].tick;
            open = false;
        }
    }
    return out;
}

class $modify(FWBase, GJBaseGameLayer) {
    // One call per physics tick: this is the macro clock.
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        if (!isHalfTick) g_tick++;
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
        if (g_sim || !g_recording || button != 1 || !isPlayer1) return;
        g_rec.push_back(Ev{g_tick, down});
    }
};

class $modify(FWPlayLayer, PlayLayer) {
    struct Fields {
        CCMenu* menu = nullptr;
        CCLabelBMFont* label = nullptr;
        ButtonSprite* recSpr = nullptr;
        CCNode* hud = nullptr;
        std::vector<CCLabelBMFont*> rows;
        std::vector<CCNode*> rings;
        int hl = -2;
    };

    void say(std::string const& text) {
        if (m_fields->label) m_fields->label->setString(text.c_str());
    }

    bool hasCheckpoints() {
        return m_isPracticeMode && m_checkpointArray && m_checkpointArray->count() > 0;
    }

    int levelKey() {
        return m_level ? static_cast<int>(m_level->m_levelID.value()) : 0;
    }

    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();

        g_recording = false;
        g_sim = false;
        g_job.active = false;
        g_tick = 0;

        auto win = CCDirector::get()->getWinSize();
        auto menu = CCMenu::create();

        auto recSpr = ButtonSprite::create("Rec");
        recSpr->setScale(0.6f);
        auto recBtn = CCMenuItemSpriteExtra::create(recSpr, this, menu_selector(FWPlayLayer::onRec));
        recBtn->setPosition({0.f, 0.f});
        menu->addChild(recBtn);

        auto testSpr = ButtonSprite::create("Test");
        testSpr->setScale(0.6f);
        auto testBtn = CCMenuItemSpriteExtra::create(testSpr, this, menu_selector(FWPlayLayer::onTest));
        testBtn->setPosition({0.f, -30.f});
        menu->addChild(testBtn);

        menu->setPosition({win.width - 55.f, win.height - 75.f});
        m_uiLayer->addChild(menu, 100);

        auto label = CCLabelBMFont::create("", "bigFont.fnt");
        label->setScale(0.4f);
        label->setPosition({win.width / 2.f, win.height - 18.f});
        m_uiLayer->addChild(label, 100);

        m_fields->menu = menu;
        m_fields->label = label;
        m_fields->recSpr = recSpr;
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        g_tick = 0;
        // While recording, every death/restart starts the macro over.
        if (g_recording && !g_sim) g_rec.clear();
    }

    void update(float dt) {
        if (m_fields->menu) {
            m_fields->menu->setVisible(Mod::get()->getSettingValue<bool>("enabled"));
        }
        if (g_job.active) {
            runJob();
            return;
        }
        updateHighlight();
        PlayLayer::update(dt);
    }

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
        if (g_recording) stopRecording();
        PlayLayer::levelComplete();
    }

    void onQuit() {
        g_recording = false;
        g_sim = false;
        g_job.active = false;
        PlayLayer::onQuit();
    }

    // ---------- recording ----------

    void stopRecording() {
        g_recording = false;
        g_macro = g_rec;
        g_macroLevel = levelKey();
        auto clicks = buildClicks(g_macro);
        if (m_fields->recSpr) m_fields->recSpr->setString("Rec");
        say(fmt::format("Macro saved: {} clicks. Tap Test.", clicks.size()));
    }

    void onRec(CCObject*) {
        if (g_job.active) return;
        if (g_recording) {
            stopRecording();
            return;
        }
        if (hasCheckpoints()) {
            say("Use normal mode or remove checkpoints");
            return;
        }
        g_rec.clear();
        g_recording = true;
        if (m_fields->recSpr) m_fields->recSpr->setString("Stop");
        say("Recording: play from the start. Finish or tap Stop.");
        this->resetLevel();
    }

    // ---------- measuring ----------

    void onTest(CCObject*) {
        if (g_job.active) {
            // tap again to cancel
            g_sim = false;
            g_job.active = false;
            this->resetLevel();
            say("Cancelled");
            return;
        }
        if (g_recording) stopRecording();
        if (g_macro.empty() || g_macroLevel != levelKey()) {
            say("Record a macro for this level first (Rec)");
            return;
        }
        if (hasCheckpoints()) {
            say("Use normal mode or remove checkpoints");
            return;
        }

        auto mod = Mod::get();
        Job J;
        J.K = static_cast<int>(mod->getSettingValue<int64_t>("range"));
        J.after = static_cast<int>(mod->getSettingValue<int64_t>("after"));
        J.clicks = buildClicks(g_macro);
        if (J.clicks.empty()) {
            say("Macro has no clicks");
            return;
        }
        for (auto& c : J.clicks) J.snapTick.push_back(std::max(0, c.press - J.K));
        J.snaps.assign(J.clicks.size(), Ref<CheckpointObject>());
        J.pass.assign(J.clicks.size(), std::vector<char>(2 * J.K + 1, 0));
        J.clickPos.assign(J.clicks.size(), CCPoint(0, 0));
        J.havePos.assign(J.clicks.size(), 0);
        J.active = true;
        g_job = std::move(J);
        this->clearDisplay();

        g_sim = true;
        g_simDied = false;
        g_simDone = false;
        this->resetLevel();
        say("Replaying macro...");
    }

    void stepTick() {
        int before = g_tick;
        for (int g = 0; g < 6 && g_tick == before && !g_simDied && !g_simDone; g++) {
            PlayLayer::update((1.f / 240.f) * 1.0001f);
        }
    }

    void runJob() {
        using clk = std::chrono::steady_clock;
        auto t0 = clk::now();
        auto budget = Mod::get()->getSettingValue<int64_t>("budget");
        while (std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - t0).count() < budget) {
            if (g_job.phase == 0) {
                analysisStep();
            } else if (testStep()) {
                finishJob();
                return;
            }
        }
        if (g_job.phase == 0) {
            int last = g_job.snapTick.empty() ? 0 : g_job.snapTick.back();
            say(fmt::format("Replaying macro: tick {}/{}", g_job.tick, last));
        } else {
            say(fmt::format("Measuring {}/{}", g_job.done, g_job.total));
        }
    }

    void beginTests() {
        auto& J = g_job;
        J.phase = 1;
        J.ci = 0;
        J.shift = -J.K;
        J.loaded = false;
        int have = 0;
        for (auto& s : J.snaps) {
            if (s) have++;
        }
        J.total = have * (2 * J.K + 1);
        J.done = 0;
        log::info("replay done at tick {}, snapshots {}, diedAt {}", J.tick, have, J.diedAt);
    }

    // Replays the unmodified macro once and snapshots the state just before each click.
    void analysisStep() {
        auto& J = g_job;
        if (J.nextSnap < J.clicks.size() && J.snapTick[J.nextSnap] == J.tick) {
            auto cp = this->createCheckpoint();
            while (J.nextSnap < J.clicks.size() && J.snapTick[J.nextSnap] == J.tick) {
                J.snaps[J.nextSnap] = Ref<CheckpointObject>(cp);
                J.nextSnap++;
            }
        }
        if (J.nextSnap >= J.clicks.size()) {
            beginTests();
            return;
        }
        while (J.evPtr < g_macro.size() && g_macro[J.evPtr].tick <= J.tick) {
            this->handleButton(g_macro[J.evPtr].down, 1, true);
            J.evPtr++;
        }
        this->stepTick();
        J.tick++;
        if (g_simDied) {
            J.diedAt = J.tick;
            beginTests();
        } else if (g_simDone) {
            beginTests();
        }
    }

    // Returns true when every test is finished.
    bool testStep() {
        auto& J = g_job;
        if (J.ci >= J.clicks.size()) return true;
        if (!J.snaps[J.ci]) {
            J.ci++;
            J.shift = -J.K;
            J.loaded = false;
            return false;
        }

        auto& c = J.clicks[J.ci];
        if (!J.loaded) {
            if (c.press + J.shift < 0) {
                J.pass[J.ci][J.shift + J.K] = 2; // can't click before the level starts
                J.shift++;
                if (J.shift > J.K) {
                    J.ci++;
                    J.shift = -J.K;
                }
                return false;
            }
            J.evs = g_macro;
            J.evs[c.pi].tick += J.shift;
            if (c.ri >= 0) J.evs[c.ri].tick += J.shift;
            std::stable_sort(J.evs.begin(), J.evs.end(), [](Ev const& a, Ev const& b) {
                if (a.tick != b.tick) return a.tick < b.tick;
                return !a.down && b.down; // release before press on the same tick
            });

            this->loadFromCheckpoint(J.snaps[J.ci].data());
            J.tick = J.snapTick[J.ci];
            J.evPtr = 0;
            bool held = false;
            while (J.evPtr < J.evs.size() && J.evs[J.evPtr].tick < J.tick) {
                held = J.evs[J.evPtr].down;
                J.evPtr++;
            }
            this->handleButton(held, 1, true);
            g_simDied = false;
            g_simDone = false;
            int next = (J.ci + 1 < J.clicks.size()) ? J.clicks[J.ci + 1].press
                                                    : (c.release >= 0 ? c.release : c.press);
            J.endTick = next + J.after + J.K;
            J.loaded = true;
        }

        while (J.evPtr < J.evs.size() && J.evs[J.evPtr].tick <= J.tick) {
            if (J.shift == 0 && J.evs[J.evPtr].down && J.evs[J.evPtr].tick == c.press && !J.havePos[J.ci]) {
                J.clickPos[J.ci] = m_player1->getPosition();
                J.havePos[J.ci] = 1;
            }
            this->handleButton(J.evs[J.evPtr].down, 1, true);
            J.evPtr++;
        }
        this->stepTick();
        J.tick++;

        if (g_simDied || g_simDone || J.tick >= J.endTick) {
            J.pass[J.ci][J.shift + J.K] = g_simDied ? 0 : 1;
            J.done++;
            J.loaded = false;
            J.shift++;
            if (J.shift > J.K) {
                J.ci++;
                J.shift = -J.K;
            }
        }
        return false;
    }

    void clearDisplay() {
        if (m_fields->hud) {
            m_fields->hud->removeFromParent();
            m_fields->hud = nullptr;
        }
        for (auto r : m_fields->rings) r->removeFromParent();
        m_fields->rings.clear();
        m_fields->rows.clear();
        m_fields->hl = -2;
        g_clickBucket.clear();
        g_clickX.clear();
    }

    void applyHighlight(int bucket) {
        for (int b = 0; b < static_cast<int>(m_fields->rows.size()); b++) {
            auto row = m_fields->rows[b];
            row->setColor(b == bucket ? ccc3(255, 255, 255) : bucketColor(b));
            row->setScale(b == bucket ? 0.55f : 0.45f);
        }
    }

    // Highlights the bucket of the last measured click the player has passed.
    void updateHighlight() {
        if (m_fields->rows.empty() || g_clickBucket.empty() || !m_player1) return;
        float x = m_player1->getPositionX();
        int bucket = -1;
        for (size_t i = 0; i < g_clickBucket.size(); i++) {
            if (g_clickBucket[i] >= 0 && g_clickX[i] >= 0.f && g_clickX[i] <= x) bucket = g_clickBucket[i];
        }
        if (bucket != m_fields->hl) {
            m_fields->hl = bucket;
            applyHighlight(bucket);
        }
    }

    void buildDisplay(std::vector<int> const& win, int tightest) {
        clearDisplay();
        auto mod = Mod::get();
        auto& J = g_job;
        int n = static_cast<int>(win.size());
        int counts[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};

        g_clickBucket.assign(n, -1);
        g_clickX.assign(n, -1.f);
        for (int i = 0; i < n; i++) {
            if (win[i] > 0) {
                int b = bucketOf(win[i]);
                counts[b]++;
                g_clickBucket[i] = b;
            }
            if (J.havePos[i]) g_clickX[i] = J.clickPos[i].x;
        }

        if (mod->getSettingValue<bool>("show-hud")) {
            auto size = CCDirector::get()->getWinSize();
            auto hud = CCNode::create();
            float y = size.height - 24.f;
            for (int b = 0; b < 9; b++) {
                auto row = CCLabelBMFont::create(
                    fmt::format("{}: {}", bucketName(b), counts[b]).c_str(), "bigFont.fnt"
                );
                row->setAnchorPoint({0.f, 0.5f});
                row->setScale(0.45f);
                row->setColor(bucketColor(b));
                row->setPosition({14.f, y});
                hud->addChild(row);
                m_fields->rows.push_back(row);
                y -= 20.f;
            }
            y -= 8.f;
            auto ms = CCLabelBMFont::create(
                fmt::format("{:.2f}MS", tightest * 1000.0 / 240.0).c_str(), "bigFont.fnt"
            );
            ms->setAnchorPoint({0.f, 0.5f});
            ms->setScale(0.45f);
            ms->setPosition({14.f, y});
            hud->addChild(ms);
            y -= 20.f;
            int measured = 0;
            for (auto w : win) {
                if (w > 0) measured++;
            }
            auto cnt = CCLabelBMFont::create(fmt::format("{} CLICKS", measured).c_str(), "bigFont.fnt");
            cnt->setAnchorPoint({0.f, 0.5f});
            cnt->setScale(0.45f);
            cnt->setPosition({14.f, y});
            hud->addChild(cnt);

            m_uiLayer->addChild(hud, 99);
            m_fields->hud = hud;
        }

        if (mod->getSettingValue<bool>("show-rings") && m_objectLayer) {
            for (int i = 0; i < n; i++) {
                if (!J.havePos[i] || win[i] < 0) continue;
                ccColor3B col = win[i] > 0 ? bucketColor(bucketOf(win[i])) : ccc3(150, 150, 150);

                auto node = CCNode::create();
                node->setPosition(J.clickPos[i]);

                const int segs = 24;
                const float radius = 17.f;
                CCPoint verts[segs];
                for (int k = 0; k < segs; k++) {
                    float a = static_cast<float>(k) * 6.2831853f / segs;
                    verts[k] = ccp(std::cos(a) * radius, std::sin(a) * radius);
                }
                auto ring = CCDrawNode::create();
                ring->drawPolygon(
                    verts, segs, ccc4f(0.f, 0.f, 0.f, 0.f), 2.5f,
                    ccc4f(col.r / 255.f, col.g / 255.f, col.b / 255.f, 1.f)
                );
                node->addChild(ring);

                auto lbl = CCLabelBMFont::create(
                    win[i] > 0 ? std::to_string(win[i]).c_str() : "?", "bigFont.fnt"
                );
                lbl->setScale(0.45f);
                lbl->setColor(col);
                lbl->setAnchorPoint({1.f, 0.5f});
                lbl->setPosition({-radius - 5.f, 0.f});
                node->addChild(lbl);

                m_objectLayer->addChild(node, 100);
                m_fields->rings.push_back(node);
            }
        }
    }

    void finishJob() {
        auto& J = g_job;
        g_sim = false;
        J.active = false;
        this->resetLevel();

        int n = static_cast<int>(J.clicks.size());
        std::vector<int> win(n, -1); // -1 not measured, 0 desync, otherwise window in ticks
        for (int i = 0; i < n; i++) {
            if (!J.snaps[i]) continue;
            auto& p = J.pass[i];
            if (p[J.K] != 1) {
                win[i] = 0;
                continue;
            }
            int lo = J.K, hi = J.K;
            while (lo > 0 && p[lo - 1] == 1) lo--;
            while (hi < 2 * J.K && p[hi + 1] == 1) hi++;
            win[i] = hi - lo + 1;
            log::info("click {} tick {} window {} early {} late {}",
                      i + 1, J.clicks[i].press, win[i], J.K - lo, hi - J.K);
        }

        int tightest = -1;
        for (auto w : win) {
            if (w > 0 && (tightest < 0 || w < tightest)) tightest = w;
        }
        if (tightest < 0) {
            say("Nothing measured (macro died too early or desynced)");
            return;
        }

        buildDisplay(win, tightest);
        if (J.diedAt >= 0) {
            say(fmt::format("Done. Replay died at tick {}, later clicks skipped", J.diedAt));
        } else {
            say("Done");
        }
    }
};
