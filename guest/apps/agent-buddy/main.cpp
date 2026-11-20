// SPDX-License-Identifier: Apache-2.0
//
// Agent Buddy: a live list of the Claude Code sessions running on your Mac.
// Each row shows a session (its project folder), whether it is working,
// needs you, or is done and waiting for your next command. The Gotchi beeps
// when a session finishes or needs you.
//
// Pick a session and the Mac brings its terminal to the front. Hold TALK
// (or BOOT) to dictate the next command with Wispr Flow (Ctrl+Option push to
// talk over the "CGotchi" Bluetooth keyboard), then SEND (or VOL-) presses
// Enter in that session.
//
// The list comes from agent_bridge.py on the Mac (see README.md), which
// connects to this App over Wi-Fi on TCP port 7333. Lines from the Mac:
//   R\t<id>\t<state>\t<name>\t<info>\n   one row (state: working, needs,
//                                         done, ready, closed)
//   END\n                                 the rows so far are the new list
// Lines to the Mac:
//   F\t<id>\n                             focus that session's terminal
//
// Controls: swipe the list to scroll, tap a card or VOL+ to pick a session;
// hold "Hold to talk" or BOOT to speak; tap Send or VOL- to press Enter.
#include <stdint.h>
#include <string.h>

#include <array>
#include <span>

#include "agent-buddy_sfx_profiles.hpp"
#include "sdk/micropixel.hpp"

namespace {

using micropixel::Color;
using micropixel::HidKeyUsage;
using micropixel::Rect;
using micropixel::SystemFont;

constexpr uint16_t kPort = 7333U;
constexpr uint32_t kMaxSessions = 12U;
constexpr uint32_t kIdMax = 40U;
constexpr uint32_t kNameMax = 40U;
constexpr uint32_t kInfoMax = 40U;
constexpr uint32_t kLineMax = 200U;

// Layout for the 296x240 glass with 60 px corners.
// Glass rules: 60 px page corners, titles 26 px in, 12 px side margin.
constexpr uint32_t kCornerRadius = 60U;
constexpr int32_t kTitleInset = 26;
constexpr int32_t kSideMargin = 12;
constexpr int32_t kColumns = 4;
constexpr int32_t kGridTop = 52;
constexpr int32_t kGridBottom = 214;
constexpr int32_t kCellHeight = 78;
constexpr int32_t kSheetTop = 118;
constexpr int32_t kSwipe = 36;        // a swipe needs this many px
constexpr int32_t kTapSlop = 8;

constexpr Color kBackground = Color::Rgb(0U, 0U, 0U);  // outside the corner curve
constexpr Color kPage = Color::Rgb(12U, 12U, 15U);
constexpr Color kCell = Color::Rgb(24U, 24U, 29U);
constexpr Color kSheet = Color::Rgb(28U, 28U, 33U);
constexpr Color kPill = Color::Rgb(30U, 30U, 35U);
constexpr Color kText = Color::Rgb(245U, 245U, 247U);
constexpr Color kSecondary = Color::Rgb(142U, 142U, 147U);
constexpr Color kFaint = Color::Rgb(80U, 80U, 86U);
constexpr Color kButton = Color::Rgb(44U, 44U, 50U);
constexpr Color kAccent = Color::Rgb(10U, 132U, 255U);
constexpr Color kAlert = Color::Rgb(255U, 55U, 95U);
// Character colours: working yellow, needs you green, done blue.
constexpr Color kWorking = Color::Rgb(255U, 204U, 0U);
constexpr Color kNeeds = Color::Rgb(48U, 209U, 88U);
constexpr Color kDone = Color::Rgb(10U, 132U, 255U);
constexpr Color kClosed = Color::Rgb(99U, 99U, 102U);

enum class State : uint8_t { kWorking, kNeeds, kDone, kReady, kClosed };

struct Session final {
    char id[kIdMax + 1U]{};
    char name[kNameMax + 1U]{};
    char info[kInfoMax + 1U]{};
    State state{State::kReady};
};

// The Guest runtime has no strcmp or snprintf, so the app keeps its own.
bool Same(const char* left, const char* right) {
    while (*left != '\0' && *left == *right) {
        ++left;
        ++right;
    }
    return *left == *right;
}

// The Host rejects a scene with an empty label, so blank text becomes a space.
const char* NonEmpty(const char* text) { return text[0] == '\0' ? " " : text; }

// Appends text to a bounded buffer; `length` is the current end.
void Append(char* out, uint32_t capacity, uint32_t& length, const char* text) {
    while (*text != '\0' && length + 1U < capacity) {
        out[length++] = *text++;
    }
    out[length] = '\0';
}

void AppendNumber(char* out, uint32_t capacity, uint32_t& length, uint32_t value) {
    char digits[11];
    uint32_t count = 0U;
    do {
        digits[count++] = static_cast<char>('0' + value % 10U);
        value /= 10U;
    } while (value != 0U);
    char text[11];
    for (uint32_t index = 0U; index < count; ++index) {
        text[index] = digits[count - 1U - index];
    }
    text[count] = '\0';
    Append(out, capacity, length, text);
}

State ParseState(const char* text) {
    if (Same(text, "working")) return State::kWorking;
    if (Same(text, "needs")) return State::kNeeds;
    if (Same(text, "done")) return State::kDone;
    if (Same(text, "closed")) return State::kClosed;
    return State::kReady;
}

const char* StateText(State state) {
    switch (state) {
        case State::kWorking: return "Working";
        case State::kNeeds: return "Needs you";
        case State::kDone: return "Done";
        case State::kClosed: return "Closed";
        case State::kReady: break;
    }
    return "Ready";
}

Color StateColor(State state) {
    switch (state) {
        case State::kWorking: return kWorking;
        case State::kNeeds: return kNeeds;
        case State::kDone:
        case State::kReady: return kDone;
        case State::kClosed: break;
    }
    return kClosed;
}

// Copies at most `capacity - 1` bytes of the field and ends it.
void CopyField(char* out, uint32_t capacity, const char* text) {
    uint32_t length = 0U;
    while (text[length] != '\0' && length + 1U < capacity) {
        out[length] = text[length];
        ++length;
    }
    out[length] = '\0';
}

void FormatAddress(uint32_t address, char (&out)[24]) {
    uint32_t length = 0U;
    out[0] = '\0';
    for (uint32_t octet = 0U; octet < 4U; ++octet) {
        if (octet != 0U) Append(out, sizeof(out), length, ".");
        AppendNumber(out, sizeof(out), length, (address >> (octet * 8U)) & 0xFFU);
    }
}

micropixel::RoundedRectStyle Style(Color fill, uint32_t radius) {
    micropixel::RoundedRectStyle style{};
    style.fill = fill;
    style.stroke = fill;
    style.radius = radius;
    style.stroke_width = 0U;
    return style;
}

class AgentBuddyApp final {
   public:
    explicit AgentBuddyApp(micropixel::Application& app)
        : app_(app),
          keyboard_(app.hid_keyboard()),
          network_(app.network()),
          tones_(app.audio()),
          info_(app.renderer().info()),
          scene_(app.renderer().CreateScene(kBackground).value()),
          poll_(app.timers().Every(micropixel::Duration::Milliseconds(1000U)).value()) {
        BuildUi();
        if (!keyboard_.Start()) {
            app_.log().Error("Bluetooth keyboard unavailable");
        }
        StartServer();
        Refresh();
    }

