#include "runtime/rtc.hpp"

namespace m3 {

namespace {

constexpr int64_t kDay = 86400;

// Howard Hinnant's days_from_civil / civil_from_days.
int64_t days_from_civil(int64_t y, int m, int d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

int64_t floor_div(int64_t a, int64_t b) { return a / b - (a % b != 0 && (a < 0) != (b < 0)); }

int weekday(int64_t seconds) { // 0 = Sunday; 1970-01-01 was a Thursday
    const int64_t days = floor_div(seconds, kDay);
    return static_cast<int>(((days + 4) % 7 + 7) % 7);
}

} // namespace

Rtc72421::Date Rtc72421::to_date(int64_t seconds) {
    const int64_t days = floor_div(seconds, kDay);
    const int64_t secs = seconds - days * kDay;
    const int64_t z = days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    const int m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    const int y = static_cast<int>(yoe + era * 400 + (m <= 2));
    return {y, m, d, static_cast<int>(secs / 3600), static_cast<int>(secs / 60 % 60), static_cast<int>(secs % 60)};
}

int64_t Rtc72421::to_seconds(const Date& d) {
    // Months and days out of range roll over (the chip's own behavior there
    // isn't known); time fields are added as they are.
    const int64_t months = int64_t{d.year} * 12 + (d.month - 1);
    const int64_t y = floor_div(months, 12);
    const int m = static_cast<int>(months - y * 12) + 1;
    return (days_from_civil(y, m, 1) + d.day - 1) * kDay + int64_t{d.hour} * 3600 + int64_t{d.minute} * 60 +
           d.second;
}

void Rtc72421::set_time(int64_t seconds, uint64_t now) {
    base_ = seconds;
    anchor_ = now;
    stopped_frac_ = 0;
}

int64_t Rtc72421::time(uint64_t now) const {
    if (stopped_ || now < anchor_) return base_;
    return base_ + static_cast<int64_t>((now - anchor_) / hz_);
}

uint8_t Rtc72421::read(uint32_t reg, uint64_t now) const {
    const Date t = to_date(time(now));
    const int year = ((t.year % 100) + 100) % 100;
    const bool h24 = cf_ & 4;
    const int hour = h24 ? t.hour : (t.hour % 12 == 0 ? 12 : t.hour % 12);
    switch (reg & 0xF) {
    case 0: return static_cast<uint8_t>(t.second % 10);
    case 1: return static_cast<uint8_t>(t.second / 10);
    case 2: return static_cast<uint8_t>(t.minute % 10);
    case 3: return static_cast<uint8_t>(t.minute / 10);
    case 4: return static_cast<uint8_t>(hour % 10);
    case 5: return static_cast<uint8_t>(hour / 10 | (!h24 && t.hour >= 12 ? 4 : 0));
    case 6: return static_cast<uint8_t>(t.day % 10);
    case 7: return static_cast<uint8_t>(t.day / 10);
    case 8: return static_cast<uint8_t>(t.month % 10);
    case 9: return static_cast<uint8_t>(t.month / 10);
    case 10: return static_cast<uint8_t>(year % 10);
    case 11: return static_cast<uint8_t>(year / 10);
    case 12: return static_cast<uint8_t>(((weekday(time(now)) + weekday_offset_) % 7 + 7) % 7);
    case 13: return static_cast<uint8_t>(cd_ & ~2); // never BUSY
    case 14: return ce_;
    default: return cf_;
    }
}

void Rtc72421::write(uint32_t reg, uint8_t value, uint64_t now) {
    value &= 0xF;
    reg &= 0xF;
    if (reg == 13) { // CD
        if ((value & 8) && !(cd_ & 8)) { // 30-second adjust: to the nearest minute
            const int64_t t = time(now);
            const int64_t s = ((t % 60) + 60) % 60;
            set_time(t - s + (s >= 30 ? 60 : 0), now);
        }
        cd_ = static_cast<uint8_t>(value & ~8 & ~2); // adjust ends at once; BUSY is read-only
        return;
    }
    if (reg == 14) {
        ce_ = value;
        return;
    }
    if (reg == 15) { // CF
        const bool stop = value & 2;
        if (stop && !stopped_) { // freeze, keeping the time
            const int64_t t = time(now);
            stopped_frac_ = now >= anchor_ ? (now - anchor_) % hz_ : 0;
            base_ = t;
            stopped_ = true;
        } else if (!stop && stopped_) {
            stopped_ = false;
            anchor_ = now >= stopped_frac_ ? now - stopped_frac_ : 0;
        }
        if (value & 1) { // RESET: the sub-second count starts again
            base_ = time(now);
            anchor_ = now;
            stopped_frac_ = 0;
        }
        cf_ = value;
        return;
    }

    const int64_t t = time(now);
    Date d = to_date(t);
    const bool h24 = cf_ & 4;
    auto set_digit = [](int& field, int digit, int scale) { field = field - (field / scale % 10) * scale + digit * scale; };
    switch (reg) {
    case 0: set_digit(d.second, value, 1); break;
    case 1: set_digit(d.second, value & 7, 10); break;
    case 2: set_digit(d.minute, value, 1); break;
    case 3: set_digit(d.minute, value & 7, 10); break;
    case 4:
    case 5: {
        // In 12-hour mode the chip holds 1-12 plus a PM flag.
        int hour = h24 ? d.hour : (d.hour % 12 == 0 ? 12 : d.hour % 12);
        bool pm = d.hour >= 12;
        if (reg == 4) set_digit(hour, value, 1);
        else {
            set_digit(hour, value & (h24 ? 3 : 1), 10);
            if (!h24) pm = value & 4;
        }
        d.hour = h24 ? hour : hour % 12 + (pm ? 12 : 0);
        break;
    }
    case 6: set_digit(d.day, value, 1); break;
    case 7: set_digit(d.day, value & 3, 10); break;
    case 8: set_digit(d.month, value, 1); break;
    case 9: set_digit(d.month, value & 1, 10); break;
    case 10:
    case 11: { // two digits: keep the century
        int yy = ((d.year % 100) + 100) % 100;
        set_digit(yy, value, reg == 10 ? 1 : 10);
        d.year = d.year - ((d.year % 100) + 100) % 100 + yy;
        break;
    }
    case 12: {
        weekday_offset_ = value % 7 - weekday(t);
        return;
    }
    default: return;
    }
    // Keep the sub-second count across a digit write.
    const uint64_t frac = stopped_ ? stopped_frac_ : (now >= anchor_ ? (now - anchor_) % hz_ : 0);
    const int64_t written = to_seconds(d);
    base_ = written;
    if (stopped_) stopped_frac_ = frac;
    else anchor_ = now - frac;
    weekday_offset_ += weekday(t) - weekday(written); // the day of the week counts on its own
}

} // namespace m3
