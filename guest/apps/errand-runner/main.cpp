// SPDX-License-Identifier: Apache-2.0
//
// Errand Runner: the CGotchi is the remote for rides and grocery orders that
// a Mac places on your iPhone (phone-harness through iPhone Mirroring).
//
// 1. Pick an errand ("Ride to Office", "Blinkit: milk") or say one with
//    Wispr Flow (hold BOOT, the "CGotchi" Bluetooth keyboard holds
//    Ctrl+Option; VOL- presses Enter).
// 2. The Mac reads fares or the cart total WITHOUT booking and sends quotes.
// 3. Pick a quote and HOLD BOOT for 2 seconds. Only then the Mac books it.
//    Anything else (VOL-, Cancel, letting go early) cancels.
//
// errand_bridge.py on the Mac connects over Wi-Fi to TCP port 7334.
// Mac -> Gotchi, one line each, tab separated:
//   M\t<id>\t<label>                 errand menu row; MEND ends the menu
//   S\t<text>                        progress ("Checking Uber...")
//   O\t<id>\t<app>\t<price>\t<detail>   quote row; QEND\t<summary> ends them
//   L\t<text>                        voice prompt is open on the Mac
//   R\t<ok|fail>\t<text>             final result
// Gotchi -> Mac:
//   Q\t<menu id>   quote this errand      V   open the voice prompt
//   C\t<quote id>  book it (after hold)   X   cancel
#include <stdint.h>
#include <string.h>

#include <array>
#include <span>

#include "errand-runner_sfx_profiles.hpp"
#include "sdk/micropixel.hpp"

namespace {

using micropixel::Color;
using micropixel::HidKeyUsage;
using micropixel::Rect;
using micropixel::SystemFont;

constexpr uint16_t kPort = 7334U;
constexpr uint32_t kMaxRows = 6U;
constexpr uint32_t kIdMax = 24U;
constexpr uint32_t kTextMax = 48U;
constexpr uint32_t kLineMax = 240U;
constexpr uint64_t kHoldUs = 2000000U;  // hold BOOT this long to book

constexpr int32_t kCornerRadius = 60;
constexpr int32_t kInset = 26;
constexpr int32_t kListTop = 50;
constexpr int32_t kRowHeight = 27;

constexpr Color kBackground = Color::Rgb(0U, 0U, 0U);
constexpr Color kPage = Color::Rgb(12U, 12U, 15U);
constexpr Color kRow = Color::Rgb(26U, 26U, 31U);
constexpr Color kPill = Color::Rgb(30U, 30U, 35U);
constexpr Color kText = Color::Rgb(245U, 245U, 247U);
constexpr Color kSecondary = Color::Rgb(142U, 142U, 147U);
constexpr Color kFaint = Color::Rgb(80U, 80U, 86U);
constexpr Color kAccent = Color::Rgb(10U, 132U, 255U);
constexpr Color kGood = Color::Rgb(48U, 209U, 88U);
constexpr Color kWarn = Color::Rgb(255U, 204U, 0U);
constexpr Color kAlert = Color::Rgb(255U, 55U, 95U);

struct Row final {
    char id[kIdMax + 1U]{};
    char title[kTextMax + 1U]{};   // menu label, or quote app
    char price[kIdMax + 1U]{};     // quotes only
    char detail[kTextMax + 1U]{};  // quotes only
};

// The Guest runtime has no strcmp or snprintf.
bool Same(const char* left, const char* right) {
    while (*left != '\0' && *left == *right) {
        ++left;
        ++right;
    }
    return *left == *right;
}

// The Host rejects a scene with an empty label.
const char* NonEmpty(const char* text) { return text[0] == '\0' ? " " : text; }

void Copy(char* out, uint32_t capacity, const char* text) {
    uint32_t length = 0U;
    while (text[length] != '\0' && length + 1U < capacity) {
        out[length] = text[length];
        ++length;
    }
    out[length] = '\0';
}

void Append(char* out, uint32_t capacity, uint32_t& length, const char* text) {
    while (*text != '\0' && length + 1U < capacity) {
        out[length++] = *text++;
    }
    out[length] = '\0';
}

micropixel::RoundedRectStyle Style(Color fill, int32_t radius) {
    micropixel::RoundedRectStyle style{};
    style.fill = fill;
    style.stroke = fill;
    style.radius = static_cast<uint32_t>(radius);
    style.stroke_width = 0U;
    return style;
}

class ErrandApp final {
   public:
    explicit ErrandApp(micropixel::Application& app)
        : app_(app),
          keyboard_(app.hid_keyboard()),
          network_(app.network()),
          tones_(app.audio()),
          info_(app.renderer().info()),
          scene_(app.renderer().CreateScene(kBackground).value()),
          poll_(app.timers().Every(micropixel::Duration::Milliseconds(100U)).value()) {
        BuildUi();
        (void)keyboard_.Start();
        StartServer();
        Refresh();
    }