    micropixel::EventResult Handle(const micropixel::Event& event) {
        if (event.TimerFrom(poll_) != nullptr || event.type() == micropixel::EventType::kResume) {
            StartServer();
            Refresh();
        } else if (const auto* network = event.network()) {
            if (network->kind() == micropixel::NetworkEventKind::kReady) {
                OnSocketReady(network->socket());
            } else {
                Refresh();
            }
        } else if (const auto* touch = event.touch()) {
            HandleTouch(*touch);
        } else if (const auto* key = event.key()) {
            HandleKey(*key);
        } else if (event.type() == micropixel::EventType::kStop) {
            EndTalk();
        }
        return micropixel::EventResult::kContinue;
    }

   private:
    // ---- UI ----

    enum class Page : uint8_t { kGrid, kSheet, kStats };

    void BuildUi() {
        width_ = static_cast<int32_t>(info_.width());
        height_ = static_cast<int32_t>(info_.height());
        (void)scene_.CreateRoundedRect({0, 0, width_, height_}, Style(kPage, kCornerRadius)).value();

        // Status pill, top centre: the Mac link.
        pill_rect_ = {width_ / 2 - 46, 6, 92, 18};
        (void)scene_.CreateRoundedRect(pill_rect_, Style(kPill, 9U)).value();
        pill_dot_ = scene_.CreateRoundedRect({pill_rect_.x + 8, pill_rect_.y + 6, 6, 6}, Style(kFaint, 3U)).value();
        pill_label_ = scene_.CreateLabel({pill_rect_.center_x() + 5, pill_rect_.y + 2}, " ", kSecondary,
                                         SystemFont::kSmall, true)
                          .value();

        // Small title on the left, coloured counts on the right.
        title_ = scene_.CreateLabel({kTitleInset, 28}, "Agents", kText, SystemFont::kMedium).value();
        const Color colors[3] = {kWorking, kNeeds, kDone};
        for (int32_t index = 0; index < 3; ++index) {
            const int32_t x = width_ - kTitleInset - 96 + index * 34;
            (void)scene_.CreateRoundedRect({x, 34, 8, 8}, Style(colors[index], 4U)).value();
            counts_[index] = scene_.CreateLabel({x + 11, 30}, "0", kText, SystemFont::kSmall).value();
        }

        // Character grid, clipped to its band and scrolled by translation.
        grid_rect_ = {kSideMargin, kGridTop, width_ - kSideMargin * 2, kGridBottom - kGridTop};
        micropixel::ContainerProperties clip{};
        clip.clip = grid_rect_;
        grid_ = scene_.CreateContainer(clip).value();
        const int32_t cell_width = grid_rect_.width / kColumns;
        for (uint32_t index = 0U; index < kMaxSessions; ++index) {
            Cell& cell = cells_[index];
            const int32_t column = static_cast<int32_t>(index) % kColumns;
            const int32_t row = static_cast<int32_t>(index) / kColumns;
            cell.rect = {grid_rect_.x + column * cell_width + 2, grid_rect_.y + row * kCellHeight + 2, cell_width - 4,
                         kCellHeight - 6};
            const int32_t cx = cell.rect.center_x();
            const int32_t top = cell.rect.y + 8;
            cell.ring = grid_.CreateRoundedRect(cell.rect, Style(kCell, 16U)).value();
            cell.body = grid_.CreateRoundedRect({cx - 19, top, 38, 36}, Style(kClosed, 14U)).value();
            cell.eye_left = grid_.CreateRoundedRect({cx - 11, top + 10, 9, 11}, Style(kText, 4U)).value();
            cell.eye_right = grid_.CreateRoundedRect({cx + 2, top + 10, 9, 11}, Style(kText, 4U)).value();
            cell.pupil_left = grid_.CreateRoundedRect({cx - 8, top + 14, 4, 5}, Style(kBackground, 2U)).value();
            cell.pupil_right = grid_.CreateRoundedRect({cx + 5, top + 14, 4, 5}, Style(kBackground, 2U)).value();
            cell.badge = grid_.CreateRoundedRect({cx + 12, top - 4, 12, 12}, Style(kAlert, 6U)).value();
            cell.name = grid_.CreateLabel({cx, top + 42}, " ", kSecondary, SystemFont::kSmall, true).value();
        }
        empty_title_ = scene_.CreateLabel({width_ / 2, 100}, " ", kText, SystemFont::kMedium, true).value();
        empty_hint_ = scene_.CreateLabel({width_ / 2, 124}, " ", kSecondary, SystemFont::kSmall, true).value();
        hint_ = scene_.CreateLabel({width_ / 2, 220}, "Swipe up for controls", kFaint, SystemFont::kSmall, true)
                    .value();

        // Controls sheet: slides up from the bottom, shaped like the glass.
        micropixel::ContainerProperties sheet{};
        sheet.clip = {0, kSheetTop, width_, height_ - kSheetTop};
        sheet.visible = false;
        sheet_ = scene_.CreateContainer(sheet).value();
        (void)sheet_.CreateRoundedRect({0, kSheetTop - 40, width_, height_ - kSheetTop + 40},
                                       Style(kSheet, kCornerRadius))
            .value();
        (void)sheet_.CreateRoundedRect({width_ / 2 - 18, kSheetTop + 6, 36, 4}, Style(kFaint, 2U)).value();
        sheet_dot_ = sheet_.CreateRoundedRect({kTitleInset, kSheetTop + 22, 10, 10}, Style(kClosed, 5U)).value();
        sheet_name_ = sheet_.CreateLabel({kTitleInset + 16, kSheetTop + 16}, " ", kText, SystemFont::kMedium).value();
        sheet_info_ = sheet_.CreateLabel({kTitleInset + 16, kSheetTop + 36}, " ", kSecondary, SystemFont::kSmall)
                          .value();
        const int32_t inner = 30;
        const int32_t gap = 10;
        const int32_t button_width = (width_ - inner * 2 - gap) / 2;
        talk_rect_ = {inner, kSheetTop + 60, button_width, 32};
        send_rect_ = {inner + button_width + gap, kSheetTop + 60, button_width, 32};
        talk_box_ = sheet_.CreateRoundedRect(talk_rect_, Style(kAccent, 16U)).value();
        (void)sheet_.CreateRoundedRect(send_rect_, Style(kButton, 16U)).value();
        talk_label_ = sheet_.CreateLabel({talk_rect_.center_x(), talk_rect_.y + 7}, "Hold to Talk", kText,
                                         SystemFont::kMedium, true)
                          .value();
        send_label_ = sheet_.CreateLabel({send_rect_.center_x(), send_rect_.y + 7}, "Send", kText,
                                         SystemFont::kMedium, true)
                          .value();

        // Stats page: swipe right from the grid.
        micropixel::ContainerProperties stats{};
        stats.visible = false;
        stats_ = scene_.CreateContainer(stats).value();
        (void)stats_.CreateRoundedRect({0, 0, width_, height_}, Style(kPage, kCornerRadius)).value();
        (void)stats_.CreateLabel({kTitleInset, 28}, "Overview", kText, SystemFont::kMedium).value();
        const char* names[4] = {"Working", "Need you", "Done", "Closed"};
        const Color tones[4] = {kWorking, kNeeds, kDone, kClosed};
        const int32_t tile_width = (width_ - 40 * 2 - 10) / 2;
        for (int32_t index = 0; index < 4; ++index) {
            const Rect tile{40 + (index % 2) * (tile_width + 10), 56 + (index / 2) * 64, tile_width, 56};
            (void)stats_.CreateRoundedRect(tile, Style(kCell, 16U)).value();
            (void)stats_.CreateRoundedRect({tile.x + 12, tile.y + 12, 8, 8}, Style(tones[index], 4U)).value();
            (void)stats_.CreateLabel({tile.x + 26, tile.y + 7}, names[index], kSecondary, SystemFont::kSmall).value();
            stat_values_[index] =
                stats_.CreateLabel({tile.x + 12, tile.y + 24}, "0", tones[index], SystemFont::kLarge).value();
        }
        stats_link_ = stats_.CreateLabel({width_ / 2, 184}, " ", kSecondary, SystemFont::kSmall, true).value();
        stats_ble_ = stats_.CreateLabel({width_ / 2, 199}, " ", kSecondary, SystemFont::kSmall, true).value();
        (void)stats_.CreateLabel({width_ / 2, 216}, "Swipe left to go back", kFaint, SystemFont::kSmall, true)
            .value();
    }

