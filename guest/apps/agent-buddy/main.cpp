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
constexpr uint32_t kVisibleRows = 3U;
constexpr uint32_t kIdMax = 40U;
constexpr uint32_t kNameMax = 40U;
constexpr uint32_t kInfoMax = 40U;
constexpr uint32_t kLineMax = 200U;

// Layout for the 296x240 glass with 60 px corners.
constexpr uint32_t kCornerRadius = 60U;
constexpr int32_t kTitleInset = 26;
constexpr int32_t kSideMargin = 12;
constexpr int32_t kPillWidth = 132;
constexpr int32_t kListTop = 60;
constexpr int32_t kCardHeight = 40;
constexpr int32_t kCardPitch = 45;
constexpr int32_t kBadgeWidth = 66;
constexpr int32_t kButtonInset = 34;
constexpr int32_t kButtonHeight = 30;
constexpr int32_t kDragSlop = 10;

// Dark palette in the spirit of iOS system colours.
constexpr Color kBackground = Color::Rgb(4U, 4U, 6U);  // outside the corner curve
constexpr Color kPage = Color::Rgb(16U, 16U, 20U);
constexpr Color kCard = Color::Rgb(30U, 30U, 36U);
constexpr Color kCardSelected = Color::Rgb(42U, 44U, 54U);
constexpr Color kPill = Color::Rgb(38U, 38U, 44U);
constexpr Color kText = Color::Rgb(245U, 245U, 247U);
constexpr Color kSecondary = Color::Rgb(142U, 142U, 147U);
constexpr Color kAccent = Color::Rgb(10U, 132U, 255U);
constexpr Color kWorking = Color::Rgb(10U, 132U, 255U);
constexpr Color kNeeds = Color::Rgb(255U, 159U, 10U);
constexpr Color kDone = Color::Rgb(48U, 209U, 88U);
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
    //
    // The glass has 60 px rounded corners: the page is a 60 px rounded card
    // on near-black, titles start 26 px in, the status pill sits top centre
    // and the buttons sit bottom centre, all clear of the curve.

    void BuildUi() {
        width_ = static_cast<int32_t>(info_.width());
        height_ = static_cast<int32_t>(info_.height());
        (void)scene_.CreateRoundedRect({0, 0, width_, height_}, Style(kPage, kCornerRadius)).value();

        // Status pill, top centre.
        pill_rect_ = {width_ / 2 - kPillWidth / 2, 7, kPillWidth, 20};
        (void)scene_.CreateRoundedRect(pill_rect_, Style(kPill, 10U)).value();
        pill_dot_ = scene_.CreateRoundedRect({pill_rect_.x + 9, pill_rect_.y + 7, 6, 6}, Style(kNeeds, 3U)).value();
        pill_label_ = scene_
                          .CreateLabel({pill_rect_.center_x() + 5, pill_rect_.y + 5}, " ", kSecondary, SystemFont::kSmall,
                                       true)
                          .value();

        // Large title, 26 px from the edge, with the Gotchi's address on the right.
        (void)scene_.CreateLabel({kTitleInset, 34}, "Sessions", kText, SystemFont::kTitle).value();
        address_ = scene_.CreateLabel({width_ - kTitleInset - 70, 40}, " ", kSecondary, SystemFont::kSmall).value();

        // Session cards.
        for (uint32_t index = 0U; index < kVisibleRows; ++index) {
            Card& card = cards_[index];
            card.rect = {kSideMargin, kListTop + static_cast<int32_t>(index) * kCardPitch, width_ - kSideMargin * 2,
                         kCardHeight};
            card.box = scene_.CreateRoundedRect(card.rect, Style(kCard, 12U)).value();
            card.dot = scene_.CreateRoundedRect({card.rect.x + 11, card.rect.y + 16, 8, 8}, Style(kClosed, 4U)).value();
            card.name = scene_.CreateLabel({card.rect.x + 27, card.rect.y + 6}, " ", kText, SystemFont::kLarge).value();
            card.info =
                scene_.CreateLabel({card.rect.x + 27, card.rect.y + 24}, " ", kSecondary, SystemFont::kSmall).value();
            const Rect badge{card.rect.x + card.rect.width - kBadgeWidth - 8, card.rect.y + 11, kBadgeWidth, 18};
            card.badge = scene_.CreateRoundedRect(badge, Style(kCard, 9U)).value();
            card.state = scene_
                             .CreateLabel({badge.center_x(), badge.y + 4}, " ", kText, SystemFont::kSmall, true)
                             .value();
        }

        // Thin scroll bar to the right of the cards.
        const int32_t list_height = kCardPitch * static_cast<int32_t>(kVisibleRows) - (kCardPitch - kCardHeight);
        track_rect_ = {width_ - kSideMargin + 3, kListTop + 4, 3, list_height - 8};
        track_ = scene_.CreateRoundedRect(track_rect_, Style(kPill, 1U)).value();
        thumb_ = scene_.CreateRoundedRect(track_rect_, Style(kClosed, 1U)).value();

        // Empty state, in the middle of the list area.
        empty_title_ = scene_
                           .CreateLabel({width_ / 2, kListTop + 36}, "No sessions yet", kText, SystemFont::kLarge, true)
                           .value();
        empty_hint_ = scene_
                          .CreateLabel({width_ / 2, kListTop + 58}, " ", kSecondary, SystemFont::kSmall, true)
                          .value();

        // Buttons, bottom centre, clear of the corner curve.
        const int32_t button_top = height_ - kButtonHeight - 9;
        const int32_t button_width = (width_ - kButtonInset * 2 - 8) / 2;
        talk_rect_ = {kButtonInset, button_top, button_width, kButtonHeight};
        send_rect_ = {kButtonInset + button_width + 8, button_top, button_width, kButtonHeight};
        talk_box_ = scene_.CreateRoundedRect(talk_rect_, Style(kAccent, kButtonHeight / 2U)).value();
        (void)scene_.CreateRoundedRect(send_rect_, Style(kCard, kButtonHeight / 2U)).value();
        talk_label_ = scene_
                          .CreateLabel({talk_rect_.center_x(), talk_rect_.y + 8}, "Hold to talk", Color::White(),
                                       SystemFont::kMedium, true)
                          .value();
        (void)scene_
            .CreateLabel({send_rect_.center_x(), send_rect_.y + 8}, "Send", kAccent, SystemFont::kMedium, true)
            .value();
    }

    // Cuts `text` with ".." so it fits `max_width` pixels in `font`.
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

    void Refresh() {
        const auto keyboard = keyboard_.State();
        ble_connected_ = keyboard && keyboard->available && keyboard->connected;

        char text[48];
        const auto net = network_.State();
        if (!net || !net->available) {
            CopyField(text, sizeof(text), "No Wi-Fi");
        } else if (!net->station_connected) {
            CopyField(text, sizeof(text), net->station_connecting ? "Joining Wi-Fi" : "Join Wi-Fi");
        } else {
            char address[24];
            FormatAddress(net->station_address, address);
            CopyField(text, sizeof(text), address);
        }
        address_.SetText(text);
        if (const auto measured = app_.renderer().MeasureText(text, SystemFont::kSmall)) {
            address_.SetPosition({width_ - kTitleInset - static_cast<int32_t>(measured->width), 40});
        }

        uint32_t working = 0U;
        uint32_t needs = 0U;
        for (uint32_t index = 0U; index < count_; ++index) {
            working += sessions_[index].state == State::kWorking ? 1U : 0U;
            needs += sessions_[index].state == State::kNeeds ? 1U : 0U;
        }
        uint32_t length = 0U;
        text[0] = '\0';
        if (client_ == 0U) {
            Append(text, sizeof(text), length, "Mac not connected");
        } else if (needs != 0U) {
            AppendNumber(text, sizeof(text), length, needs);
            Append(text, sizeof(text), length, " need you");
        } else if (working != 0U) {
            AppendNumber(text, sizeof(text), length, working);
            Append(text, sizeof(text), length, " working");
        } else {
            Append(text, sizeof(text), length, count_ == 0U ? "Mac connected" : "All done");
        }
        pill_label_.SetText(text);
        pill_label_.SetColor(needs != 0U ? kNeeds : kSecondary);
        pill_dot_.SetFillColor(client_ == 0U ? kClosed : (needs != 0U ? kNeeds : (working != 0U ? kWorking : kDone)));

        if (selected_ >= count_) {
            selected_ = count_ == 0U ? 0U : count_ - 1U;
        }
        if (follow_selection_) {
            if (selected_ < first_) {
                first_ = selected_;
            } else if (selected_ >= first_ + kVisibleRows) {
                first_ = selected_ - kVisibleRows + 1U;
            }
            follow_selection_ = false;
        }
        const uint32_t max_first = count_ > kVisibleRows ? count_ - kVisibleRows : 0U;
        if (first_ > max_first) {
            first_ = max_first;
        }

        for (uint32_t index = 0U; index < kVisibleRows; ++index) {
            Card& card = cards_[index];
            const uint32_t session_index = first_ + index;
            const bool shown = session_index < count_;
            card.box.SetVisible(shown);
            card.dot.SetVisible(shown);
            card.name.SetVisible(shown);
            card.info.SetVisible(shown);
            card.badge.SetVisible(shown);
            card.state.SetVisible(shown);
            if (!shown) {
                continue;
            }
            const Session& session = sessions_[session_index];
            const bool selected = session_index == selected_;
            const Color color = StateColor(session.state);
            card.box.SetFillColor(selected ? kCardSelected : kCard);
            card.box.SetStrokeColor(selected ? kAccent : kCard);
            card.box.SetStrokeWidth(selected ? 2U : 0U);
            card.dot.SetFillColor(color);
            card.dot.SetStrokeColor(color);
            char fitted[kNameMax + 4U];
            Fit(session.name, SystemFont::kLarge, card.rect.width - 27 - kBadgeWidth - 14, fitted, sizeof(fitted));
            card.name.SetText(fitted);
            Fit(session.info, SystemFont::kSmall, card.rect.width - 27 - kBadgeWidth - 14, fitted, sizeof(fitted));
            card.info.SetText(fitted);
            const Color tint = Color::Mix(color, selected ? kCardSelected : kCard, 60U);
            card.badge.SetFillColor(tint);
            card.badge.SetStrokeColor(tint);
            card.state.SetText(StateText(session.state));
            card.state.SetColor(color);
        }

        const bool scrollable = count_ > kVisibleRows;
        track_.SetVisible(scrollable);
        thumb_.SetVisible(scrollable);
        if (scrollable) {
            const int32_t thumb_height = track_rect_.height * static_cast<int32_t>(kVisibleRows) /
                                         static_cast<int32_t>(count_);
            const int32_t travel = track_rect_.height - thumb_height;
            const int32_t offset = travel * static_cast<int32_t>(first_) / static_cast<int32_t>(max_first);
            thumb_.SetRect({track_rect_.x, track_rect_.y + offset, track_rect_.width, thumb_height});
        }

        const bool empty = client_ == 0U || count_ == 0U;
        empty_title_.SetVisible(empty);
        empty_hint_.SetVisible(empty);
        empty_title_.SetText(client_ == 0U ? "Waiting for your Mac" : "No sessions yet");
        empty_hint_.SetText(client_ == 0U ? "Start agent_bridge.py on the Mac" : "Start claude on your Mac");

        talk_box_.SetFillColor(talking_ ? kNeeds : (ble_connected_ ? kAccent : kClosed));
        talk_label_.SetText(talking_ ? "Listening..." : (ble_connected_ ? "Hold to talk" : "Pair CGotchi"));
        (void)app_.renderer().Present(scene_);
    }

    // ---- Input ----
    //
    // Swipe the list up or down to scroll, tap a card to pick it. VOL+ picks
    // the next session, VOL- sends Enter, BOOT is push to talk.

    void HandleTouch(const micropixel::TouchEvent& touch) {
        const auto point = touch.position();
        switch (touch.phase()) {
            case micropixel::TouchPhase::kDown:
                if (talk_rect_.contains(point)) {
                    BeginTalk();
                } else if (send_rect_.contains(point)) {
                    SendEnter();
                } else if (point.y >= kListTop - 6 && point.y < talk_rect_.y - 4) {
                    in_list_ = true;
                    dragged_ = false;
                    touch_start_y_ = point.y;
                    drag_first_ = first_;
                }
                break;
            case micropixel::TouchPhase::kMove:
                if (in_list_) {
                    const int32_t dy = point.y - touch_start_y_;
                    if (dy > kDragSlop || dy < -kDragSlop) {
                        dragged_ = true;
                    }
                    if (dragged_) {
                        // Finger up moves the list up, like on a phone.
                        const int32_t rows = -dy / kCardPitch;
                        int32_t first = static_cast<int32_t>(drag_first_) + rows;
                        const int32_t max_first = count_ > kVisibleRows ? static_cast<int32_t>(count_ - kVisibleRows) : 0;
                        first = first < 0 ? 0 : (first > max_first ? max_first : first);
                        if (static_cast<uint32_t>(first) != first_) {
                            first_ = static_cast<uint32_t>(first);
                            Refresh();
                        }
                    }
                }
                break;
            case micropixel::TouchPhase::kUp:
                if (in_list_ && !dragged_) {
                    for (uint32_t index = 0U; index < kVisibleRows; ++index) {
                        if (first_ + index < count_ && cards_[index].rect.contains(point)) {
                            Select(first_ + index, false);
                            break;
                        }
                    }
                }
                in_list_ = false;
                EndTalk();
                break;
            case micropixel::TouchPhase::kCancel:
                in_list_ = false;
                EndTalk();
                break;
        }
    }

    void HandleKey(const micropixel::KeyEvent& key) {
        const bool down = key.phase() == micropixel::KeyPhase::kDown;
        const bool repeat = key.phase() == micropixel::KeyPhase::kRepeat;
        if (key.code() == micropixel::KeyCode::kRight && (down || repeat)) {
            if (count_ != 0U) {
                Select(selected_ + 1U < count_ ? selected_ + 1U : 0U, true);
            }
        } else if (key.code() == micropixel::KeyCode::kLeft && down) {
            SendEnter();
        } else if (key.code() == micropixel::KeyCode::kConfirm && down) {
            BeginTalk();
        } else if (key.code() == micropixel::KeyCode::kConfirm && !repeat) {
            EndTalk();
        }
    }

    void Select(uint32_t index, bool follow) {
        if (index >= count_) {
            return;
        }
        selected_ = index;
        follow_selection_ = follow;
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

    struct Card final {
        Rect rect{};
        micropixel::RoundedRectNode box;
        micropixel::RoundedRectNode dot;
        micropixel::LabelNode name;
        micropixel::LabelNode info;
        micropixel::RoundedRectNode badge;
        micropixel::LabelNode state;
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
    micropixel::LabelNode address_;
    std::array<Card, kVisibleRows> cards_{};
    Rect track_rect_{};
    micropixel::RoundedRectNode track_;
    micropixel::RoundedRectNode thumb_;
    micropixel::LabelNode empty_title_;
    micropixel::LabelNode empty_hint_;
    micropixel::RoundedRectNode talk_box_;
    micropixel::LabelNode talk_label_;
    Rect talk_rect_{};
    Rect send_rect_{};
    int32_t width_{};
    int32_t height_{};

    bool in_list_{};
    bool dragged_{};
    int32_t touch_start_y_{};
    uint32_t drag_first_{};
    bool follow_selection_{};

    std::array<Session, kMaxSessions> sessions_{};
    std::array<Session, kMaxSessions> pending_{};
    uint32_t count_{};
    uint32_t pending_count_{};
    uint32_t selected_{};
    uint32_t first_{};
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
