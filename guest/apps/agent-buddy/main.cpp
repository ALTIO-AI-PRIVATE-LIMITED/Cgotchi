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
// Controls: tap a row or VOL+ to pick the next session; hold TALK or BOOT to
// speak; tap SEND or VOL- to press Enter.
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
constexpr uint32_t kVisibleRows = 4U;
constexpr uint32_t kIdMax = 40U;
constexpr uint32_t kNameMax = 18U;
constexpr uint32_t kInfoMax = 30U;
constexpr uint32_t kLineMax = 160U;

constexpr Color kBackground = Color::Rgb(14U, 16U, 22U);
constexpr Color kRow = Color::Rgb(30U, 34U, 44U);
constexpr Color kRowSelected = Color::Rgb(52U, 60U, 82U);
constexpr Color kText = Color::Rgb(238U, 240U, 244U);
constexpr Color kMuted = Color::Rgb(150U, 158U, 172U);
constexpr Color kWorking = Color::Rgb(64U, 140U, 255U);
constexpr Color kNeeds = Color::Rgb(250U, 166U, 26U);
constexpr Color kDone = Color::Rgb(72U, 200U, 120U);
constexpr Color kClosed = Color::Rgb(90U, 96U, 110U);
constexpr Color kTalk = Color::Rgb(64U, 120U, 255U);
constexpr Color kTalkActive = Color::Rgb(230U, 60U, 90U);
constexpr Color kSend = Color::Rgb(37U, 185U, 76U);

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

    void BuildUi() {
        width_ = static_cast<int32_t>(info_.width());
        height_ = static_cast<int32_t>(info_.height());
        const int32_t margin = 6;
        header_ = scene_.CreateLabel({margin, 4}, "Claude Sessions", kText, SystemFont::kMedium).value();
        link_ = scene_.CreateLabel({width_ - 112, 6}, "", kMuted, SystemFont::kSmall).value();

        const int32_t bar_height = 50;
        const int32_t list_top = 28;
        const int32_t list_bottom = height_ - bar_height - margin * 2;
        row_height_ = (list_bottom - list_top) / static_cast<int32_t>(kVisibleRows);
        for (uint32_t index = 0U; index < kVisibleRows; ++index) {
            Row& row = rows_[index];
            row.rect = {margin, list_top + static_cast<int32_t>(index) * row_height_, width_ - margin * 2,
                        row_height_ - 4};
            row.box = scene_.CreateRoundedRect(row.rect, Style(kRow, 10U)).value();
            row.dot = scene_.CreateRoundedRect({row.rect.x + 8, row.rect.y + row.rect.height / 2 - 6, 12, 12},
                                               Style(kClosed, 6U))
                          .value();
            row.name = scene_.CreateLabel({row.rect.x + 28, row.rect.y + 3}, "", kText, SystemFont::kMedium).value();
            row.info = scene_.CreateLabel({row.rect.x + 28, row.rect.y + row.rect.height / 2 + 2}, "", kMuted,
                                          SystemFont::kSmall)
                           .value();
            row.state = scene_.CreateLabel({row.rect.x + row.rect.width - 80, row.rect.y + 4}, "", kText,
                                           SystemFont::kSmall)
                            .value();
        }
        empty_ = scene_.CreateLabel({width_ / 2, list_top + row_height_}, "", kMuted, SystemFont::kMedium, true)
                     .value();

        const int32_t bar_top = height_ - bar_height - margin;
        const int32_t button_width = (width_ - margin * 3) / 2;
        talk_rect_ = {margin, bar_top, button_width, bar_height};
        send_rect_ = {margin * 2 + button_width, bar_top, button_width, bar_height};
        talk_box_ = scene_.CreateRoundedRect(talk_rect_, Style(kTalk, 16U)).value();
        (void)scene_.CreateRoundedRect(send_rect_, Style(kSend, 16U)).value();
        talk_label_ = scene_
                          .CreateLabel({talk_rect_.center_x(), talk_rect_.y + bar_height / 2 - 10}, "HOLD TO TALK",
                                       Color::White(), SystemFont::kMedium, true)
                          .value();
        (void)scene_
            .CreateLabel({send_rect_.center_x(), send_rect_.y + bar_height / 2 - 10}, "SEND", Color::White(),
                         SystemFont::kMedium, true)
            .value();
    }

    void Refresh() {
        const auto keyboard = keyboard_.State();
        ble_connected_ = keyboard && keyboard->available && keyboard->connected;

        char text[48];
        const auto net = network_.State();
        if (!net || !net->available) {
            CopyField(text, sizeof(text), "No Wi-Fi");
        } else if (!net->station_connected) {
            CopyField(text, sizeof(text), net->station_connecting ? "Wi-Fi..." : "Join Wi-Fi");
        } else {
            char address[24];
            FormatAddress(net->station_address, address);
            CopyField(text, sizeof(text), address);
        }
        link_.SetText(text);
        link_.SetColor(client_ != 0U ? kDone : kNeeds);

        uint32_t working = 0U;
        uint32_t needs = 0U;
        for (uint32_t index = 0U; index < count_; ++index) {
            working += sessions_[index].state == State::kWorking ? 1U : 0U;
            needs += sessions_[index].state == State::kNeeds ? 1U : 0U;
        }
        if (count_ == 0U) {
            CopyField(text, sizeof(text), "Claude Sessions");
        } else {
            uint32_t length = 0U;
            text[0] = '\0';
            AppendNumber(text, sizeof(text), length, working);
            Append(text, sizeof(text), length, " run  ");
            AppendNumber(text, sizeof(text), length, needs);
            Append(text, sizeof(text), length, " need you");
        }
        header_.SetText(text);
        header_.SetColor(needs != 0U ? kNeeds : kText);

        if (selected_ >= count_) {
            selected_ = count_ == 0U ? 0U : count_ - 1U;
        }
        if (selected_ < first_) {
            first_ = selected_;
        } else if (selected_ >= first_ + kVisibleRows) {
            first_ = selected_ - kVisibleRows + 1U;
        }
        if (first_ + kVisibleRows > count_) {
            first_ = count_ > kVisibleRows ? count_ - kVisibleRows : 0U;
        }
        for (uint32_t index = 0U; index < kVisibleRows; ++index) {
            Row& row = rows_[index];
            const uint32_t session_index = first_ + index;
            const bool shown = session_index < count_;
            row.box.SetVisible(shown);
            row.dot.SetVisible(shown);
            row.name.SetVisible(shown);
            row.info.SetVisible(shown);
            row.state.SetVisible(shown);
            if (!shown) {
                continue;
            }
            const Session& session = sessions_[session_index];
            const bool selected = session_index == selected_;
            row.box.SetFillColor(selected ? kRowSelected : kRow);
            row.box.SetStrokeColor(selected ? Color::White() : kRow);
            row.box.SetStrokeWidth(selected ? 2U : 0U);
            row.dot.SetFillColor(StateColor(session.state));
            row.dot.SetStrokeColor(StateColor(session.state));
            row.name.SetText(session.name);
            row.info.SetText(session.info);
            row.state.SetText(StateText(session.state));
            row.state.SetColor(StateColor(session.state));
        }

        if (client_ == 0U) {
            empty_.SetText("Start agent_bridge.py on Mac");
        } else if (count_ == 0U) {
            empty_.SetText("No Claude sessions running");
        } else {
            empty_.SetText("");
        }
        talk_box_.SetFillColor(talking_ ? kTalkActive : kTalk);
        talk_label_.SetText(talking_ ? "LISTENING..." : (ble_connected_ ? "HOLD TO TALK" : "PAIR CGotchi"));
        (void)app_.renderer().Present(scene_);
    }

    // ---- Input ----

    void HandleTouch(const micropixel::TouchEvent& touch) {
        const auto point = touch.position();
        if (touch.phase() == micropixel::TouchPhase::kDown) {
            if (talk_rect_.contains(point)) {
                BeginTalk();
                return;
            }
            if (send_rect_.contains(point)) {
                SendEnter();
                return;
            }
            for (uint32_t index = 0U; index < kVisibleRows; ++index) {
                if (first_ + index < count_ && rows_[index].rect.contains(point)) {
                    Select(first_ + index);
                    return;
                }
            }
        } else if (touch.phase() == micropixel::TouchPhase::kUp ||
                   touch.phase() == micropixel::TouchPhase::kCancel) {
            EndTalk();
        }
    }

    void HandleKey(const micropixel::KeyEvent& key) {
        const bool down = key.phase() == micropixel::KeyPhase::kDown;
        const bool repeat = key.phase() == micropixel::KeyPhase::kRepeat;
        if (key.code() == micropixel::KeyCode::kRight && down) {
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

    struct Row final {
        Rect rect{};
        micropixel::RoundedRectNode box;
        micropixel::RoundedRectNode dot;
        micropixel::LabelNode name;
        micropixel::LabelNode info;
        micropixel::LabelNode state;
    };

    micropixel::Application& app_;
    micropixel::HidKeyboard keyboard_;
    micropixel::Network network_;
    micropixel::ToneSequencer<2U> tones_;
    micropixel::RendererInfo info_;
    micropixel::Scene scene_;
    micropixel::Timer poll_;
    micropixel::LabelNode header_;
    micropixel::LabelNode link_;
    micropixel::LabelNode empty_;
    micropixel::RoundedRectNode talk_box_;
    micropixel::LabelNode talk_label_;
    std::array<Row, kVisibleRows> rows_{};
    Rect talk_rect_{};
    Rect send_rect_{};
    int32_t width_{};
    int32_t height_{};
    int32_t row_height_{};

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