    micropixel::EventResult Handle(const micropixel::Event& event) {
        if (event.TimerFrom(poll_) != nullptr) {
            // 100 ms tick: drives the hold bar; the rest refreshes twice a second.
            if (holding_) HoldTick();
            if (++tick_ % 5U == 0U) {
                StartServer();
                Refresh();
            }
        } else if (event.type() == micropixel::EventType::kResume) {
            StartServer();
            Refresh();
        } else if (const auto* net = event.network()) {
            if (net->kind() == micropixel::NetworkEventKind::kReady) {
                OnSocketReady(net->socket());
            } else {
                Refresh();
            }
        } else if (const auto* touch = event.touch()) {
            HandleTouch(*touch);
        } else if (const auto* key = event.key()) {
            HandleKey(*key);
        } else if (event.type() == micropixel::EventType::kStop) {
            EndTalk();
            EndHold();
        }
        return micropixel::EventResult::kContinue;
    }

   private:
    enum class Page : uint8_t { kMenu, kVoice, kWorking, kQuotes, kResult };

    // ---- UI ----

    void BuildUi() {
        width_ = static_cast<int32_t>(info_.width());
        height_ = static_cast<int32_t>(info_.height());
        (void)scene_.CreateRoundedRect({0, 0, width_, height_}, Style(kPage, kCornerRadius)).value();

        const Rect pill{width_ / 2 - 46, 6, 92, 18};
        (void)scene_.CreateRoundedRect(pill, Style(kPill, 9)).value();
        pill_dot_ = scene_.CreateRoundedRect({pill.x + 8, pill.y + 6, 6, 6}, Style(kFaint, 3)).value();
        pill_label_ =
            scene_.CreateLabel({pill.center_x() + 5, pill.y + 2}, " ", kSecondary, SystemFont::kSmall, true).value();
        title_ = scene_.CreateLabel({kInset, 28}, "Errands", kText, SystemFont::kMedium).value();

        const int32_t row_width = width_ - kInset * 2 + 8;
        for (uint32_t index = 0U; index < kMaxRows + 1U; ++index) {
            Cell& cell = cells_[index];
            cell.rect = {kInset - 4, kListTop + static_cast<int32_t>(index) * kRowHeight, row_width, kRowHeight - 3};
            cell.box = scene_.CreateRoundedRect(cell.rect, Style(kRow, 10)).value();
            cell.title = scene_.CreateLabel({cell.rect.x + 10, cell.rect.y + 4}, " ", kText, SystemFont::kSmall).value();
            cell.price = scene_.CreateLabel({cell.rect.x + cell.rect.width - 10, cell.rect.y + 4}, " ", kGood,
                                            SystemFont::kSmall)
                             .value();
        }

        big_ = scene_.CreateLabel({width_ / 2, 92}, " ", kText, SystemFont::kMedium, true).value();
        small_ = scene_.CreateLabel({width_ / 2, 118}, " ", kSecondary, SystemFont::kSmall, true).value();

        // Hold-to-book bar on the quotes page.
        bar_rect_ = {kInset + 10, 196, width_ - (kInset + 10) * 2, 22};
        bar_back_ = scene_.CreateRoundedRect(bar_rect_, Style(kPill, 11)).value();
        // The Host rejects zero-size rects; start at the bar's height and hide it.
        bar_fill_ = scene_.CreateRoundedRect({bar_rect_.x, bar_rect_.y, bar_rect_.height, bar_rect_.height}, Style(kAccent, 11))
                        .value();
        hint_ = scene_.CreateLabel({width_ / 2, 200}, " ", kSecondary, SystemFont::kSmall, true).value();
    }

