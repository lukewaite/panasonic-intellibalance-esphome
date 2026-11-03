#pragma once
// The ERV link logic, free of ESPHome so it can be tested on a host (firmware/test/).
// firmware/ervlink.yaml feeds it the bytes from both UARTs and a clock, and wires its hooks
// to entities, the relay, the RXD driver and flash.
//
//   RXD line = wall control -> ERV (commands); its tap reads the wall control's output.
//   TXD line = ERV -> wall control (state); its tap reads the ERV's output.
// Both taps keep reading live traffic while intercepting.
//
// Frame: 02 | A1 A0 | D7..D0 | 03 | BCC, ASCII hex, BCC = ~(XOR of A1..ETX).
// The 8 data chars are 4 bytes, each sent LOW NIBBLE FIRST: "3103F0B4" = 13 30 0F 4B.
// Register map and quirks: docs/protocol.md.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace ervlink {

enum Line : uint8_t { RXD = 0, TXD = 1 };

// What the core reports, through Hooks::value (on/off as 1/0, numbers, NAN = unknown) or
// Hooks::text (the selects' options, the unexpected-register report).
enum class Out : uint8_t {
  POWER,          // 01 byte 0
  VENTILATING,    // fans running: on and not in the ASHRAE rest
  BOOST,          // 21 byte 0
  CONNECTED,      // the ERV's state line is live
  INTERCEPTING,   // relay in
  POWER_SWITCH,   // the controls, showing the ERV's state
  BOOST_SWITCH,
  SUPPLY_CFM,     // 01 bytes 1/2; 0 during the rest, NAN while off
  EXHAUST_CFM,
  ASHRAE,         // 11 byte 0, min/h
  BOOST_MIN,      // 11 byte 1, min; NAN unless boosting
  INSIDE_TEMP,    // B1, NAN unless air has moved for 1 min
  INSIDE_RH,
  OUTSIDE_TEMP,
  OUTSIDE_RH,
  SUPPLY_SELECT,  // text: "120 CFM"; while off, what Power on will use
  EXHAUST_SELECT,
  ASHRAE_SELECT,  // text: "30 min/h"
  UNKNOWN_REG,    // text: "TXD 41 00000000"
};

// Power on's settings: airflow codes and ASHRAE in tens of min/h.
struct Settings {
  uint8_t supply, exhaust, ashrae;
};

struct Stats {
  uint32_t frames = 0, bad = 0, junk = 0;
};

// Every hook must be set.
struct Hooks {
  std::function<void(Out, float)> value;
  std::function<void(Out, const std::string &)> text;
  // A known register changed (raw hex, for the diagnostic sensors).
  std::function<void(Line, const std::string &addr, const std::string &data)> reg;
  std::function<void(bool)> relay;
  std::function<void(const uint8_t *, size_t)> send;  // RXD driver, towards the ERV
  // Persist. Called only while the link is free (a flash commit takes tens of ms).
  std::function<void(bool)> save_want;
  std::function<void(const Settings &)> save_settings;
  // A resume in progress, so a reset in the middle of it (a brownout as the relay goes in)
  // carries on with the tries left instead of starting over: 0 none, else 1 + tries made.
  std::function<void(uint8_t)> save_resume;
  std::function<bool()> resume_enabled;  // the "Resume after power failure" switch
  std::function<void(int level, const char *msg)> log;  // 0 error, 1 warning, 2 info
};

class Core {
 public:
  // now: ms since boot when listening starts. want_on/saved/resume_mark: as persisted.
  // board_power_reset: the board itself just powered up (power-on, brownout or power-glitch
  // reset).
  Core(Hooks hooks, bool want_on, Settings saved, uint8_t resume_mark, bool board_power_reset,
       uint32_t now);

  void feed(Line line, uint8_t c, uint32_t now);  // one received byte
  void tick(uint32_t now);                        // call after feeding, every loop

  // From the controls. Sent by tick(); refused or ignored ones snap the controls back.
  void request_power(bool on) { req_power_ = on; }
  void request_boost(bool on) { req_boost_ = on; }
  void request_supply(uint8_t code) { req_supply_ = code; }
  void request_exhaust(uint8_t code) { req_exhaust_ = code; }
  void request_ashrae(uint8_t tens) { req_ashrae_ = tens; }
  void request_test() { req_test_ = true; }

  // An intercept is under way: run the loop as fast as possible.
  bool busy() const { return phase_ != IDLE; }
  const Stats &stats(Line l) const { return lines_[l].stats; }
  // Inspection, for tests.
  bool want_on() const { return want_on_; }
  bool resuming() const { return powerup_; }
  bool relay() const { return relay_; }
  const Settings &saved() const { return saved_; }

  // Airflow code -> CFM (NAN if not one the wall control uses), and its menu text.
  static float cfm(uint8_t code);
  static bool valid_airflow(uint8_t code);

 private:
  struct LineState {
    uint8_t buf[13];
    uint8_t len = 0;
    char last[3] = "";      // address of the last good frame
    uint32_t last_end = 0;  // when it was seen complete
    uint32_t last_late = 0;  // how much earlier it may have completed (time since the previous tick)
    Stats stats;
  };
  enum Phase { IDLE, WAIT_IN, ACTIVE, WAIT_OUT };

  void handle_(Line which, uint32_t t);
  void publish_controls_();
  void remember_(bool v);
  void arm_resume_(const char *why, uint32_t t);
  void end_resume_(const char *why);
  void mark_resume_(uint8_t m);
  void start_request_(uint32_t now);
  void set_relay_(bool v);
  int gap_left_(Line l, const char *end1, const char *end2, int gap, uint32_t now) const;
  void log_(int level, const char *fmt, ...);

  Hooks h_;
  LineState lines_[2];
  std::map<std::string, std::string> regs_[2];  // last value per known register
  std::map<std::string, std::string> unknown_;  // last value per unexpected line+register

  // Latest decoded state.
  bool on_ = false, boosting_ = false, moving_ = false, power_known_ = false;
  uint8_t cur_supply_ = 0, cur_exhaust_ = 0, cur_ashrae_ = 0;
  uint32_t settled_since_ = 0, moving_since_ = 0;
  uint8_t b1_[4] = {};
  bool b1_dirty_ = false, b1_shown_ = false;
  int8_t linked_ = -1;  // ERV state line: -1 not yet known, 0 silent, 1 live

  // Resume after a mains outage (see tick()).
  bool want_on_, save_want_ = false, save_resume_ = false;
  uint8_t resume_mark_ = 0;
  Settings saved_;
  bool board_power_reset_, boot_checked_ = false, powerup_ = false;
  uint32_t listen_from_, powerup_at_ = 0, next_try_ = 0;
  uint8_t tries_ = 0;

  // Requests: power/boost -1 none, 0 off, 1 on; airflow code / ASHRAE tens, 0 = none.
  int8_t req_power_ = -1, req_boost_ = -1;
  uint8_t req_supply_ = 0, req_exhaust_ = 0, req_ashrae_ = 0;
  bool req_test_ = false;

  // Intercept.
  Phase phase_ = IDLE;
  bool relay_ = false;
  uint8_t cycles_ = 0, sent_ = 0;
  uint32_t t_req_ = 0, t_next_ = 0, t_rx00_ = 0, t_on_ = 0, last_tick_ = 0;
  uint8_t c00_[4], c10_[4], c20_[4];  // the command; FF = leave this byte alone
  bool has_cmd_ = false;
  const char *what_ = "";
};

}  // namespace ervlink
