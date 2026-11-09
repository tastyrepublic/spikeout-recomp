// Built without libcurl (no internet lobby): a report can only be saved as a zip (src/net/report.hpp).
#include "net/report.hpp"

namespace net::report {

bool available() { return false; }
bool can_log_in() { return false; }
DeviceCode login_start() { return {{}, {}, {}, "this build has no internet connection"}; }
Poll login_poll(const std::string&) { return {}; }
std::string login_name(const std::string&, std::string* error) {
    if (error) *error = "this build has no internet connection";
    return {};
}
Sent send_as_user(const Report&, const std::string&) { return {false, 0, {}, "this build has no internet connection"}; }
Issue fetch_issue(int, const std::string&) {
    Issue i;
    i.error = "this build has no internet connection";
    return i;
}

} // namespace net::report
