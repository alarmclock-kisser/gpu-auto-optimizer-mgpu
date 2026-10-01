#include "app/gui/ui.hpp"
#include "core/boot.hpp"
#include "core/objectives.hpp"
#include "core/version.hpp"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace gao::gui {

namespace {

constexpr size_t kHistory = 60;
constexpr size_t kLogShort = 6;   // log lines on the dashboard before "View full log"

ImFont* g_regular = nullptr;
ImFont* g_bold = nullptr;
bool g_full_log = false;

const ImVec4 kBg{0.067f, 0.075f, 0.090f, 1};
const ImVec4 kNav{0.050f, 0.057f, 0.069f, 1};
const ImVec4 kCard{0.090f, 0.102f, 0.122f, 1};
const ImVec4 kInner{0.110f, 0.125f, 0.149f, 1};
const ImVec4 kInnerHover{0.140f, 0.158f, 0.188f, 1};
const ImVec4 kBorder{0.170f, 0.192f, 0.227f, 1};
const ImVec4 kAccent{0.180f, 0.435f, 0.835f, 1};
const ImVec4 kAccentHover{0.231f, 0.502f, 0.906f, 1};
const ImVec4 kAccentBright{0.290f, 0.580f, 1.000f, 1};
const ImVec4 kSelectedBg{0.098f, 0.145f, 0.220f, 1};
const ImVec4 kText{0.900f, 0.910f, 0.930f, 1};
const ImVec4 kDim{0.600f, 0.640f, 0.690f, 1};
const ImVec4 kGood{0.360f, 0.780f, 0.500f, 1};
const ImVec4 kWarn{0.910f, 0.710f, 0.230f, 1};
const ImVec4 kBad{0.930f, 0.400f, 0.400f, 1};

// Segoe Fluent Icons (Windows 11) / Segoe MDL2 Assets (Windows 10) code points.
enum Icon : unsigned {
    kIconHome = 0xE80F, kIconPulse = 0xE9D9, kIconInfo = 0xE946, kIconChip = 0xEEA1, kIconMemory = 0xE964,
    kIconTemp = 0xE9CA, kIconPower = 0xE945, kIconFan = 0xF16A, kIconRefresh = 0xE72C, kIconUndo = 0xE777,
    kIconPlay = 0xF5B0, kIconStop = 0xE71A, kIconClock = 0xE823, kIconStar = 0xE734, kIconQuiet = 0xE992,
    kIconLeaf = 0xEC0A, kIconGauge = 0xEC4A, kIconShield = 0xEA18, kIconList = 0xE8FD, kIconBack = 0xE72B,
};

// UTF-8 for a code point in the Basic Multilingual Plane's private use area.
std::string ic(unsigned cp) {
    const char s[4] = {static_cast<char>(0xE0 | (cp >> 12)), static_cast<char>(0x80 | ((cp >> 6) & 0x3F)),
                       static_cast<char>(0x80 | (cp & 0x3F)), 0};
    return s;
}
std::string with_icon(unsigned cp, const char* text) { return ic(cp) + "   " + text; }

float em() { return ImGui::GetFontSize(); }
float base() { return ImGui::GetStyle().FontSizeBase; }

struct PresetInfo {
    Preset preset;
    Icon icon;
    const char* title;
    const char* summary;
};
constexpr PresetInfo kPresets[] = {
    {Preset::BestOfMyGpu, kIconStar, "Best of my GPU",
     "Highest confirmed clocks with a safety margin and the most power the card allows."},
    {Preset::CoolAndEfficient, kIconLeaf, "Cool & efficient",
     "Power only: the lowest power limit that costs under 2 % speed, clocks left stock."},
    {Preset::Quiet, kIconQuiet, "Quiet", "Milder clocks and the lowest power limit that costs under 2 % speed."},
    {Preset::MaxPerformance, kIconGauge, "Max performance",
     "Highest confirmed clocks minus one step and the highest power limit."},
};

const char* title_of(Preset preset) {
    for (const PresetInfo& p : kPresets)
        if (p.preset == preset) return p.title;
    return preset_name(preset);
}

std::string offset(int mhz) { return (mhz >= 0 ? "+" : "") + std::to_string(mhz) + " MHz"; }

// ------------------------------------------------------------------ widgets

void text_bold(const char* text, float scale = 1.0f) {
    ImGui::PushFont(g_bold, base() * scale);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
}
void heading(const char* text) { text_bold(text, 1.2f); }
void dim(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}
void wrapped(const ImVec4& color, const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

// A rounded panel. height 0: as tall as its contents. Always pair with end_card().
void begin_card(const char* id, float height = 0, const ImVec4& bg = kCard, float width = 0) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em() * 0.9f, em() * 0.75f));
    ImGui::BeginChild(id, ImVec2(width, height),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding |
                          (height == 0 ? ImGuiChildFlags_AutoResizeY : ImGuiChildFlags_None),
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}
void end_card() { ImGui::EndChild(); }

// Puts the next item at the right edge of the current line, `width` wide.
void align_right(float width) {
    ImGui::SameLine(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - width);
}

float button_width(const std::string& label) {
    return ImGui::CalcTextSize(label.c_str()).x + ImGui::GetStyle().FramePadding.x * 2;
}

bool primary_button(const std::string& label, ImVec2 size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentBright);
    ImGui::PushStyleColor(ImGuiCol_Border, kAccent);
    const bool pressed = ImGui::Button(label.c_str(), size);
    ImGui::PopStyleColor(4);
    return pressed;
}