    void Fit(const char* text, int32_t max_width, char* out, uint32_t capacity) const {
        Copy(out, capacity, NonEmpty(text));
        uint32_t length = static_cast<uint32_t>(strlen(out));
        for (;;) {
            const auto measured = app_.renderer().MeasureText(out, SystemFont::kSmall);
            if (!measured || static_cast<int32_t>(measured->width) <= max_width || length <= 3U) {
                return;
            }
            uint32_t end = (out[length - 1U] == '.' && out[length - 2U] == '.') ? length - 3U : length - 1U;
            while (end > 1U && out[end - 1U] == ' ') {
                --end;
            }
            out[end] = '.';
            out[end + 1U] = '.';
            out[end + 2U] = '\0';
            length = end + 2U;
        }
    }

    // Rows on screen: the menu plus "Say it", or the quotes.
    uint32_t RowCount() const {
        if (page_ == Page::kMenu) return menu_count_ + 1U;
        if (page_ == Page::kQuotes) return quote_count_;
        return 0U;
    }

    void ShowPage(Page page) {
        page_ = page;
        selected_ = 0U;
        EndHold();
        Refresh();
    }

    void Refresh() {
        const auto keyboard = keyboard_.State();
        ble_ = keyboard && keyboard->available && keyboard->connected;
        const auto net = network_.State();
        const char* link = "Mac linked";
        Color dot = kGood;
        if (!net || !net->available) {
            link = "No Wi-Fi";
            dot = kAlert;
        } else if (!net->station_connected) {
            link = net->station_connecting ? "Wi-Fi..." : "Join Wi-Fi";
            dot = kWarn;
        } else if (client_ == 0U) {
            link = "No Mac";
            dot = kWarn;
        }
        pill_label_.SetText(link);
        pill_dot_.SetFillColor(dot);
        pill_dot_.SetStrokeColor(dot);

        const uint32_t rows = RowCount();
        if (selected_ >= rows && rows != 0U) selected_ = rows - 1U;
        char text[kTextMax + 1U];
        for (uint32_t index = 0U; index < kMaxRows + 1U; ++index) {
            Cell& cell = cells_[index];
            const bool shown = index < rows;
            cell.box.SetVisible(shown);
            cell.title.SetVisible(shown);
            cell.price.SetVisible(shown && page_ == Page::kQuotes);
            if (!shown) continue;
            const bool picked = index == selected_;
            cell.box.SetFillColor(picked ? Color::Mix(kAccent, kRow, 50U) : kRow);
            cell.box.SetStrokeColor(picked ? kText : kRow);
            cell.box.SetStrokeWidth(picked ? 1U : 0U);
            if (page_ == Page::kMenu) {
                const bool voice = index == menu_count_;
                Fit(voice ? "Say it... (hold BOOT)" : menu_[index].title, cell.rect.width - 20, text, sizeof(text));
                cell.title.SetText(text);
                cell.title.SetColor(voice ? kAccent : kText);
            } else {
                const Row& quote = quotes_[index];
                char line[kTextMax * 2U + 4U];
                uint32_t length = 0U;
                line[0] = '\0';
                Append(line, sizeof(line), length, quote.title);
                if (quote.detail[0] != '\0') {
                    Append(line, sizeof(line), length, "  ");
                    Append(line, sizeof(line), length, quote.detail);
                }
                Fit(line, cell.rect.width - 90, text, sizeof(text));
                cell.title.SetText(text);
                cell.title.SetColor(kText);
                cell.price.SetText(NonEmpty(quote.price));
                const auto measured = app_.renderer().MeasureText(NonEmpty(quote.price), SystemFont::kSmall);
                const int32_t price_width = measured ? static_cast<int32_t>(measured->width) : 40;
                cell.price.SetPosition({cell.rect.x + cell.rect.width - 10 - price_width, cell.rect.y + 4});
            }
        }

        const char* title = "Errands";
        const char* big = " ";
        const char* small = " ";
        const char* hint = " ";
        bool bar = false;
        switch (page_) {
            case Page::kMenu:
                if (client_ == 0U) {
                    big = "Not connected";
                    small = "Run errand_bridge.py on the Mac";
                } else if (menu_count_ == 0U) {
                    big = "Waiting for errands";
                }
                hint = client_ == 0U ? " " : "Tap or VOL+, then BOOT";
                break;
            case Page::kVoice:
                title = "Say it";
                big = talking_ ? "Listening..." : "Hold BOOT and speak";
                small = ble_ ? (voice_note_[0] != '\0' ? voice_note_ : "e.g. \"ride to Koramangala\"")
                             : "Pair \"CGotchi\" Bluetooth first";
                hint = "VOL- send   tap here to cancel";
                break;
            case Page::kWorking:
                title = "Working";
                big = status_[0] != '\0' ? status_ : "Asking the phone...";
                small = (tick_ / 5U % 3U == 0U) ? "." : ((tick_ / 5U % 3U == 1U) ? ".." : "...");
                hint = "Tap or VOL- to cancel";
                break;
            case Page::kQuotes:
                title = summary_[0] != '\0' ? summary_ : "Quotes";
                if (quote_count_ == 0U) big = "No quotes";
                bar = quote_count_ != 0U;
                hint = holding_ ? "Keep holding..." : "HOLD BOOT 2 s to book   VOL- cancel";
                break;
            case Page::kResult:
                title = result_ok_ ? "Done" : "Stopped";
                big = result_[0] != '\0' ? result_ : (result_ok_ ? "Booked" : "Cancelled");
                small = "Tap or press any key";
                break;
        }
        char fitted[kTextMax + 1U];
        Fit(title, width_ - kInset * 2, fitted, sizeof(fitted));
        title_.SetText(fitted);
        Fit(big, width_ - 50, fitted, sizeof(fitted));
        big_.SetText(fitted);
        big_.SetColor(page_ == Page::kResult ? (result_ok_ ? kGood : kAlert) : kText);
        small_.SetText(small);
        hint_.SetText(hint);
        hint_.SetPosition({width_ / 2, bar ? 175 : 205});
        bar_back_.SetVisible(bar);
        UpdateBar();
        (void)app_.renderer().Present(scene_);
    }

