#include "doctest/doctest.h"
#include "core/config.hpp"
#include "core/boot.hpp"

using namespace gao;

namespace {
Profile sample() {
    Profile p;
    p.preset = Preset::Quiet;
    p.power_pct = 80;
    p.core_mhz = 90;
    p.mem_mhz = 600;
    p.driver = "610.74";
    p.gpu = "GPU-8a1b";
    p.saved_at = "2026-09-26 22:41";
    return p;
}
}

TEST_CASE("per-GPU profiles and settings survive a round-trip") {
    Config c;
    c.selected_gpu = "GPU-8a1b";
    DeviceSettings& first = ensure_device(c, "GPU-8a1b");
    first.profile = sample();
    first.boot_strikes = 2;
    first.profile->fan_curve = FanCurve{50, {{50, 30}, {80, 100}}};
    first.fan_curve = FanCurve{std::nullopt, {{40, 40}, {70, 90}}};
    first.fan_control = true;
    first.fan_min_pct = 50;
    const auto tested_curve = first.profile->fan_curve;
    const auto edited_curve = first.fan_curve;
    DeviceSettings& second = ensure_device(c, "GPU-2");
    second.profile = sample();
    second.profile->gpu = "GPU-2";
    second.boot_strikes = 1;

    const Config back = from_json(to_json(c));
    REQUIRE(back.devices.size() == 2);
    CHECK(back.selected_gpu == "GPU-8a1b");
    const DeviceSettings* restored = find_device(back, "GPU-8a1b");
    REQUIRE(restored);
    REQUIRE(restored->profile);
    CHECK(restored->profile->preset == Preset::Quiet);
    CHECK(restored->profile->power_pct == 80);
    CHECK(restored->profile->core_mhz == 90);
    CHECK(restored->profile->mem_mhz == 600);
    CHECK(restored->profile->driver == "610.74");
    CHECK(restored->profile->gpu == "GPU-8a1b");
    CHECK(restored->profile->saved_at == "2026-09-26 22:41");
    CHECK(restored->boot_strikes == 2);
    CHECK(restored->profile->fan_curve == tested_curve);
    CHECK(restored->fan_curve == edited_curve);
    CHECK(restored->fan_control);
    CHECK(restored->fan_min_pct == 50);
    CHECK(find_profile(back, "GPU-2")->gpu == "GPU-2");
    CHECK(find_device(back, "GPU-2")->boot_strikes == 1);
    CHECK(to_json(c).find("\"quiet\"") != std::string::npos);
}

TEST_CASE("a session GPU selection uses that GPU's own saved profile") {
    Config config;
    config.selected_gpu = "GPU-4060";
    DeviceSettings& first = ensure_device(config, "GPU-4060");
    first.profile = sample();
    first.profile->gpu = "GPU-4060";
    DeviceSettings& second = ensure_device(config, "GPU-3060");
    second.profile = sample();
    second.profile->gpu = "GPU-3060";
    second.profile->core_mhz = 150;

    const std::string session_selection = "GPU-3060";
    const std::string& active_gpu = effective_gpu_selection(config, session_selection);
    const Profile* active_profile = find_profile(config, active_gpu);

    CHECK(active_gpu == "GPU-3060");
    REQUIRE(active_profile);
    CHECK(active_profile->core_mhz == 150);
    CHECK(config.selected_gpu == "GPU-4060");
    CHECK(effective_gpu_selection(config, std::string{}) == "GPU-4060");
}

TEST_CASE("a config without profiles round-trips selected GPU fan settings") {
    Config c;
    c.selected_gpu = "GPU-1";
    DeviceSettings& device = ensure_device(c, c.selected_gpu);
    device.boot_strikes = 1;
    device.fan_control = true;
    const Config back = from_json(to_json(c));
    CHECK_FALSE(find_profile(back, "GPU-1"));
    REQUIRE(find_device(back, "GPU-1"));
    CHECK(find_device(back, "GPU-1")->boot_strikes == 1);
    CHECK(find_device(back, "GPU-1")->fan_control);
}

TEST_CASE("unreadable JSON yields defaults instead of throwing") {
    const Config c = from_json("{ this is not json");
    CHECK(c.devices.empty());
    CHECK(from_json("").devices.empty());
    CHECK(from_json("[1,2]").devices.empty());
}

TEST_CASE("a profile with a wrong type, a missing field or an unknown preset is ignored") {
    CHECK_FALSE(find_profile(from_json(R"({"profile":{"preset":"best","power_pct":"105","core_mhz":0,"mem_mhz":0,"driver":"x","gpu":"g","saved_at":"y"}})"), "g"));
    CHECK_FALSE(find_profile(from_json(R"({"profile":{"preset":"best","power_pct":105,"core_mhz":0,"driver":"x","gpu":"g","saved_at":"y"}})"), "g"));
    CHECK_FALSE(find_profile(from_json(R"({"profile":{"preset":"turbo","power_pct":105,"core_mhz":0,"mem_mhz":0,"driver":"x","gpu":"g","saved_at":"y"}})"), "g"));
    CHECK(find_profile(from_json(R"({"profile":{"preset":"best","power_pct":105,"core_mhz":0,"mem_mhz":0,"driver":"x","gpu":"g","saved_at":"y"}})"), "g"));
}