    void Fit(const char* text, SystemFont font, int32_t max_width, char* out, uint32_t capacity) const {
        CopyField(out, capacity, NonEmpty(text));
        uint32_t length = static_cast<uint32_t>(strlen(out));
        for (;;) {
            const auto measured = app_.renderer().MeasureText(out, font);
            if (!measured || static_cast<int32_t>(measured->width) <= max_width || length <= 3U) {
                return;
            }
            // Drop one character before the "..", trimming any trailing space.
            const uint32_t keep = (out[length - 1U] == '.' && out[length - 2U] == '.') ? length - 3U : length - 1U;
            uint32_t end = keep;
            while (end > 1U && out[end - 1U] == ' ') {
                --end;
            }
            out[end] = '.';
            out[end + 1U] = '.';
            out[end + 2U] = '\0';
            length = end + 2U;
        }
    }

    int32_t MaxScroll() const {
        const int32_t rows = (static_cast<int32_t>(count_) + kColumns - 1) / kColumns;
        const int32_t content = rows * kCellHeight;
        return content > grid_rect_.height ? content - grid_rect_.height : 0;
    }

    void ApplyScroll() {
        const int32_t limit = MaxScroll();
        scroll_ = scroll_ < 0 ? 0 : (scroll_ > limit ? limit : scroll_);
        grid_.SetTranslation({0, -scroll_});
    }