    void UpdateBar() {
        uint64_t elapsed = 0U;
        if (holding_) {
            const uint64_t now = app_.clock().Now().microseconds();
            elapsed = now > hold_start_ ? now - hold_start_ : 0U;
            if (elapsed > kHoldUs) elapsed = kHoldUs;
        }
        int32_t width = static_cast<int32_t>(static_cast<uint64_t>(bar_rect_.width) * elapsed / kHoldUs);
        // The Host rejects zero-size rects: keep the fill at least bar-height wide, hidden until it is.
        const bool fill_shown = page_ == Page::kQuotes && quote_count_ != 0U && width >= bar_rect_.height;
        if (width < bar_rect_.height) width = bar_rect_.height;
        bar_fill_.SetRect({bar_rect_.x, bar_rect_.y, width, bar_rect_.height});
        bar_fill_.SetVisible(fill_shown);
        const Color color = holding_ ? kGood : kAccent;
        bar_fill_.SetFillColor(color);
        bar_fill_.SetStrokeColor(color);
    }

    // ---- Input ----

    void HandleTouch(const micropixel::TouchEvent& touch) {
        const auto phase = touch.phase();
        const auto point = phase == micropixel::TouchPhase::kUp ? last_point_ : touch.position();
        last_point_ = point;
        if (phase == micropixel::TouchPhase::kDown) {
            touch_start_ = point;
            if (page_ == Page::kQuotes && bar_rect_.contains(point)) {
                BeginHold();
            }
            return;
        }
        if (phase != micropixel::TouchPhase::kUp && phase != micropixel::TouchPhase::kCancel) {
            return;
        }
        if (holding_) {
            EndHold();  // let go early: nothing is booked
            Refresh();
            return;
        }
        if (phase != micropixel::TouchPhase::kUp) return;
        switch (page_) {
            case Page::kMenu:
            case Page::kQuotes:
                for (uint32_t index = 0U; index < RowCount(); ++index) {
                    if (cells_[index].rect.contains(point)) {
                        selected_ = index;
                        Sound(errand_runner_sfx::kPress);
                        if (page_ == Page::kMenu) Choose();
                        else Refresh();
                        return;
                    }
                }
                break;
            case Page::kVoice:
            case Page::kWorking:
                Cancel();
                break;
            case Page::kResult:
                ShowPage(Page::kMenu);
                break;
        }
    }

