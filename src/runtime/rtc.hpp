// Epson RTC-72421 real-time clock (0xF0140000): sixteen 4-bit registers,
// register n at +4n, read and written in the top byte of each word (byte 1
// reads 0x03: battery voltage OK).
//
//   0-1 seconds (units, tens)   2-3 minutes   4-5 hours (tens bit 2: PM in
//   12-hour mode)   6-7 day   8-9 month   10-11 year (two digits)
//   12 day of the week (0-6, counts on its own)
//   13 CD: HOLD (bit 0), BUSY (1), IRQ flag (2), 30-second adjust (3)
//   14 CE: interrupt mask and period (kept, no interrupt is raised)
//   15 CF: RESET (bit 0), STOP (1), 24/12 hours (2, 1 = 24), TEST (3)
//
// Supermodel's RTC72421.cpp returns the host's time and ignores writes.
// Here the clock runs on the emulated time (CPU cycles), from a start time
// the frontend gives it, so a run is reproducible; writes set it, as on
// the chip (from the datasheet: digits are written while HOLD or STOP is
// set, RESET clears the sub-second count, 30-second adjust rounds to the
// nearest minute). Not emulated: BUSY (reads 0, so HOLD is never needed),
// the interrupt output, TEST, and the chip's behavior for impossible dates
// (here they roll over to a real date).
#pragma once

#include <array>
#include <cstdint>

namespace m3 {

class State;

class Rtc72421 {
public:
    // `clock_hz`: cycles per second of the `now` values passed in.
    explicit Rtc72421(uint64_t clock_hz) : hz_(clock_hz) {}

    // Wall-clock time as seconds since 1970-01-01 00:00 (no time zone: the
    // chip shows these fields as they are), taking effect at `now`.
    void set_time(int64_t seconds, uint64_t now);
    int64_t time(uint64_t now) const;

    uint8_t read(uint32_t reg, uint64_t now) const;
    void write(uint32_t reg, uint8_t value, uint64_t now);

    // Calendar helpers (proleptic Gregorian, days since 1970-01-01).
    struct Date {
        int year, month, day, hour, minute, second;
    };
    static Date to_date(int64_t seconds);
    static int64_t to_seconds(const Date& d);

    void state(State& s); // save states (runtime/state.hpp)

private:
    uint64_t hz_;
    int64_t base_ = 0;      // time at `anchor_`
    uint64_t anchor_ = 0;   // cycle count where the current second started
    bool stopped_ = false;
    uint64_t stopped_frac_ = 0; // sub-second cycles kept while stopped
    int weekday_offset_ = 0;    // written day of the week minus the computed one
    uint8_t cd_ = 0, ce_ = 0, cf_ = 0x4; // 24-hour mode
};

} // namespace m3