    void RevealSelected() {
        const int32_t top = static_cast<int32_t>(selected_) / kColumns * kCellHeight;
        if (top < scroll_) {
            scroll_ = top;
        } else if (top + kCellHeight > scroll_ + grid_rect_.height) {
            scroll_ = top + kCellHeight - grid_rect_.height;
        }
    }

    void ShowPage(Page page) {
        page_ = page;
        sheet_.SetVisible(page == Page::kSheet);
        stats_.SetVisible(page == Page::kStats);
        Refresh();
    }

    void Refresh() {
        const auto keyboard = keyboard_.State();
        ble_connected_ = keyboard && keyboard->available && keyboard->connected;
        ++tick_;

        char text[48];
        const auto net = network_.State();
        Color dot = kFaint;
        char address[24] = "";
        if (net && net->station_connected) {
            FormatAddress(net->station_address, address);
        }
        if (!net || !net->available) {
            CopyField(text, sizeof(text), "No Wi-Fi");
            dot = kAlert;
        } else if (!net->station_connected) {
            CopyField(text, sizeof(text), net->station_connecting ? "Wi-Fi..." : "Join Wi-Fi");
            dot = kWorking;
        } else if (client_ != 0U) {
            CopyField(text, sizeof(text), "Mac linked");
            dot = kNeeds;
        } else {
            CopyField(text, sizeof(text), "No Mac");
            dot = kWorking;
        }
        pill_label_.SetText(text);
        pill_dot_.SetFillColor(dot);
        pill_dot_.SetStrokeColor(dot);
        {
            uint32_t length = 0U;
            char line[64] = "";
            Append(line, sizeof(line), length, text);
            if (address[0] != '\0') {
                Append(line, sizeof(line), length, "  ");
                Append(line, sizeof(line), length, address);
            }
            stats_link_.SetText(line);
            stats_ble_.SetText(ble_connected_ ? "Bluetooth keyboard paired" : "Bluetooth: pair \"CGotchi\" on the Mac");
            stats_ble_.SetColor(ble_connected_ ? kNeeds : kWorking);
        }

        uint32_t totals[4] = {0U, 0U, 0U, 0U};  // working, needs, done, closed
        for (uint32_t index = 0U; index < count_; ++index) {
            switch (sessions_[index].state) {
                case State::kWorking: ++totals[0]; break;
                case State::kNeeds: ++totals[1]; break;
                case State::kClosed: ++totals[3]; break;
                case State::kDone:
                case State::kReady: ++totals[2]; break;
            }
        }
        for (uint32_t index = 0U; index < 4U; ++index) {
            uint32_t length = 0U;
            text[0] = '\0';
            AppendNumber(text, sizeof(text), length, totals[index]);
            if (index < 3U) counts_[index].SetText(text);
            stat_values_[index].SetText(text);
        }

        if (selected_ >= count_) {
            selected_ = count_ == 0U ? 0U : count_ - 1U;
        }
        const int32_t look = (tick_ % 2U == 0U) ? -1 : 2;  // working eyes glance left and right
        for (uint32_t index = 0U; index < kMaxSessions; ++index) {
            Cell& cell = cells_[index];
            const bool shown = index < count_;
            cell.ring.SetVisible(shown);
            cell.body.SetVisible(shown);
            cell.eye_left.SetVisible(shown);
            cell.eye_right.SetVisible(shown);
            cell.pupil_left.SetVisible(shown);
            cell.pupil_right.SetVisible(shown);
            cell.name.SetVisible(shown);
            if (!shown) {
                cell.badge.SetVisible(false);
                continue;
            }
            const Session& session = sessions_[index];
            const Color color = StateColor(session.state);
            const bool selected = index == selected_;
            cell.ring.SetFillColor(selected ? Color::Mix(color, kCell, 40U) : kCell);
            cell.ring.SetStrokeColor(selected ? kText : kCell);
            cell.ring.SetStrokeWidth(selected ? 2U : 0U);
            cell.body.SetFillColor(color);
            cell.body.SetStrokeColor(color);
            const int32_t cx = cell.rect.center_x();
            const int32_t top = cell.rect.y + 8;
            const bool sleepy = session.state == State::kDone || session.state == State::kReady ||
                                session.state == State::kClosed;
            const int32_t eye_height = sleepy ? 4 : 11;
            const int32_t eye_y = top + (sleepy ? 16 : 10);
            cell.eye_left.SetRect({cx - 11, eye_y, 9, eye_height});
            cell.eye_right.SetRect({cx + 2, eye_y, 9, eye_height});
            const int32_t offset = session.state == State::kWorking ? look : 0;
            cell.pupil_left.SetVisible(!sleepy);
            cell.pupil_right.SetVisible(!sleepy);
            cell.pupil_left.SetRect({cx - 9 + 1 + offset, top + 14, 4, 5});
            cell.pupil_right.SetRect({cx + 4 + offset, top + 14, 4, 5});
            cell.badge.SetVisible(session.state == State::kNeeds && tick_ % 2U == 0U);
            char name[kNameMax + 1U];
            Fit(session.name, SystemFont::kSmall, cell.rect.width - 4, name, sizeof(name));
            cell.name.SetText(name);
            cell.name.SetColor(selected ? kText : kSecondary);
        }
        ApplyScroll();

        const bool empty = count_ == 0U;
        empty_title_.SetVisible(empty && page_ == Page::kGrid);
        empty_hint_.SetVisible(empty && page_ == Page::kGrid);
        empty_title_.SetText(client_ == 0U ? "Not connected" : "No sessions");
        empty_hint_.SetText(client_ == 0U ? "Start agent_bridge.py on your Mac" : "Start Claude Code to see it here");
        hint_.SetVisible(page_ == Page::kGrid);

        if (count_ != 0U) {
            char title[kNameMax + 1U];
            Fit(sessions_[selected_].name, SystemFont::kMedium, width_ - kTitleInset - 130, title, sizeof(title));
            title_.SetText(title);
        } else {
            title_.SetText("Agents");
        }
        if (count_ != 0U) {
            const Session& session = sessions_[selected_];
            char name[kNameMax + 1U];
            Fit(session.name, SystemFont::kMedium, width_ - kTitleInset * 2 - 16, name, sizeof(name));
            sheet_name_.SetText(name);
            char line[kInfoMax + 24U];
            uint32_t length = 0U;
            line[0] = '\0';
            Append(line, sizeof(line), length, StateText(session.state));
            Append(line, sizeof(line), length, "  \xC2\xB7  ");
            Append(line, sizeof(line), length, NonEmpty(session.info));
            char info[kInfoMax + 24U];
            Fit(line, SystemFont::kSmall, width_ - kTitleInset * 2 - 16, info, sizeof(info));
            sheet_info_.SetText(info);
            sheet_dot_.SetFillColor(StateColor(session.state));
            sheet_dot_.SetStrokeColor(StateColor(session.state));
        } else {
            sheet_name_.SetText("No session");
            sheet_info_.SetText(" ");
        }
        const Color talk = talking_ ? kAlert : (ble_connected_ ? kAccent : kButton);
        talk_box_.SetFillColor(talk);
        talk_box_.SetStrokeColor(talk);
        talk_label_.SetText(talking_ ? "Listening..." : (ble_connected_ ? "Hold to Talk" : "Pair CGotchi"));
        send_label_.SetColor(ble_connected_ ? kText : kSecondary);
        (void)app_.renderer().Present(scene_);
    }