// An on/off switch; returns true when clicked.
bool toggle(const char* id, bool on) {
    const float h = em() * 1.25f, w = h * 1.9f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec4 track = on ? kAccent : (ImGui::IsItemHovered() ? kInnerHover : kInner);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(track), h / 2);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(on ? kAccent : kBorder), h / 2);
    const float r = h / 2 - h * 0.15f;
    dl->AddCircleFilled(ImVec2(on ? p.x + w - h / 2 : p.x + h / 2, p.y + h / 2), r, ImGui::GetColorU32(kText));
    return clicked;
}

// A telemetry tile: icon, label, value.
void tile(const char* id, Icon icon, const char* label, const std::string& value, float w, float h) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kInner);
    ImGui::BeginChild(id, ImVec2(w, h), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();
    const float content_h = em() * 2.45f;
    ImGui::SetCursorPos(ImVec2(em() * 0.9f, (h - content_h) / 2 + em() * 0.3f));
    ImGui::PushFont(nullptr, base() * 1.8f);
    ImGui::PushStyleColor(ImGuiCol_Text, kAccentBright);
    ImGui::TextUnformatted(ic(icon).c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::SameLine(0, em() * 0.8f);
    ImGui::SetCursorPosY((h - content_h) / 2);
    ImGui::BeginGroup();
    dim(label);
    text_bold(value.c_str(), 1.3f);
    ImGui::EndGroup();
    ImGui::EndChild();
}

void telemetry_tiles(const UiState& s) {
    const Telemetry& t = s.telemetry;
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float w = (ImGui::GetContentRegionAvail().x - gap * 4) / 5, h = em() * 4.2f;
    const std::string core = s.telemetry_ready ? reading(t.core_mhz, " MHz") : "N/A";
    const std::string memory = s.telemetry_ready ? reading(t.mem_mhz, " MHz") : "N/A";
    const std::string temperature = s.telemetry_ready ? reading(t.temp_c, " \xC2\xB0""C") : "N/A";
    const std::string power = s.telemetry_ready ? reading(t.power_w, "") + " / " + reading(t.power_limit_w, " W")
                                                : "N/A / N/A";
    tile("core", kIconChip, "Core clock", core, w, h);
    ImGui::SameLine();
    tile("mem", kIconMemory, "Memory clock", memory, w, h);
    ImGui::SameLine();
    tile("temp", kIconTemp, "Temperature", temperature, w, h);
    ImGui::SameLine();
    tile("power", kIconPower, "Power", power, w, h);
    ImGui::SameLine();
    const char* mode = s.fan_state.mode == FanMode::Curve     ? " (curve)"
                       : s.fan_state.mode == FanMode::Foreign ? " (other program)"
                       : s.fan_control && s.fan_available     ? " (driver)"
                                                              : "";
    tile("fan", kIconFan, "Fan speed", s.telemetry_ready ? reading(t.fan_pct, " %") + mode : "N/A", w, h);
}

// The four presets side by side; clicking one selects it.
void preset_cards(UiState& s) {
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float w = (ImGui::GetContentRegionAvail().x - gap * 3) / 4, h = em() * 7.6f;
    int i = 0;
    for (const PresetInfo& p : kPresets) {
        ImGui::PushID(i);
        const bool selected = s.preset == p.preset;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, selected ? kSelectedBg : kInner);
        ImGui::PushStyleColor(ImGuiCol_Border, selected ? kAccentBright : kBorder);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, selected ? 2.0f : 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em() * 0.8f, em() * 0.7f));
        ImGui::BeginChild("preset", ImVec2(w, h), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar);
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
        if (ImGui::RadioButton("##pick", selected)) s.preset = p.preset;
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, selected ? kAccentBright : kDim);
        ImGui::TextUnformatted(ic(p.icon).c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        text_bold(p.title, 1.05f);
        wrapped(kDim, std::string(p.summary) + " Up to " + std::to_string(objectives_for(p.preset).max_temp_c) +
                          " \xC2\xB0""C.");
        ImGui::EndChild();
        if (ImGui::IsItemClicked()) s.preset = p.preset;
        if (i < 3) ImGui::SameLine();
        ImGui::PopID();
        ++i;
    }
}

// ------------------------------------------------------------------ dashboard