TEST_CASE("legacy single-profile config migrates to a UUID-keyed device") {
    const Config c = from_json(R"({"boot_strikes":2,"fan_curve":{"stop_below_c":null,"points":[[40,40],[70,90]]},
        "fan_control":true,"fan_min_pct":50,"fan_min_gpu":"GPU-8a1b",
        "profile":{"preset":"quiet","power_pct":80,"core_mhz":90,"mem_mhz":600,
        "driver":"610.74","gpu":"GPU-8a1b","saved_at":"2026-09-26 22:41"}})");
    CHECK(c.selected_gpu == "GPU-8a1b");
    REQUIRE(find_device(c, "GPU-8a1b"));
    REQUIRE(find_profile(c, "GPU-8a1b"));
    CHECK(find_device(c, "GPU-8a1b")->boot_strikes == 2);
    CHECK(find_device(c, "GPU-8a1b")->fan_curve.has_value());
    CHECK(find_device(c, "GPU-8a1b")->fan_control);
    CHECK(find_device(c, "GPU-8a1b")->fan_min_pct == 50);
}

TEST_CASE("a profile saved before GPU identity existed is ignored") {
    CHECK_FALSE(find_profile(from_json(R"({"profile":{"preset":"best","power_pct":105,"core_mhz":135,"mem_mhz":1050,"driver":"610.74","saved_at":"y"}})"), ""));
}

TEST_CASE("a strike count that is not a sane integer fails closed") {
    for (const char* bad : {R"({"devices":[{"gpu":"GPU-1","boot_strikes":3.0}]})",
                            R"({"devices":[{"gpu":"GPU-1","boot_strikes":-1}]})",
                            R"({"devices":[{"gpu":"GPU-1","boot_strikes":4294967296}]})",
                            R"({"devices":[{"gpu":"GPU-1","boot_strikes":"x"}]})",
                            R"({"devices":[{"gpu":"GPU-1","boot_strikes":1001}]})",
                            R"({"devices":[{"gpu":"GPU-1","boot_strikes":null}]})"}) {
        CAPTURE(bad);
        CHECK(find_device(from_json(bad), "GPU-1")->boot_strikes == kMaxBootStrikes);
    }
    CHECK(find_device(from_json(R"({"devices":[{"gpu":"GPU-1","boot_strikes":0}]})"), "GPU-1")->boot_strikes == 0);
    CHECK(find_device(from_json(R"({"devices":[{"gpu":"GPU-1","boot_strikes":2}]})"), "GPU-1")->boot_strikes == 2);
}

TEST_CASE("a mismatched profile UUID cannot be attached to another device") {
    const Config c = from_json(R"({"devices":[{"gpu":"GPU-1","profile":{"preset":"best","power_pct":105,
        "core_mhz":0,"mem_mhz":0,"driver":"x","gpu":"GPU-2","saved_at":"y"}}]})");
    REQUIRE(find_device(c, "GPU-1"));
    CHECK_FALSE(find_profile(c, "GPU-1"));
}

TEST_CASE("an invalid fan curve reads as absent without losing the profile") {
    Config c;
    DeviceSettings& device = ensure_device(c, "GPU-8a1b");
    device.profile = sample();
    device.profile->fan_curve = FanCurve{std::nullopt, {{60, 60}, {40, 40}}};
    device.fan_curve = FanCurve{std::nullopt, {{40, 40}}};
    const Config back = from_json(to_json(c));
    REQUIRE(find_profile(back, "GPU-8a1b"));
    CHECK_FALSE(find_profile(back, "GPU-8a1b")->fan_curve.has_value());
    CHECK_FALSE(find_device(back, "GPU-8a1b")->fan_curve.has_value());
    const Config junk = from_json(R"({"fan_curve":{"points":"x"},"fan_control":"yes"})");
    CHECK(junk.devices.empty());
}

TEST_CASE("the active fan curve is the edited one, then tested, then profile default") {
    Config c;
    CHECK_FALSE(active_fan_curve(c, "GPU-1").has_value());
    DeviceSettings& device = ensure_device(c, "GPU-8a1b");
    device.profile = sample();
    CHECK(active_fan_curve(c, "GPU-8a1b") == default_curve(Preset::Quiet));
    device.profile->fan_curve = FanCurve{50, {{50, 40}, {80, 100}}};
    CHECK(active_fan_curve(c, "GPU-8a1b") == device.profile->fan_curve);
    device.fan_curve = FanCurve{std::nullopt, {{40, 40}, {70, 90}}};
    CHECK(active_fan_curve(c, "GPU-8a1b") == device.fan_curve);
}

TEST_CASE("learned fan minimum and fan settings belong to the GPU they were learned on") {
    Config c;
    ensure_device(c, "GPU-1").fan_min_pct = 50;
    ensure_device(c, "GPU-2").fan_min_pct = 45;
    const Config back = from_json(to_json(c));
    CHECK(fan_min_for(back, "GPU-1", 30) == 50);
    CHECK(fan_min_for(back, "GPU-2", 30) == 45);
    CHECK(fan_min_for(back, "GPU-3", 30) == 30);
    CHECK(fan_min_for(back, "GPU-1", 55) == 55);
    CHECK(from_json(R"({"devices":[{"gpu":"GPU-1","fan_min_pct":500}]})").devices.front().fan_min_pct == 0);
}