    // ---- Input ----

    // Grid: drag scrolls, tap picks, swipe up opens controls (when the grid
    // cannot scroll further), swipe right opens the overview.
    void HandleTouch(const micropixel::TouchEvent& touch) {
        const auto phase = touch.phase();
        // The finger-up event may not carry a position; use the last one seen.
        const auto point = phase == micropixel::TouchPhase::kUp ? last_point_ : touch.position();
        last_point_ = point;
        if (phase == micropixel::TouchPhase::kDown) {
            touch_start_ = point;
            touch_scroll_ = scroll_;
            moved_ = false;
            if (page_ == Page::kSheet) {
                if (talk_rect_.contains(point)) {
                    BeginTalk();
                } else if (send_rect_.contains(point)) {
                    SendEnter();
                }
            }
            return;
        }
        const int32_t dx = point.x - touch_start_.x;
        const int32_t dy = point.y - touch_start_.y;
        if (dx > kTapSlop || dx < -kTapSlop || dy > kTapSlop || dy < -kTapSlop) {
            moved_ = true;
        }
        if (phase == micropixel::TouchPhase::kMove) {
            if (page_ == Page::kGrid && moved_ && MaxScroll() > 0 && (dy > dx || dy > -dx) == (dy > 0)) {
                scroll_ = touch_scroll_ - dy;
                ApplyScroll();
                (void)app_.renderer().Present(scene_);
            }
            return;
        }
        EndTalk();
        if (phase != micropixel::TouchPhase::kUp) {
            return;
        }
        const bool horizontal = (dx > 0 ? dx : -dx) > (dy > 0 ? dy : -dy);
        if (page_ == Page::kStats) {
            if (horizontal && dx < -kSwipe) ShowPage(Page::kGrid);
            return;
        }
        if (page_ == Page::kSheet) {
            if (!horizontal && dy > kSwipe) ShowPage(Page::kGrid);
            else if (!moved_ && point.y < kSheetTop) ShowPage(Page::kGrid);
            return;
        }
        if (horizontal && dx > kSwipe) {
            ShowPage(Page::kStats);
        } else if (!horizontal && dy < -kSwipe && touch_scroll_ >= MaxScroll()) {
            ShowPage(Page::kSheet);
        } else if (!moved_) {
            const micropixel::Point local{point.x, point.y + scroll_};
            for (uint32_t index = 0U; index < count_; ++index) {
                if (cells_[index].rect.contains(local) && grid_rect_.contains(point)) {
                    Select(index);
                    break;
                }
            }
        }
    }

