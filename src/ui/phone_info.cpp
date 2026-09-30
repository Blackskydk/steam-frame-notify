#include "ui/phone_info.h"

namespace frame_notify::ui {

std::string PhoneInfo::field(const std::string& name) const {
    const auto found = fields.find(name);
    return found == fields.end() ? std::string() : found->second;
}

bool phone_state_is_pairing(const std::string& state) {
    return state.rfind("pair_", 0) == 0;
}

bool phone_state_needs_attention(const std::string& state) {
    return state == "pair_conflict" || state == "pair_confirm" || state == "pair_done" ||
           state == "pair_failed";
}

PhoneChip phone_chip(const PhoneInfo& info) {
    const std::string& state = info.state;
    if (state == "connected") {
        const std::string phone = info.field("phone");
        return {(phone.empty() ? std::string("iPhone") : phone) + " connected", Tone::kGood};
    }
    if (state == "connecting") return {"Connecting\xE2\x80\xA6", Tone::kWarning};
    if (state == "unpaired") return {"Pair an iPhone", Tone::kActive};
    if (state == "needs_repair") return {"Needs attention", Tone::kWarning};
    if (state == "no_bluetooth") {
        return {info.field("reason") == "powered_off" ? "Bluetooth is off" : "Bluetooth unavailable",
                Tone::kBad};
    }
    if (state == "pair_confirm") return {"Confirm pairing code", Tone::kActive};
    if (state == "pair_done") return {"Paired", Tone::kGood};
    if (state == "pair_failed") return {"Pairing failed", Tone::kBad};
    if (phone_state_is_pairing(state)) return {"Pairing\xE2\x80\xA6", Tone::kActive};
    if (state == "helper_unavailable") return {"Bluetooth helper unavailable", Tone::kBad};
    if (state == "helper_restarting") return {"Restarting Bluetooth\xE2\x80\xA6", Tone::kWarning};
    if (state == "starting") return {"Starting\xE2\x80\xA6", Tone::kNeutral};
    return {state, Tone::kNeutral};
}

}  // namespace frame_notify::ui