    void HandleKey(const micropixel::KeyEvent& key) {
        const auto phase = key.phase();
        const bool down = phase == micropixel::KeyPhase::kDown;
        const bool released = phase == micropixel::KeyPhase::kUp || phase == micropixel::KeyPhase::kCancel;
        const auto code = key.code();
        if (page_ == Page::kResult) {
            if (down) ShowPage(Page::kMenu);
            return;
        }
        if (code == micropixel::KeyCode::kRight && (down || phase == micropixel::KeyPhase::kRepeat)) {
            const uint32_t rows = RowCount();
            if (rows != 0U) {
                selected_ = selected_ + 1U < rows ? selected_ + 1U : 0U;
                Sound(errand_runner_sfx::kPress);
                Refresh();
            }
        } else if (code == micropixel::KeyCode::kLeft && down) {
            if (page_ == Page::kVoice) {
                SendEnter();
            } else if (page_ != Page::kMenu) {
                Cancel();
            }
        } else if (code == micropixel::KeyCode::kConfirm) {
            if (page_ == Page::kMenu && down) {
                Choose();
            } else if (page_ == Page::kVoice) {
                if (down) BeginTalk();
                if (released) EndTalk();
            } else if (page_ == Page::kQuotes) {
                if (down) BeginHold();
                if (released && holding_) {
                    EndHold();
                    Refresh();
                }
            }
        }
    }

    void Choose() {
        if (client_ == 0U) {
            Sound(errand_runner_sfx::kFail);
            return;
        }
        if (selected_ == menu_count_) {
            voice_note_[0] = '\0';
            SendLine("V\n");
            ShowPage(Page::kVoice);
            return;
        }
        char line[kIdMax + 4U];
        uint32_t length = 0U;
        line[0] = '\0';
        Append(line, sizeof(line), length, "Q\t");
        Append(line, sizeof(line), length, menu_[selected_].id);
        Append(line, sizeof(line), length, "\n");
        SendLine(line);
        StartWorking("Asking the phone...");
    }

    void StartWorking(const char* status) {
        Copy(status_, sizeof(status_), status);
        quote_count_ = 0U;
        pending_quotes_ = 0U;
        ShowPage(Page::kWorking);
    }

    void Cancel() {
        EndTalk();
        EndHold();
        SendLine("X\n");
        Sound(errand_runner_sfx::kFail);
        ShowPage(Page::kMenu);
    }

    // Booking needs BOOT (or the bar) held for kHoldUs without letting go.
    void BeginHold() {
        if (holding_ || quote_count_ == 0U || client_ == 0U) return;
        holding_ = true;
        hold_start_ = app_.clock().Now().microseconds();
        Sound(errand_runner_sfx::kPress);
        Refresh();
    }

    void EndHold() { holding_ = false; }