void gpu_card(const UiState& s, const UiActions& act) {
    begin_card("gpu");
    const float top = ImGui::GetCursorPosY();
    const std::string detect = with_icon(kIconRefresh, "Detect GPUs");
    ImGui::BeginGroup();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, em() * 0.15f));
    const auto selected = std::find_if(s.gpus.begin(), s.gpus.end(),
                                       [&](const app::GpuInfo& gpu) { return gpu.uuid == s.selected_gpu; });
    const std::string selected_label = selected == s.gpus.end()
                                           ? (s.selected_gpu.empty() ? "No NVIDIA GPU found" : "Selected GPU unavailable")
                                           : app::gpu_label(*selected);
    ImGui::SetNextItemWidth(std::max(em() * 10, ImGui::GetContentRegionAvail().x -
                                                        button_width(detect) - ImGui::GetStyle().ItemSpacing.x));
    if (ImGui::BeginCombo("##selected_gpu", selected_label.c_str())) {
        for (const app::GpuInfo& gpu : s.gpus) {
            const std::string label = app::gpu_label(gpu);
            const bool is_selected = gpu.uuid == s.selected_gpu;
            if (ImGui::Selectable(label.c_str(), is_selected) && !is_selected && act.select_gpu)
                act.select_gpu(gpu.uuid);
            if (is_selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    dim(("Driver " + (s.driver.empty() ? std::string("unknown") : s.driver)).c_str());
    ImGui::PopStyleVar();
    ImGui::EndGroup();
    align_right(button_width(detect));
    ImGui::SetCursorPosY(top);
    if (ImGui::Button(detect.c_str())) act.detect_gpu();
    ImGui::Spacing();
    if (!s.gpu_error.empty()) wrapped(kWarn, s.gpu_error);
    telemetry_tiles(s);
    end_card();
}

void tuning_row(const char* label, const std::string& value, const ImVec4& color = kText) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    dim(label);
    ImGui::TableNextColumn();
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(value.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

void tuning_card(const UiState& s, const UiActions& act) {
    begin_card("tuning");
    heading("Current tuning");
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    int rows = s.applied ? 5 : 2;
    if (s.profile && (!s.profile_driver_ok || !s.profile_gpu_ok)) ++rows;
    if (s.strikes > 0) ++rows;
    const float row_h = em() + ImGui::GetStyle().CellPadding.y * 2;
    const float h = std::max(em() * 9.6f, rows * row_h + em() * 1.8f);
    const float right_w = std::max(em() * 17, ImGui::GetContentRegionAvail().x * 0.36f);

    begin_card("values", h, kInner, ImGui::GetContentRegionAvail().x - right_w - gap);
    if (ImGui::BeginTable("rows", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, em() * 10);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
        if (s.applied) {
            const AppliedState& a = *s.applied;
            const bool stock = a.core_mhz == 0 && a.mem_mhz == 0 && a.power_pct >= 99 && a.power_pct <= 101;
            const bool ours = s.profile && a.core_mhz == s.profile->core_mhz && a.mem_mhz == s.profile->mem_mhz &&
                              a.power_pct >= s.profile->power_pct - 1 && a.power_pct <= s.profile->power_pct + 1;
            if (ours) tuning_row("Applied now", std::string(title_of(s.profile->preset)) + " (the saved tune)", kGood);
            else if (stock) tuning_row("Applied now", "Stock");
            else tuning_row("Applied now", "Set by another program", kWarn);
            tuning_row("Core clock offset", offset(a.core_mhz));
            tuning_row("Memory clock offset", offset(a.mem_mhz));
            tuning_row("Power limit", std::to_string(a.power_pct) + " %");
        } else {
            tuning_row("Applied now", "Unknown (the driver does not report it)", kDim);
        }
        if (s.profile) {
            const Profile& p = *s.profile;
            tuning_row("Saved tune", std::string(title_of(p.preset)) + ": " + std::to_string(p.power_pct) + " %, core " +
                                         offset(p.core_mhz) + ", memory " + offset(p.mem_mhz) + " (" + p.saved_at + ")");
            if (!s.profile_driver_ok || !s.profile_gpu_ok)
                tuning_row("", "Tuned on driver " + p.driver + (s.profile_gpu_ok ? "" : " and another card") +
                                   ". Optimize again before relying on it.",
                           kWarn);
        } else {
            tuning_row("Saved tune", "None yet. Pick a profile and optimize.", kDim);
        }
        if (s.strikes > 0)
            tuning_row("Crash strikes", std::to_string(s.strikes) + " of " + std::to_string(kMaxBootStrikes) +
                                            " (apply at logon stops at " + std::to_string(kMaxBootStrikes) + ")",
                       kWarn);
        ImGui::EndTable();
    }
    end_card();

    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kInner);
    ImGui::BeginChild("actions", ImVec2(0, h), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleColor();
    const float bw = ImGui::GetContentRegionAvail().x, bh = em() * 2.3f;
    if (s.elevated) {
        ImGui::BeginDisabled(!s.profile);
        if (primary_button("Apply saved tune", ImVec2(bw, bh))) act.apply_profile();
        ImGui::EndDisabled();
    } else {
        if (primary_button("Restart as administrator", ImVec2(bw, bh))) act.restart_elevated();
    }
    // Apply at logon: a framed row with a switch.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kCard);
    ImGui::BeginChild("logon", ImVec2(bw, bh), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();
    ImGui::SetCursorPos(ImVec2(em() * 0.8f, (bh - em()) / 2 - 1));
    ImGui::TextUnformatted("Apply at logon");
    ImGui::SameLine(bw - em() * 3.4f);
    ImGui::SetCursorPosY((bh - em() * 1.25f) / 2 - 1);
    // Turning it off never needs a profile; turning it on does.
    ImGui::BeginDisabled(!s.elevated || (!s.has_profiles && !s.boot_on));
    if (toggle("##logon", s.boot_on)) act.set_boot(!s.boot_on);
    ImGui::EndDisabled();
    ImGui::EndChild();
    ImGui::BeginDisabled(!s.elevated);
    if (ImGui::Button(with_icon(kIconUndo, "Revert to stock").c_str(), ImVec2(bw, bh))) act.revert_to_stock();
    ImGui::EndDisabled();
    ImGui::EndChild();
    end_card();
}

void profiles_card(UiState& s) {
    begin_card("profiles");
    heading("Optimization profiles");
    const std::string go = with_icon(kIconPulse, "Optimize...");
    align_right(button_width(go));
    if (ImGui::Button(go.c_str())) s.page = Page::Optimize;
    ImGui::Spacing();
    preset_cards(s);
    end_card();
}

struct LogLine {
    std::string time, order, text;
    bool warn;
};

std::string display_log_time(const std::string& value) {
    if (value.size() >= 16 && value[4] == '-' && value[7] == '-' && value[10] == ' ') {
        if (value.size() >= 23) return value.substr(11, 12);
        return value.substr(11, 5) + ":00.000";
    }
    return value;
}

// boot.log lines and this session's notes, newest first.
std::vector<LogLine> log_lines(const UiState& s) {
    std::vector<LogLine> out;
    for (const std::string& line : s.boot_log) {
        const size_t sep = line.find("  ");
        const std::string timestamp = sep == std::string::npos ? "" : line.substr(0, sep);
        LogLine l{display_log_time(timestamp), timestamp, sep == std::string::npos ? line : line.substr(sep + 2), false};
        l.warn = l.text.find("not applied") != std::string::npos || l.text.find("failed") != std::string::npos ||
                 l.text.find("stopped") != std::string::npos || l.text.find("another program") != std::string::npos;
        out.push_back(std::move(l));
    }
    for (const Note& n : s.notes) out.push_back({display_log_time(n.time), n.time, n.text, n.warn});
    std::stable_sort(out.begin(), out.end(), [](const LogLine& a, const LogLine& b) { return a.order > b.order; });
    return out;
}

void log_card(const UiState& s) {
    begin_card("log");
    heading("Log");
    const std::string more = with_icon(kIconList, g_full_log ? "Show recent" : "View full log");
    align_right(button_width(more));
    if (ImGui::Button(more.c_str())) g_full_log = !g_full_log;
    const auto lines = log_lines(s);
    if (lines.empty()) {
        dim("Nothing yet.");
    } else if (ImGui::BeginTable("log", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_BordersOuter |
                                               ImGuiTableFlags_PadOuterX)) {
        ImGui::TableSetupColumn("time", ImGuiTableColumnFlags_WidthFixed, em() * 8.5f);
        ImGui::TableSetupColumn("text", ImGuiTableColumnFlags_WidthStretch);
        const size_t n = g_full_log ? lines.size() : std::min(lines.size(), kLogShort);
        for (size_t i = 0; i < n; ++i) {
            ImGui::TableNextRow(ImGuiTableRowFlags_None, em() * 1.7f);
            ImGui::TableNextColumn();
            dim(lines[i].time.c_str());
            ImGui::TableNextColumn();
            wrapped(lines[i].warn ? kWarn : kText, lines[i].text);
        }
        ImGui::EndTable();
    }
    end_card();
}

constexpr FanPreset kFanPresets[] = {FanPreset::Silent, FanPreset::Normal, FanPreset::Cool, FanPreset::Aggressive};
constexpr const char* kFanPresetTitles[] = {"Silent", "Normal", "Cool", "Aggressive"};

// One button per fan preset; `current` is highlighted. Returns the clicked one.
std::optional<FanPreset> fan_preset_buttons(const char* id, const std::optional<FanPreset>& current) {
    std::optional<FanPreset> clicked;
    ImGui::PushID(id);
    for (size_t i = 0; i < std::size(kFanPresets); ++i) {
        if (i > 0) ImGui::SameLine();
        const bool on = current == kFanPresets[i];
        if (on) {
            ImGui::PushStyleColor(ImGuiCol_Button, kSelectedBg);
            ImGui::PushStyleColor(ImGuiCol_Border, kAccentBright);
        }
        if (ImGui::Button(kFanPresetTitles[i])) clicked = kFanPresets[i];
        if (on) ImGui::PopStyleColor(2);
    }
    ImGui::PopID();
    return clicked;
}

// The preset whose curve equals c, if any.
std::optional<FanPreset> preset_of(const FanCurve& c) {
    for (const FanPreset p : kFanPresets)
        if (fan_preset_curve(p) == c) return p;
    return std::nullopt;
}

// Temperature 20-100 C on x, fan 0-100 % on y. Points drag with the mouse and
// stay ordered: a point cannot pass its neighbours in temperature, and fan %
// never goes down from one point to the next. Returns true when a drag ended,
// so the caller saves once per edit rather than every frame.
bool curve_editor(FanCurve& c, int min_pct, int max_temp_c, int now_temp_c, int* selected) {
    const float w = ImGui::GetContentRegionAvail().x, h = em() * 16;
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float pad = em() * 2.2f;
    const ImVec2 a(o.x + pad, o.y + em() * 0.5f), b(o.x + w - em() * 0.5f, o.y + h - pad);
    auto to_screen = [&](float t, float p) {
        return ImVec2(a.x + (t - 20) / 80 * (b.x - a.x), b.y - p / 100 * (b.y - a.y));
    };
    dl->AddRectFilled(a, b, ImGui::GetColorU32(kInner), em() * 0.3f);
    for (int t = 20; t <= 100; t += 10) {
        const ImVec2 p = to_screen(static_cast<float>(t), 0);
        dl->AddLine(ImVec2(p.x, a.y), ImVec2(p.x, b.y), ImGui::GetColorU32(kBorder));
        dl->AddText(ImVec2(p.x - em() * 0.6f, b.y + em() * 0.3f), ImGui::GetColorU32(kDim), (std::to_string(t) + "\xC2\xB0").c_str());
    }
    for (int p = 0; p <= 100; p += 25) {
        const ImVec2 q = to_screen(20, static_cast<float>(p));
        dl->AddLine(ImVec2(a.x, q.y), ImVec2(b.x, q.y), ImGui::GetColorU32(kBorder));
        dl->AddText(ImVec2(o.x, q.y - em() * 0.5f), ImGui::GetColorU32(kDim), (std::to_string(p) + "%").c_str());
    }
    // Fan-stop zone, the card's minimum, the profile's limit and the current temperature.
    if (c.stop_below_c) {
        const ImVec2 z = to_screen(static_cast<float>(*c.stop_below_c), 0);
        dl->AddRectFilled(a, ImVec2(z.x, b.y), ImGui::GetColorU32(ImVec4(kGood.x, kGood.y, kGood.z, 0.10f)));
    }
    const ImVec2 m = to_screen(20, static_cast<float>(min_pct));
    dl->AddLine(ImVec2(a.x, m.y), ImVec2(b.x, m.y), ImGui::GetColorU32(kWarn), 1.0f);
    const ImVec2 lim = to_screen(static_cast<float>(max_temp_c), 100);
    dl->AddLine(ImVec2(lim.x, a.y), ImVec2(lim.x, b.y), ImGui::GetColorU32(kBad), 1.5f);
    if (now_temp_c >= 20) {
        const ImVec2 n = to_screen(static_cast<float>(std::min(now_temp_c, 100)), 0);
        dl->AddLine(ImVec2(n.x, a.y), ImVec2(n.x, b.y), ImGui::GetColorU32(kText), 1.0f);
    }
    // What the fans actually do, guard rails included: exact, not rounded to
    // whole percents, so straight segments stay straight and the jumps at the
    // stop threshold and the limit are vertical.
    auto effective = [&](float t) {
        if (t >= static_cast<float>(max_temp_c)) return 100.0f;
        if (c.stop_below_c && t < static_cast<float>(*c.stop_below_c)) return 0.0f;
        const auto& pts = c.points;
        float v = static_cast<float>(pts.back().pct);
        if (t <= static_cast<float>(pts.front().temp_c)) {
            v = static_cast<float>(pts.front().pct);
        } else {
            for (size_t k = 1; k < pts.size(); ++k) {
                if (t > static_cast<float>(pts[k].temp_c)) continue;
                const float f = (t - static_cast<float>(pts[k - 1].temp_c)) / static_cast<float>(pts[k].temp_c - pts[k - 1].temp_c);
                v = static_cast<float>(pts[k - 1].pct) + f * static_cast<float>(pts[k].pct - pts[k - 1].pct);
                break;
            }
        }
        return std::max(v, static_cast<float>(min_pct));
    };
    std::vector<ImVec2> line;
    for (int i = 0; i <= 80 * 8; ++i) {
        const float t = 20 + static_cast<float>(i) / 8;
        line.push_back(to_screen(t, effective(t)));
    }
    dl->AddPolyline(line.data(), static_cast<int>(line.size()), ImGui::GetColorU32(kAccentBright), ImDrawFlags_None, 2.5f);
    bool released = false;
    for (int i = 0; i < static_cast<int>(c.points.size()); ++i) {
        FanPoint& pt = c.points[static_cast<size_t>(i)];
        const ImVec2 p = to_screen(static_cast<float>(pt.temp_c), static_cast<float>(pt.pct));
        const float r = em() * 0.45f;
        ImGui::SetCursorScreenPos(ImVec2(p.x - r * 1.5f, p.y - r * 1.5f));
        ImGui::PushID(i);
        ImGui::InvisibleButton("pt", ImVec2(r * 3, r * 3));
        if (ImGui::IsItemActivated()) *selected = i;
        if (ImGui::IsItemActive()) {
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            const int lo_t = i > 0 ? c.points[static_cast<size_t>(i) - 1].temp_c + 1 : 20;
            const int hi_t = i + 1 < static_cast<int>(c.points.size()) ? c.points[static_cast<size_t>(i) + 1].temp_c - 1 : 100;
            const int lo_p = i > 0 ? c.points[static_cast<size_t>(i) - 1].pct : 0;
            const int hi_p = i + 1 < static_cast<int>(c.points.size()) ? c.points[static_cast<size_t>(i) + 1].pct : 100;
            pt.temp_c = std::clamp(static_cast<int>(std::lround(20 + (mouse.x - a.x) / (b.x - a.x) * 80)), lo_t, hi_t);
            pt.pct = std::clamp(static_cast<int>(std::lround((b.y - mouse.y) / (b.y - a.y) * 100)), lo_p, hi_p);
        }
        if (ImGui::IsItemDeactivated()) released = true;
        const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive() || *selected == i;
        // A point a guard rail overrides is drawn hollow, off the line, with the reason.
        const char* why = pt.temp_c >= max_temp_c ? "the profile's limit: the fans run at 100 % here"
                          : c.stop_below_c && pt.temp_c < *c.stop_below_c ? "the fan-stop zone: the driver has the fans here"
                          : pt.pct < min_pct ? "raised to the card's minimum"
                                             : nullptr;
        const ImU32 col = ImGui::GetColorU32(hot ? kText : kAccentBright);
        if (why) {
            dl->AddCircleFilled(p, r, ImGui::GetColorU32(kInner));
            dl->AddCircle(p, r, col, 0, 2.0f);
        } else {
            dl->AddCircleFilled(p, r, col);
        }
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
            if (why) ImGui::SetTooltip("%d \xC2\xB0""C  %d %%\nOverridden by %s", pt.temp_c, pt.pct, why);
            else ImGui::SetTooltip("%d \xC2\xB0""C  %d %%", pt.temp_c, pt.pct);
        }
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + h));
    ImGui::Dummy(ImVec2(w, 0));
    return released;
}

void fan_page(UiState& s, const UiActions& act) {
    static FanCurve edit;          // the curve being edited; saved when a drag ends
    static bool editing = false;
    static int selected = -1;
    begin_card("fan");
    heading("Fan curve");
    if (!s.fan_available) {
        wrapped(kDim, s.elevated ? "This card's fans cannot be controlled through the NVIDIA driver."
                                 : "Fan control needs administrator rights. Restart as administrator from the dashboard.");
        end_card();
        return;
    }
    if (!s.fan_curve) {
        wrapped(kDim, "Optimize first: the fan curve belongs to a saved tune.");
        end_card();
        return;
    }
    if (!editing) edit = *s.fan_curve;

    const std::string label = "Fan control";
    ImGui::TextUnformatted(label.c_str());
    ImGui::SameLine();
    if (toggle("##fanon", s.fan_control)) act.set_fan_control(!s.fan_control);
    ImGui::SameLine(0, em() * 2);
    switch (s.fan_state.mode) {
        case FanMode::Curve: dim(("Curve: " + std::to_string(s.fan_state.pct) + " %").c_str()); break;
        case FanMode::Foreign: ImGui::TextColored(kWarn, "Another program controls the fans."); break;
        case FanMode::Failed: ImGui::TextColored(kBad, "A fan speed did not verify; the driver controls the fans."); break;
        case FanMode::Driver: dim(s.fan_control ? "The NVIDIA driver controls the fans right now." : "Off: the NVIDIA driver controls the fans."); break;
    }
    ImGui::Spacing();

    const bool released = curve_editor(edit, s.fan_min_pct, s.fan_max_temp_c, s.telemetry.temp_c, &selected);
    editing = ImGui::IsAnyItemActive();
    if (released && valid(edit)) act.set_fan_curve(edit);

    bool stop = edit.stop_below_c.has_value();
    if (ImGui::Checkbox("Stop the fans below", &stop)) {
        edit.stop_below_c = stop ? std::optional<int>(50) : std::nullopt;
        act.set_fan_curve(edit);
    }
    if (stop) {
        ImGui::SameLine();
        int v = *edit.stop_below_c;
        ImGui::SetNextItemWidth(em() * 12);
        ImGui::SliderInt("##stop", &v, 30, 70, "%d \xC2\xB0""C");
        edit.stop_below_c = v;
        if (ImGui::IsItemDeactivatedAfterEdit()) act.set_fan_curve(edit);
    }
    ImGui::SameLine(0, em() * 2);
    ImGui::BeginDisabled(edit.points.size() >= 6);
    if (ImGui::Button("Add point")) {
        // Halfway along the widest temperature gap.
        size_t at = 1;
        for (size_t i = 1; i < edit.points.size(); ++i)
            if (edit.points[i].temp_c - edit.points[i - 1].temp_c > edit.points[at].temp_c - edit.points[at - 1].temp_c) at = i;
        const FanPoint& l = edit.points[at - 1];
        const FanPoint& r = edit.points[at];
        edit.points.insert(edit.points.begin() + static_cast<std::ptrdiff_t>(at), FanPoint{(l.temp_c + r.temp_c) / 2, (l.pct + r.pct) / 2});
        if (valid(edit)) act.set_fan_curve(edit);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(edit.points.size() <= 2 || selected < 0 || selected >= static_cast<int>(edit.points.size()));
    if (ImGui::Button("Remove point")) {
        edit.points.erase(edit.points.begin() + selected);
        selected = -1;
        act.set_fan_curve(edit);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Profile default")) {
        selected = -1;
        act.reset_fan_curve();
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Presets");
    ImGui::SameLine(0, em());
    if (const auto p = fan_preset_buttons("fan", preset_of(edit))) {
        selected = -1;
        edit = fan_preset_curve(*p);
        act.set_fan_curve(edit);
    }

    ImGui::Spacing();
    if (s.fan_tested && quieter_than(edit, *s.fan_tested, s.fan_max_temp_c))
        wrapped(kWarn, "This curve is quieter than the one the tune was tested with, so the card runs warmer than during the "
                       "test. Optimize again to be sure the tune holds.");
    wrapped(kDim, "Below the card's minimum (yellow line) a fan cannot run slower. The fans start at the stop threshold and stop "
                  "again only once the card is 8 \xC2\xB0""C cooler and idle for a minute (longer if they had to restart soon "
                  "after stopping); then the NVIDIA driver has them. From the red line (the profile's temperature limit) the fans "
                  "always run at 100 %.");
    end_card();
}

void dashboard(UiState& s, const UiActions& act) {
    gpu_card(s, act);
    tuning_card(s, act);
    profiles_card(s);
    log_card(s);
}

// ------------------------------------------------------------------ optimize

void plot(const char* label, const std::deque<float>& h, float lo, float hi, const char* overlay, float w) {
    std::vector<float> v(h.begin(), h.end());
    if (v.empty()) v.push_back(0);
    ImGui::PlotLines(label, v.data(), static_cast<int>(v.size()), 0, overlay, lo, hi, ImVec2(w, em() * 5));
}

void choose(UiState& s, const UiActions& act) {
    begin_card("choose");
    heading("Choose a profile");
    dim("Each profile searches for the highest stable settings and applies a safety margin.");
    ImGui::Spacing();
    preset_cards(s);
    ImGui::Spacing();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Fan curve during the run");
    ImGui::SameLine(0, em());
    if (ImGui::RadioButton("Profile's own", !s.optimize_fan)) s.optimize_fan.reset();
    ImGui::SameLine(0, em());
    if (const auto p = fan_preset_buttons("run", s.optimize_fan)) s.optimize_fan = p;
    ImGui::Spacing();
    const ImVec2 big(em() * 13, em() * 2.6f);
    ImGui::PushFont(g_bold, base() * 1.15f);
    const bool go = s.elevated ? primary_button(with_icon(kIconPlay, "Optimize GPU"), big)
                               : primary_button("Restart as administrator", big);
    ImGui::PopFont();
    if (go) s.elevated ? act.optimize(s.preset) : act.restart_elevated();
    ImGui::SameLine(0, em() * 1.5f);
    ImGui::BeginGroup();
    ImGui::TextUnformatted((ic(kIconClock) + "  About 10 minutes of full GPU load.").c_str());
    dim("Abort restores stock at any time. A setting that crashes the machine is never tried again.");
    ImGui::EndGroup();
    end_card();
}

void run_screen(UiState& s, const OptimizeWorker::Snapshot& run, const UiActions& act) {
    begin_card("run");
    text_bold((std::string("Optimizing: ") + title_of(s.preset)).c_str(), 1.4f);
    if (run.running) {
        const std::string abort = with_icon(kIconStop, "Abort");
        align_right(button_width(abort) + em());
        if (ImGui::Button(abort.c_str(), ImVec2(button_width(abort) + em(), 0))) act.abort();
    }
    dim("Aborting finishes the current probe, then restores stock.");
    ImGui::Spacing();
    telemetry_tiles(s);
    ImGui::Spacing();
    const float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2;
    char overlay[64];
    std::snprintf(overlay, sizeof(overlay), "temperature %s", reading(s.telemetry.temp_c, " C").c_str());
    plot("##temp", s.temp_history, 30, 90, overlay, w);
    ImGui::SameLine();
    std::snprintf(overlay, sizeof(overlay), "power %s W", reading(s.telemetry.power_w, "").c_str());
    plot("##power", s.power_history, 0, static_cast<float>(std::max(s.telemetry.power_limit_w, 1)), overlay, w);
    end_card();

    begin_card("probes");
    heading("Probes");
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kInner);
    ImGui::BeginChild("lines", ImVec2(0, std::max(em() * 10, ImGui::GetContentRegionAvail().y - em())),
                      ImGuiChildFlags_Borders);
    ImGui::PopStyleColor();
    for (const std::string& line : run.log) ImGui::TextUnformatted(line.c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    end_card();
    if (!run.running && run.outcome) s.screen = Screen::Results;
}

void results(UiState& s, const OptimizeWorker::Snapshot& run, const UiActions& act) {
    if (!run.outcome) {
        s.screen = Screen::Choose;
        return;
    }
    const app::OptimizeOutcome& o = *run.outcome;
    begin_card("results");
    if (!o.ran) {
        heading("Not started");
        wrapped(kBad, o.error);
    } else if (!o.result.ok) {
        heading("Not applied");
        wrapped(kBad, o.result.reason);
        wrapped(kText, o.result.stock_restored ? "The card is back at stock." : "Reset to stock FAILED -- run `gao --reset`.");
    } else {
        const OptimizeResult& r = o.result;
        heading("Done");
        char line[160];
        std::snprintf(line, sizeof(line), "Applied: power %d %%, core +%d MHz, memory +%d MHz", r.power_pct, r.core_mhz,
                      r.mem_mhz);
        wrapped(kGood, line);
        std::snprintf(line, sizeof(line), "Confirmed edges: core +%d MHz, memory +%d MHz. The difference is the safety margin.",
                      r.core_confirmed, r.mem_confirmed);
        wrapped(kDim, line);
        ImGui::Spacing();
        if (ImGui::BeginTable("cmp", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_BordersOuter |
                                            ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX)) {
            ImGui::TableSetupColumn("");
            ImGui::TableSetupColumn("Before (stock)");
            ImGui::TableSetupColumn("After");
            ImGui::TableHeadersRow();
            auto row = [](const char* name, const std::string& a, const std::string& b) {
                ImGui::TableNextRow(ImGuiTableRowFlags_None, em() * 1.6f);
                ImGui::TableNextColumn(); dim(name);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(a.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(b.c_str());
            };
            const double gain = r.baseline.score > 0 ? (r.soak.score / r.baseline.score - 1) * 100 : 0;
            char sb[32], sa[48];
            std::snprintf(sb, sizeof(sb), "%.0f it/s", r.baseline.score);
            std::snprintf(sa, sizeof(sa), "%.0f it/s (%+.1f %%)", r.soak.score, gain);
            row("Score", sb, sa);
            row("Core clock", reading(r.baseline.avg_core_mhz, " MHz"), reading(r.soak.avg_core_mhz, " MHz"));
            row("Memory clock", reading(r.baseline.avg_mem_mhz, " MHz"), reading(r.soak.avg_mem_mhz, " MHz"));
            row("Peak temperature", reading(r.baseline.peak_temp_c, " \xC2\xB0""C"), reading(r.soak.peak_temp_c, " \xC2\xB0""C"));
            row("Power", reading(r.baseline.avg_power_w, " W"), reading(r.soak.avg_power_w, " W"));
            ImGui::EndTable();
        }
        ImGui::Spacing();
        if (o.saved) wrapped(kText, "Saved. It stays applied until reboot; turn on apply at logon to keep it.");
        else wrapped(kWarn, "Not saved: " + o.save_note);
        if (o.saved && !s.boot_on && primary_button("Apply at every logon")) act.set_boot(true);
        if (o.saved && !s.boot_on) ImGui::SameLine();
        if (ImGui::Button(with_icon(kIconUndo, "Revert to stock").c_str())) act.revert_to_stock();
        ImGui::SameLine();
    }
    if (ImGui::Button(with_icon(kIconBack, "Back").c_str())) s.screen = Screen::Choose;
    end_card();
}

void optimize_page(UiState& s, const OptimizeWorker::Snapshot& run, const UiActions& act) {
    if (run.running) s.screen = Screen::Run;
    switch (s.screen) {
        case Screen::Choose: choose(s, act); break;
        case Screen::Run: run_screen(s, run, act); break;
        case Screen::Results: results(s, run, act); break;
    }
}

// ------------------------------------------------------------------ about

void about() {
    begin_card("about");
    text_bold("GPU Auto Optimizer", 1.6f);
    dim(("Version " + std::string(kVersion)).c_str());
    ImGui::Spacing();
    wrapped(kText, "Finds the power limit and the core and memory clock offsets your NVIDIA card runs stably at, and keeps them "
                   "applied. Every setting it writes is read back and checked, a failed apply ends at stock, and a crash journal "
                   "makes sure a setting that froze the machine is never tried again.");
    ImGui::Spacing();
    wrapped(kDim, "State: %ProgramData%\\GpuAutoOptimizer (gao.json, journal.jsonl, boot.log)");
    wrapped(kDim, "Command line: gao.exe --help");
    wrapped(kDim, "Source: github.com/Rovey/gpu-auto-optimizer. MIT License.");
    end_card();
}

// ------------------------------------------------------------------ frame

void nav_item(UiState& s, Page page, Icon icon, const char* label) {
    const bool selected = s.page == page;
    const ImVec2 size(ImGui::GetContentRegionAvail().x, em() * 2.4f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton(label, size)) s.page = page;
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (selected || hovered)
        dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), ImGui::GetColorU32(selected ? kAccent : kInner), em() * 0.35f);
    const float y = p.y + (size.y - em()) / 2;
    dl->AddText(ImVec2(p.x + em() * 0.9f, y), ImGui::GetColorU32(kText), ic(icon).c_str());
    dl->AddText(ImVec2(p.x + em() * 2.6f, y), ImGui::GetColorU32(kText), label);
}

void sidebar(UiState& s) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kNav);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em() * 0.6f, em() * 0.8f));
    ImGui::BeginChild("nav", ImVec2(em() * 13, 0), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    nav_item(s, Page::Dashboard, kIconHome, "Dashboard");
    nav_item(s, Page::Optimize, kIconPulse, "Optimize");
    nav_item(s, Page::Fan, kIconFan, "Fan");
    nav_item(s, Page::About, kIconInfo, "About");
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - em() * 2.2f);
    ImGui::PushStyleColor(ImGuiCol_Text, s.elevated ? kGood : kDim);
    ImGui::TextUnformatted((ic(kIconShield) + (s.elevated ? "  Administrator" : "  Read-only")).c_str());
    ImGui::PopStyleColor();
    ImGui::EndChild();
}

}

