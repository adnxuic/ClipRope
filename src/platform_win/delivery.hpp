#pragma once

#include <windows.h>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace cliprope::win {
enum class PastePayload { other, text, files, image_files };
enum class DeliveryResult {
    shortcut_sent, no_field, lost_target, access_denied,
    replaced_publication, send_failed, unconfirmed, consumed
};
DeliveryResult send_paste_shortcut(decltype(&SendInput) send = SendInput);
class TargetSession;
class CardPressInput;

class EditGesture final {
public:
    EditGesture();
    ~EditGesture();
    EditGesture(const EditGesture&) = delete;
    EditGesture& operator=(const EditGesture&) = delete;
    void begin_bind(bool warm = false, std::shared_ptr<CardPressInput> input = {});
    bool binding_ready();
    void bind(); // Synchronous adapter for command-line clients.
    std::optional<DeliveryResult> poll_delivery(const std::function<bool()>& current, PastePayload payload);
    // Only the ClipRope surface that interrupted this editing session may restore focus.
    const char* file_target_status() const { return file_status_; }
    const char* restore_status() const { return restore_status_; }
    std::string target_status() const;
    bool provider_timeout() const { return provider_timeout_; }
    void cancel();
    void allow_restore_from(HWND surface) { surface_ = surface; }
    void restore_interrupted_focus();
    bool bound_folder() const { return folder_; }
    DeliveryResult deliver(const std::function<bool()>& current, PastePayload payload = PastePayload::other);

private:
    HWND foreground_{}, field_{}, surface_{};
    DWORD pid_{}, tid_{}, delivery_sequence_{};
    const char* file_status_{"unbound"};
    const char* restore_status_{"not-needed"};
    std::string target_status_{"no-native-focus"};
    bool consumed_{true}, canceled_{};
    bool folder_{}, verifying_{}, native_{}, binding_complete_{}, provider_timeout_{};
    ULONGLONG deadline_{}, restore_until_{}, keys_until_{};
    std::unique_ptr<TargetSession> session_;
};
} // namespace cliprope::win