    void HoldTick() {
        const uint64_t now = app_.clock().Now().microseconds();
        if (now - hold_start_ < kHoldUs) {
            UpdateBar();
            (void)app_.renderer().Present(scene_);
            return;
        }
        EndHold();
        char line[kIdMax + 4U];
        uint32_t length = 0U;
        line[0] = '\0';
        Append(line, sizeof(line), length, "C\t");
        Append(line, sizeof(line), length, quotes_[selected_].id);
        Append(line, sizeof(line), length, "\n");
        SendLine(line);
        Sound(errand_runner_sfx::kConfirm);
        StartWorking("Booking...");
    }

    // Wispr Flow push-to-talk: Ctrl+Option held over the Bluetooth keyboard.
    void BeginTalk() {
        if (talking_) return;
        if (!ble_) {
            Sound(errand_runner_sfx::kFail);
            return;
        }
        talking_ = true;
        (void)keyboard_.Key(HidKeyUsage::kLeftControl, true);
        (void)keyboard_.Key(HidKeyUsage::kLeftAlt, true);
        Refresh();
    }

    void EndTalk() {
        if (!talking_) return;
        (void)keyboard_.Key(HidKeyUsage::kLeftAlt, false);
        (void)keyboard_.Key(HidKeyUsage::kLeftControl, false);
        talking_ = false;
        Refresh();
    }

    void SendEnter() {
        if (!ble_) {
            Sound(errand_runner_sfx::kFail);
            return;
        }
        Sound(errand_runner_sfx::kPress);
        (void)keyboard_.Key(HidKeyUsage::kEnter, true);
        (void)keyboard_.Key(HidKeyUsage::kEnter, false);
        StartWorking("Understanding...");
    }

    void Sound(std::span<const micropixel::ToneSpec> effect) { (void)tones_.Play(effect); }

    // ---- Network ----

    void StartServer() {
        if (listener_ != 0U) return;
        const auto net = network_.State();
        if (!net || !net->station_connected) return;
        auto listener = network_.Listen(kPort);
        if (!listener) {
            app_.log().Error("cannot listen on port 7334");
            return;
        }
        listener_ = *listener;
        (void)network_.Advertise("_cgotchi-errand", "CGotchi Errands", kPort, nullptr, 0U);
    }

    void OnSocketReady(uint32_t socket) {
        if (socket == listener_ && listener_ != 0U) {
            for (;;) {
                auto accepted = network_.Accept(listener_);
                if (!accepted) break;
                if (client_ != 0U) (void)network_.Close(client_);
                client_ = accepted->socket;
                line_length_ = 0U;
                pending_menu_ = 0U;
            }
            Refresh();
        } else if (socket == client_ && client_ != 0U) {
            ReadClient();
        }
    }

    void ReadClient() {
        uint8_t buffer[256];
        for (;;) {
            auto received = network_.Receive(client_, buffer, sizeof(buffer));
            if (!received) {
                if (received.error().code() != micropixel::ErrorCode::kWouldBlock) DropClient();
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
                } else if (c != '\r' && line_length_ + 1U < sizeof(line_)) {
                    line_[line_length_++] = c;
                }
            }
        }
    }

    void DropClient() {
        (void)network_.Close(client_);
        client_ = 0U;
        line_length_ = 0U;
        EndHold();
        if (page_ != Page::kMenu) ShowPage(Page::kMenu);
        else Refresh();
    }