void load_fonts(const std::string& fonts_dir) {
    ImGuiIO& io = ImGui::GetIO();
    auto add_icons = [&] {
        ImFontConfig cfg;
        cfg.MergeMode = true;
        cfg.GlyphOffset.y = 3.0f;   // the icon fonts sit higher than Segoe UI's baseline
        if (!io.Fonts->AddFontFromFileTTF((fonts_dir + "\\SegoeIcons.ttf").c_str(), 17.0f, &cfg))
            io.Fonts->AddFontFromFileTTF((fonts_dir + "\\segmdl2.ttf").c_str(), 17.0f, &cfg);   // Windows 10
    };
    g_regular = fonts_dir.empty() ? nullptr : io.Fonts->AddFontFromFileTTF((fonts_dir + "\\segoeui.ttf").c_str(), 17.0f);
    if (!g_regular) {
        g_regular = g_bold = io.Fonts->AddFontDefault();
        return;
    }
    add_icons();
    g_bold = io.Fonts->AddFontFromFileTTF((fonts_dir + "\\seguisb.ttf").c_str(), 17.0f);
    if (g_bold) add_icons();
    else g_bold = g_regular;
}

void apply_style(float scale) {
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    ImGui::StyleColorsDark();
    ImVec4* c = st.Colors;
    c[ImGuiCol_WindowBg] = kBg;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = kCard;
    c[ImGuiCol_Border] = kBorder;
    c[ImGuiCol_Text] = kText;
    c[ImGuiCol_TextDisabled] = kDim;
    c[ImGuiCol_FrameBg] = kInner;
    c[ImGuiCol_FrameBgHovered] = kInnerHover;
    c[ImGuiCol_FrameBgActive] = kInnerHover;
    c[ImGuiCol_Button] = kInner;
    c[ImGuiCol_ButtonHovered] = kInnerHover;
    c[ImGuiCol_ButtonActive] = kSelectedBg;
    c[ImGuiCol_CheckMark] = kAccentBright;
    c[ImGuiCol_Header] = kInner;
    c[ImGuiCol_HeaderHovered] = kInnerHover;
    c[ImGuiCol_HeaderActive] = kSelectedBg;
    c[ImGuiCol_TableHeaderBg] = kInner;
    c[ImGuiCol_TableBorderStrong] = kBorder;
    c[ImGuiCol_TableBorderLight] = kBorder;
    c[ImGuiCol_TableRowBg] = kInner;
    c[ImGuiCol_TableRowBgAlt] = ImVec4(0.100f, 0.114f, 0.137f, 1);
    c[ImGuiCol_PlotLines] = kAccentBright;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    st.WindowRounding = 0;
    st.ChildRounding = 8;
    st.FrameRounding = 6;
    st.GrabRounding = 6;
    st.FrameBorderSize = 1;
    st.ChildBorderSize = 1;
    st.FramePadding = ImVec2(12, 7);
    st.ItemSpacing = ImVec2(12, 12);
    st.CellPadding = ImVec2(10, 4);
    st.ScaleAllSizes(scale);
    st.FontScaleDpi = scale;
}

void push_history(std::deque<float>& h, float v) {
    h.push_back(v);
    while (h.size() > kHistory) h.pop_front();
}

void draw_ui(UiState& s, const OptimizeWorker::Snapshot& run, const UiActions& act) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("GPU Auto Optimizer", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar();
    sidebar(s);
    ImGui::SameLine(0, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em() * 0.9f, em() * 0.9f));
    ImGui::BeginChild("content", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    switch (s.page) {
        case Page::Dashboard: dashboard(s, act); break;
        case Page::Optimize: optimize_page(s, run, act); break;
        case Page::Fan: fan_page(s, act); break;
        case Page::About: about(); break;
    }
    ImGui::EndChild();
    ImGui::End();
}

}
