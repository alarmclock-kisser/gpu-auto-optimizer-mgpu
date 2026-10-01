#include "doctest/doctest.h"
#include "core/manual.hpp"

using namespace gao;

namespace {
struct Card {
    int power = 100, core = 0, mem = 0;
    bool fail_core = false;
    bool reset_ok = true;
    int writes = 0;
    GpuControl gpu(bool with_power = true) {
        GpuControl g;
        g.set_core_offset = [this](int v) {
            ++writes;
            if (fail_core) return false;
            core = v;
            return true;
        };
        g.set_mem_offset = [this](int v) {
            ++writes;
            mem = v;
            return true;
        };
        g.reset_to_stock = [this] {
            if (!reset_ok) return false;
            power = 100;
            core = 0;
            mem = 0;
            return true;
        };
        if (with_power)
            g.set_power_limit = [this](int p) {
                ++writes;
                power = p;
                return true;
            };
        return g;
    }
};

Journal make_journal(std::vector<std::string>* out) {
    return Journal({}, [out](const std::string& line) {
        out->push_back(line);
        return true;
    });
}
}

TEST_CASE("zones mark the top of each range as extreme") {
    CHECK(core_zone(-200) == ManualZone::Safe);   // a downclock is the safe direction
    CHECK(core_zone(0) == ManualZone::Safe);
    CHECK(core_zone(210) == ManualZone::Safe);
    CHECK(core_zone(225) == ManualZone::Warm);
    CHECK(core_zone(300) == ManualZone::Extreme);
    CHECK(mem_zone(-500) == ManualZone::Safe);
    CHECK(mem_zone(1050) == ManualZone::Safe);
    CHECK(mem_zone(1100) == ManualZone::Warm);
    CHECK(mem_zone(1500) == ManualZone::Extreme);
    CHECK(power_zone(100, 50, 130) == ManualZone::Safe);
    CHECK(power_zone(105, 50, 130) == ManualZone::Warm);
    CHECK(power_zone(120, 50, 130) == ManualZone::Extreme);
    CHECK(power_zone(200, 50, 130) == ManualZone::Extreme);
    ManualTune hot;
    hot.core_mhz = 300;
    CHECK(manual_is_extreme(hot, 50, 130));
    ManualTune mild;
    mild.core_mhz = 150;
    mild.mem_mhz = 500;
    CHECK_FALSE(manual_is_extreme(mild, 50, 130));
}

TEST_CASE("snap rounds to the hardware steps") {
    ManualTune t;
    t.core_mhz = 152;
    t.mem_mhz = 1074;
    t.power_pct = 103;
    const ManualTune s = manual_snap(t);
    CHECK(s.core_mhz == 150);
    CHECK(s.mem_mhz == 1050);
    CHECK(s.power_pct == 105);
    ManualTune down;
    down.core_mhz = -152;
    down.mem_mhz = -260;
    const ManualTune snapped = manual_snap(down);
    CHECK(snapped.core_mhz == -150);
    CHECK(snapped.mem_mhz == -250);
}

TEST_CASE("a downclock applies; past the manual floor it is refused unwritten") {
    Card card;
    ManualTune t;
    t.core_mhz = -150;
    t.mem_mhz = -250;
    std::string why;
    CHECK(apply_manual(card.gpu(), t, nullptr, 50, 150, &why));
    CHECK(card.core == -150);
    CHECK(card.mem == -250);
    ManualTune bad;
    bad.core_mhz = -215;
    CHECK_FALSE(apply_manual(card.gpu(), bad, nullptr, 50, 150, &why));
    CHECK(why.find("nothing applied") != std::string::npos);
    CHECK(card.core == -150);   // the refused apply wrote nothing
}

TEST_CASE("out-of-range manual values are refused before anything is written") {
    Card card;
    ManualTune bad;
    bad.core_mhz = 400;
    std::string why;
    CHECK_FALSE(apply_manual(card.gpu(), bad, nullptr, 50, 150, &why));
    CHECK(why.find("nothing applied") != std::string::npos);
    CHECK(card.writes == 0);
    CHECK(card.core == 0);
}

TEST_CASE("disabled knobs apply as stock") {
    Card card;
    ManualTune t;
    t.use_core = false;
    t.use_mem = false;
    t.use_power = false;
    t.core_mhz = 300;
    t.mem_mhz = 1500;
    t.power_pct = 150;
    std::string why;
    CHECK(apply_manual(card.gpu(), t, nullptr, 50, 150, &why));
    CHECK(card.core == 0);
    CHECK(card.mem == 0);
    CHECK(card.power == 100);
}

TEST_CASE("a failing setter resets to stock") {
    Card card;
    card.fail_core = true;
    ManualTune t;
    t.core_mhz = 150;
    std::string why;
    CHECK_FALSE(apply_manual(card.gpu(), t, nullptr, 50, 150, &why));
    CHECK(card.core == 0);
    CHECK(card.power == 100);
    CHECK(why.find("card at stock") != std::string::npos);
}

TEST_CASE("manual apply journals begin before hardware and completes after") {
    Card card;
    std::vector<std::string> lines;
    Journal journal = make_journal(&lines);
    ManualTune t;
    t.core_mhz = 150;
    t.mem_mhz = 500;
    std::string why;
    CHECK(apply_manual(card.gpu(), t, &journal, 50, 150, &why));
    CHECK(lines.size() == 2);
    CHECK(lines[0].find("begin") != std::string::npos);
    CHECK(lines[1].find("MANUAL APPLY") != std::string::npos);
    CHECK(card.core == 150);
    CHECK(card.mem == 500);
}

TEST_CASE("a downclock is applied but not journaled as a ceiling") {
    Card card;
    std::vector<std::string> lines;
    Journal journal = make_journal(&lines);
    ManualTune t;
    t.core_mhz = -100;
    t.mem_mhz = 500;
    std::string why;
    CHECK(apply_manual(card.gpu(), t, &journal, 50, 150, &why));
    CHECK(card.core == -100);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].find("\"core\"") == std::string::npos);   // ceilings stay upper bounds of raised clocks
    CHECK(lines[0].find("\"mem\"") != std::string::npos);
}