    void SendLine(const char* line) {
        if (client_ != 0U) (void)network_.Send(client_, line, static_cast<uint32_t>(strlen(line)));
    }

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
        static char empty[1] = {'\0'};
        std::array<char*, 5> fields{empty, empty, empty, empty, empty};
        const uint32_t count = Split(line, fields);
        const char* kind = fields[0];
        if (Same(kind, "M") && count >= 3U && pending_menu_ < kMaxRows) {
            Row& row = next_menu_[pending_menu_++];
            Copy(row.id, sizeof(row.id), fields[1]);
            Copy(row.title, sizeof(row.title), fields[2]);
        } else if (Same(kind, "MEND")) {
            menu_ = next_menu_;
            menu_count_ = pending_menu_;
            pending_menu_ = 0U;
            Refresh();
        } else if (Same(kind, "S") && count >= 2U) {
            Copy(status_, sizeof(status_), fields[1]);
            if (page_ == Page::kVoice) ShowPage(Page::kWorking);
            else Refresh();
        } else if (Same(kind, "L") && count >= 2U) {
            Copy(voice_note_, sizeof(voice_note_), fields[1]);
            Refresh();
        } else if (Same(kind, "O") && count >= 5U && pending_quotes_ < kMaxRows) {
            Row& row = next_quotes_[pending_quotes_++];
            Copy(row.id, sizeof(row.id), fields[1]);
            Copy(row.title, sizeof(row.title), fields[2]);
            Copy(row.price, sizeof(row.price), fields[3]);
            Copy(row.detail, sizeof(row.detail), fields[4]);
        } else if (Same(kind, "QEND")) {
            if (page_ != Page::kWorking) {  // cancelled meanwhile
                pending_quotes_ = 0U;
                return;
            }
            quotes_ = next_quotes_;
            quote_count_ = pending_quotes_;
            pending_quotes_ = 0U;
            Copy(summary_, sizeof(summary_), count >= 2U ? fields[1] : "");
            Sound(errand_runner_sfx::kReady);
            page_ = Page::kQuotes;
            selected_ = 0U;
            Refresh();
        } else if (Same(kind, "R") && count >= 3U) {
            result_ok_ = Same(fields[1], "ok");
            Copy(result_, sizeof(result_), fields[2]);
            if (result_ok_) Sound(errand_runner_sfx::kReady);
            else Sound(errand_runner_sfx::kFail);
            ShowPage(Page::kResult);
        }
    }

    struct Cell final {
        Rect rect{};
        micropixel::RoundedRectNode box;
        micropixel::LabelNode title;
        micropixel::LabelNode price;
    };

    micropixel::Application& app_;
    micropixel::HidKeyboard keyboard_;
    micropixel::Network network_;
    micropixel::ToneSequencer<2U> tones_;
    micropixel::RendererInfo info_;
    micropixel::Scene scene_;
    micropixel::Timer poll_;
    micropixel::RoundedRectNode pill_dot_;
    micropixel::LabelNode pill_label_;
    micropixel::LabelNode title_;
    std::array<Cell, kMaxRows + 1U> cells_{};
    micropixel::LabelNode big_;
    micropixel::LabelNode small_;
    micropixel::LabelNode hint_;
    Rect bar_rect_{};
    micropixel::RoundedRectNode bar_back_;
    micropixel::RoundedRectNode bar_fill_;
    int32_t width_{};
    int32_t height_{};
    micropixel::Point touch_start_{};
    micropixel::Point last_point_{};

    Page page_{Page::kMenu};
    uint32_t selected_{};
    uint32_t tick_{};
    bool ble_{};
    bool talking_{};
    bool holding_{};
    uint64_t hold_start_{};

    std::array<Row, kMaxRows> menu_{};
    std::array<Row, kMaxRows> next_menu_{};
    uint32_t menu_count_{};
    uint32_t pending_menu_{};
    std::array<Row, kMaxRows> quotes_{};
    std::array<Row, kMaxRows> next_quotes_{};
    uint32_t quote_count_{};
    uint32_t pending_quotes_{};
    char summary_[kTextMax + 1U]{};
    char status_[kTextMax + 1U]{};
    char voice_note_[kTextMax + 1U]{};
    char result_[kTextMax + 1U]{};
    bool result_ok_{};

    uint32_t listener_{};
    uint32_t client_{};
    char line_[kLineMax + 1U]{};
    uint32_t line_length_{};
};

}  // namespace

int main() {
    micropixel::Application app;
    ErrandApp errands(app);
    app.log().Info("Errand Runner ready");
    app.Run([&errands](const micropixel::Event& event) { return errands.Handle(event); });
    return 0;
}