    void HandleKey(const micropixel::KeyEvent& key) {
        const bool down = key.phase() == micropixel::KeyPhase::kDown;
        const bool repeat = key.phase() == micropixel::KeyPhase::kRepeat;
        if (key.code() == micropixel::KeyCode::kRight && (down || repeat)) {
            if (count_ != 0U) {
                Select(selected_ + 1U < count_ ? selected_ + 1U : 0U);
            }
        } else if (key.code() == micropixel::KeyCode::kLeft && down) {
            SendEnter();
        } else if (key.code() == micropixel::KeyCode::kConfirm && down) {
            BeginTalk();
        } else if (key.code() == micropixel::KeyCode::kConfirm && !repeat) {
            EndTalk();
        }
    }

    void Select(uint32_t index) {
        if (index >= count_) {
            return;
        }
        selected_ = index;
        RevealSelected();
        Sound(agent_buddy_sfx::kPress);
        char line[kIdMax + 4U];
        uint32_t length = 0U;
        line[0] = '\0';
        Append(line, sizeof(line), length, "F\t");
        Append(line, sizeof(line), length, sessions_[index].id);
        Append(line, sizeof(line), length, "\n");
        SendLine(line);
        Refresh();
    }

    // Wispr Flow push-to-talk: hold Ctrl+Option while speaking.
    void BeginTalk() {
        if (talking_) {
            return;
        }
        if (!ble_connected_) {
            Sound(agent_buddy_sfx::kFail);
            return;
        }
        Sound(agent_buddy_sfx::kPress);
        talking_ = true;
        (void)keyboard_.Key(HidKeyUsage::kLeftControl, true);
        (void)keyboard_.Key(HidKeyUsage::kLeftAlt, true);
        Refresh();
    }

    void EndTalk() {
        if (!talking_) {
            return;
        }
        (void)keyboard_.Key(HidKeyUsage::kLeftAlt, false);
        (void)keyboard_.Key(HidKeyUsage::kLeftControl, false);
        talking_ = false;
        Refresh();
    }

    void SendEnter() {
        if (!ble_connected_) {
            Sound(agent_buddy_sfx::kFail);
            return;
        }
        Sound(agent_buddy_sfx::kPress);
        (void)keyboard_.Key(HidKeyUsage::kEnter, true);
        (void)keyboard_.Key(HidKeyUsage::kEnter, false);
    }

    void Sound(std::span<const micropixel::ToneSpec> effect) { (void)tones_.Play(effect); }

    // ---- Network ----

    void StartServer() {
        if (listener_ != 0U) {
            return;
        }
        const auto net = network_.State();
        if (!net || !net->station_connected) {
            return;
        }
        auto listener = network_.Listen(kPort);
        if (!listener) {
            app_.log().Error("cannot listen on port 7333");
            return;
        }
        listener_ = *listener;
        // Lets agent_bridge.py find the Gotchi without typing its address.
        (void)network_.Advertise("_cgotchi-agents", "CGotchi Agents", kPort, nullptr, 0U);
        app_.log().Info("Agent Buddy listening on port 7333");
    }

    void OnSocketReady(uint32_t socket) {
        if (socket == listener_ && listener_ != 0U) {
            for (;;) {
                auto accepted = network_.Accept(listener_);
                if (!accepted) {
                    break;
                }
                // The newest bridge wins; one Mac drives the list.
                if (client_ != 0U) {
                    (void)network_.Close(client_);
                }
                client_ = accepted->socket;
                line_length_ = 0U;
            }
            Refresh();
        } else if (socket == client_ && client_ != 0U) {
            ReadClient();
        }
    }

    void ReadClient() {
        uint8_t buffer[512];
        for (;;) {
            auto received = network_.Receive(client_, buffer, sizeof(buffer));
            if (!received) {
                if (received.error().code() == micropixel::ErrorCode::kWouldBlock) {
                    return;
                }
                DropClient();
                return;
            }
            if (received->closed) {
                DropClient();
                return;
            }
            for (uint32_t index = 0U; index < received->length; ++index) {
                const char c = static_cast<char>(buffer[index]);
                if (c == '\n') {
                    line_[line_length_] = '\0';
                    OnLine(line_);
                    line_length_ = 0U;
                } else if (line_length_ + 1U < sizeof(line_)) {
                    line_[line_length_++] = c;
                }
            }
        }
    }

    void DropClient() {
        (void)network_.Close(client_);
        client_ = 0U;
        line_length_ = 0U;
        Refresh();
    }

    void SendLine(const char* line) {
        if (client_ != 0U) {
            (void)network_.Send(client_, line, static_cast<uint32_t>(strlen(line)));
        }
    }

    // Splits a tab separated line in place.
    static uint32_t Split(char* line, std::array<char*, 5>& fields) {
        uint32_t count = 0U;
        fields[count++] = line;
        for (char* cursor = line; *cursor != '\0' && count < fields.size(); ++cursor) {
            if (*cursor == '\t') {
                *cursor = '\0';
                fields[count++] = cursor + 1;
            }
        }
        return count;
    }

    void OnLine(char* line) {
        if (line_length_ > 0U && line[line_length_ - 1U] == '\r') {
            line[line_length_ - 1U] = '\0';
        }
        if (Same(line, "END")) {
            Commit();
            return;
        }
        std::array<char*, 5> fields{};
        if (Split(line, fields) == 5U && Same(fields[0], "R") && pending_count_ < kMaxSessions) {
            Session& session = pending_[pending_count_++];
            CopyField(session.id, sizeof(session.id), fields[1]);
            session.state = ParseState(fields[2]);
            CopyField(session.name, sizeof(session.name), fields[3]);
            CopyField(session.info, sizeof(session.info), fields[4]);
        }
    }

    // Swaps in the new list, keeps the selection on the same session and
    // beeps when a session has just finished or started needing you.
    void Commit() {
        char selected_id[kIdMax + 1U]{};
        if (selected_ < count_) {
            CopyField(selected_id, sizeof(selected_id), sessions_[selected_].id);
        }
        bool needs = false;
        bool done = false;
        for (uint32_t index = 0U; index < pending_count_; ++index) {
            const Session& next = pending_[index];
            const Session* before = Find(next.id);
            const bool changed = before == nullptr || before->state != next.state;
            if (changed && synced_ && next.state == State::kNeeds) {
                needs = true;
            } else if (changed && synced_ && next.state == State::kDone && before != nullptr &&
                       before->state == State::kWorking) {
                done = true;
            }
        }
        sessions_ = pending_;
        count_ = pending_count_;
        pending_count_ = 0U;
        synced_ = true;
        for (uint32_t index = 0U; index < count_; ++index) {
            if (Same(sessions_[index].id, selected_id)) {
                selected_ = index;
            }
        }
        if (needs) {
            Sound(agent_buddy_sfx::kAsk);
        } else if (done) {
            Sound(agent_buddy_sfx::kDone);
        }
        Refresh();
    }

    const Session* Find(const char* id) const {
        for (uint32_t index = 0U; index < count_; ++index) {
            if (Same(sessions_[index].id, id)) {
                return &sessions_[index];
            }
        }
        return nullptr;
    }

    struct Cell final {
        Rect rect{};
        micropixel::RoundedRectNode ring;
        micropixel::RoundedRectNode body;
        micropixel::RoundedRectNode eye_left;
        micropixel::RoundedRectNode eye_right;
        micropixel::RoundedRectNode pupil_left;
        micropixel::RoundedRectNode pupil_right;
        micropixel::RoundedRectNode badge;
        micropixel::LabelNode name;
    };

    micropixel::Application& app_;
    micropixel::HidKeyboard keyboard_;
    micropixel::Network network_;
    micropixel::ToneSequencer<2U> tones_;
    micropixel::RendererInfo info_;
    micropixel::Scene scene_;
    micropixel::Timer poll_;
    Rect pill_rect_{};
    micropixel::RoundedRectNode pill_dot_;
    micropixel::LabelNode pill_label_;
    micropixel::LabelNode title_;
    std::array<micropixel::LabelNode, 3> counts_{};
    Rect grid_rect_{};
    micropixel::ContainerNode grid_;
    std::array<Cell, kMaxSessions> cells_{};
    micropixel::LabelNode empty_title_;
    micropixel::LabelNode empty_hint_;
    micropixel::LabelNode hint_;
    micropixel::ContainerNode sheet_;
    micropixel::RoundedRectNode sheet_dot_;
    micropixel::LabelNode sheet_name_;
    micropixel::LabelNode sheet_info_;
    micropixel::RoundedRectNode talk_box_;
    micropixel::LabelNode talk_label_;
    micropixel::LabelNode send_label_;
    Rect talk_rect_{};
    Rect send_rect_{};
    micropixel::ContainerNode stats_;
    std::array<micropixel::LabelNode, 4> stat_values_{};
    micropixel::LabelNode stats_link_;
    micropixel::LabelNode stats_ble_;
    int32_t width_{};
    int32_t height_{};
    Page page_{Page::kGrid};
    int32_t scroll_{};
    micropixel::Point touch_start_{};
    micropixel::Point last_point_{};
    int32_t touch_scroll_{};
    bool moved_{};
    uint32_t tick_{};

    std::array<Session, kMaxSessions> sessions_{};
    std::array<Session, kMaxSessions> pending_{};
    uint32_t count_{};
    uint32_t pending_count_{};
    uint32_t selected_{};
    bool synced_{};

    uint32_t listener_{};
    uint32_t client_{};
    char line_[kLineMax + 1U]{};
    uint32_t line_length_{};

    bool ble_connected_{};
    bool talking_{};
};

}  // namespace

int main() {
    micropixel::Application app;
    AgentBuddyApp buddy(app);
    app.log().Info("Agent Buddy ready");
    app.Run([&buddy](const micropixel::Event& event) { return buddy.Handle(event); });
    return 0;
}
