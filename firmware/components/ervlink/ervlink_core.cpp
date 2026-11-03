#include "ervlink_core.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

namespace ervlink {

namespace {

bool is_hex(uint8_t c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'); }
uint8_t hexval(char c) { return c <= '9' ? c - '0' : c - 'A' + 10; }

std::string dump(const uint8_t *b, size_t n) {
  std::string s;
  char h[4];
  for (size_t i = 0; i < n; i++) {
    snprintf(h, sizeof h, "%02X ", b[i]);
    s += h;
  }
  return s;
}

bool known_register(Line l, const std::string &a) {
  if (l == RXD) return a == "00" || a == "10" || a == "20" || a == "30" || a == "F0";
  return a == "01" || a == "11" || a == "21" || a == "31" || a == "A1" || a == "B1" || a == "F1";
}

// One RXD frame: STX, address, 4 bytes as 8 hex chars low nibble first, ETX, BCC.
void frame(std::vector<uint8_t> &out, const char *addr, const uint8_t *v) {
  static const char hex[] = "0123456789ABCDEF";
  size_t start = out.size();
  out.push_back(0x02);
  out.push_back(addr[0]);
  out.push_back(addr[1]);
  for (int k = 0; k < 4; k++) {
    out.push_back(hex[v[k] & 0xF]);
    out.push_back(hex[v[k] >> 4]);
  }
  out.push_back(0x03);
  uint8_t bcc = 0;
  for (size_t i = start + 1; i < out.size(); i++) bcc ^= out[i];
  out.push_back((uint8_t) ~bcc);
}

const uint8_t IDLE00[4] = {0xFF, 0xFF, 0xFF, 0}, IDLE10[4] = {0xFF, 0xFF, 0, 0},
              IDLE20[4] = {0xFF, 0, 0, 0}, ZERO[4] = {0, 0, 0, 0};

const char *line_name(Line l) { return l == RXD ? "RXD" : "TXD"; }

}  // namespace

// Airflow code: 50 + 10 x v up to 13 (180 CFM); the menu then jumps to 200, which is code 16.
// 14 and 15 have never been seen.
float Core::cfm(uint8_t v) { return v <= 13 ? 50.0f + 10.0f * v : v == 16 ? 200.0f : NAN; }
bool Core::valid_airflow(uint8_t v) { return (v >= 1 && v <= 13 && v % 2) || v == 16; }

Core::Core(Hooks hooks, bool want_on, Settings saved, bool board_power_reset, uint32_t now)
    : h_(std::move(hooks)),
      want_on_(want_on),
      saved_(saved),
      board_power_reset_(board_power_reset),
      listen_from_(now) {}

void Core::log_(int level, const char *fmt, ...) {
  char msg[192];
  va_list args;
  va_start(args, fmt);
  vsnprintf(msg, sizeof msg, fmt, args);
  va_end(args);
  h_.log(level, msg);
}

// ---- Receiving ----

void Core::feed(Line which, uint8_t c, uint32_t now) {
  LineState &L = lines_[which];
  if (L.len == 0) {
    if (c == 0x02) L.buf[L.len++] = c;
    else L.stats.junk++;
    return;
  }
  // Bytes 1..11 are ASCII hex or ETX, never STX, so an STX there means the previous frame
  // was cut short: count it and resync. Byte 12 (BCC) can be 02.
  if (c == 0x02 && L.len < 12) {
    L.stats.bad++;
    log_(1, "%s truncated frame: %s", line_name(which), dump(L.buf, L.len).c_str());
    L.len = 1;
    return;
  }
  L.buf[L.len++] = c;
  if (L.len == 13) {
    handle_(which, now);
    L.len = 0;
  }
}

void Core::handle_(Line which, uint32_t t) {
  LineState &L = lines_[which];
  const uint8_t *f = L.buf;
  bool ok = f[11] == 0x03;
  uint8_t x = 0;
  for (int i = 1; i <= 11; i++) {
    if (i <= 10 && !is_hex(f[i])) ok = false;
    x ^= f[i];
  }
  if (!ok || (uint8_t) ~x != f[12]) {
    L.stats.bad++;
    log_(1, "%s bad frame (%s): %s", line_name(which), ok ? "BCC" : "format", dump(f, 13).c_str());
    return;
  }
  L.stats.frames++;
  if (which == TXD) {
    // The wall control is booting (see tick()). RXD otherwise never pauses for more than
    // ~51 ms, intercepting or not (its tap reads the wall control); checked outside
    // intercepts all the same.
    uint32_t rx = lines_[RXD].last_end ? lines_[RXD].last_end : listen_from_;
    if (!relay_ && t - rx > 500) arm_resume_("wall control silent while the ERV talks", t);
    if (!boot_checked_) {
      boot_checked_ = true;
      log_(2, "first ERV frame at %u ms, listening from %u ms, board power-up %s", (unsigned) t,
           (unsigned) listen_from_, board_power_reset_ ? "yes" : "no");
      // Started too late to see the wall control boot: go by the board's own power-up, as it
      // most likely shared the outage (a USB replug looks the same).
      if (listen_from_ > 1500 && board_power_reset_) arm_resume_("board power-on reset", t);
    }
  }
  L.last[0] = f[1];
  L.last[1] = f[2];
  L.last[2] = 0;
  L.last_end = t;
  std::string addr((const char *) f + 1, 2), data((const char *) f + 3, 8);
  // No command sets all of a register to FF (00 byte 3 and 10/20 bytes 2-3 are always 0).
  if (which == RXD && data == "FFFFFFFF") arm_resume_("wall control burst", t);
  // Low address nibble encodes direction: 0 on RXD, 1 on TXD.
  if (addr[1] != (which == RXD ? '0' : '1') || !known_register(which, addr)) {
    // Remember the last value per line+address so a few interleaved unknown registers are
    // each reported once per change, not on every frame.
    std::string key = std::string(line_name(which)) + " " + addr;
    std::string &seen = unknown_[key];
    if (seen != data) {
      seen = data;
      log_(1, "unexpected register: %s %s", key.c_str(), data.c_str());
      h_.text(Out::UNKNOWN_REG, key + " " + data);
    }
    return;
  }
  std::string &last = regs_[which][addr];
  if (last == data) return;
  log_(2, "%s %s: %s -> %s", line_name(which), addr.c_str(), last.empty() ? "(none)" : last.c_str(),
       data.c_str());
  last = data;
  h_.reg(which, addr, data);

  // Byte k is chars 2k (low nibble) and 2k+1 (high nibble).
  uint8_t b[4];
  for (int k = 0; k < 4; k++) b[k] = hexval(data[2 * k]) | hexval(data[2 * k + 1]) << 4;
  // While off the ERV zeroes 01 and 11, so their values mean nothing then.
  settled_since_ = t;
  if (addr == "01") {
    // Airflow FE/FE while on: the rest part of the ASHRAE cycle (2026-10-30: at 30 min/h, FE
    // exactly 30 min after the setting took effect). Fans stopped.
    bool resting = b[1] == 0xFE && b[2] == 0xFE;
    bool now_moving = b[0] && !resting;
    // A unit already moving air at boot counts as settled.
    if (now_moving && !moving_) moving_since_ = power_known_ ? t : t - 60000;
    power_known_ = true;
    on_ = b[0] != 0;
    if (powerup_ && on_) end_resume_("the ERV is on");  // otherwise learned in tick()
    moving_ = now_moving;
    cur_supply_ = b[1];
    cur_exhaust_ = b[2];
    h_.value(Out::POWER, on_);
    h_.value(Out::VENTILATING, moving_);
    h_.value(Out::SUPPLY_CFM, !on_ ? NAN : resting ? 0.0f : cfm(b[1]));
    h_.value(Out::EXHAUST_CFM, !on_ ? NAN : resting ? 0.0f : cfm(b[2]));
    // 11 may have come first (at boot, listening from mid-cycle).
    h_.value(Out::ASHRAE, on_ && cur_ashrae_ ? 10.0f * cur_ashrae_ : NAN);
  } else if (addr == "11") {
    cur_ashrae_ = b[0];
    h_.value(Out::ASHRAE, on_ ? 10.0f * b[0] : NAN);
    h_.value(Out::BOOST_MIN, b[1] ? 10.0f * b[1] : NAN);
  } else if (addr == "21") {
    boosting_ = b[0] != 0;
    h_.value(Out::BOOST, boosting_);
  } else if (addr == "B1") {
    memcpy(b1_, b, 4);
    b1_dirty_ = true;
  }
  if (addr == "01" || addr == "11" || addr == "21") publish_controls_();
}

// The controls show the ERV's state; while off, the settings Power on will use. Called after
// every change and after every request, so a refused request (or one the ERV ignores) snaps
// back in HA.
void Core::publish_controls_() {
  if (!power_known_) return;
  h_.value(Out::POWER_SWITCH, on_);
  h_.value(Out::BOOST_SWITCH, on_ && boosting_);
  char s[16];
  uint8_t sup = on_ && valid_airflow(cur_supply_) ? cur_supply_ : saved_.supply;
  uint8_t exh = on_ && valid_airflow(cur_exhaust_) ? cur_exhaust_ : saved_.exhaust;
  uint8_t ash = on_ ? cur_ashrae_ : saved_.ashrae;
  if (valid_airflow(sup)) {
    snprintf(s, sizeof s, "%d CFM", (int) cfm(sup));
    h_.text(Out::SUPPLY_SELECT, s);
  }
  if (valid_airflow(exh)) {
    snprintf(s, sizeof s, "%d CFM", (int) cfm(exh));
    h_.text(Out::EXHAUST_SELECT, s);
  }
  if (ash >= 1 && ash <= 6) {
    snprintf(s, sizeof s, "%d min/h", 10 * ash);
    h_.text(Out::ASHRAE_SELECT, s);
  }
}

// ---- Resume after a mains outage ----
// The wall control runs on the ERV's 5 V, so it boots with the ERV: it sends FFFFFFFF once on
// every RXD register, then nothing for ~2.1 s while the ERV's state line already runs
// (2025-12-14 capture). Either sign starts a resume episode, during which the ERV's "off" is
// the outage, not a choice. The episode ends once the ERV is on or the resume gives up, and
// want_on follows the ERV again. A dropout of one of the board's own taps is not mistaken for
// it in a harmful way: a TXD tap gap leaves RXD running (no episode), and an RXD tap gap
// leaves want_on current, so the episode ends at once.
//
// want_on: whether the ERV should be running, i.e. its power state as last reported, except
// the "off" it comes back with after an outage.

void Core::remember_(bool v) {
  if (v == want_on_) return;
  want_on_ = v;
  save_want_ = true;
  log_(2, "remembered: the ERV should be %s", v ? "on" : "off");
}

void Core::arm_resume_(const char *why, uint32_t t) {
  if (powerup_) return;
  log_(2, "mains power-up (%s)", why);
  powerup_ = true;
  powerup_at_ = t;
  next_try_ = t + 10000;
  tries_ = 0;
}

void Core::end_resume_(const char *why) {
  if (!powerup_) return;
  log_(2, "resume episode over: %s", why);
  powerup_ = false;
  if (power_known_) remember_(on_);
}

// ---- Every loop ----

void Core::tick(uint32_t now) {
  // No state from the ERV for 10 s: it is unpowered or disconnected. Report it as off (so
  // "off for 10 minutes" alerts still fire) with no readings, refuse commands, and forget the
  // last registers so the ERV's first frames on return are decoded afresh (without
  // publishing, so HA history shows no fake change). Not recorded in want_on: that only
  // follows what the ERV reports.
  uint32_t tx_end = lines_[TXD].last_end;
  bool now_linked = tx_end && now - tx_end < 10000;
  // At boot, give the ERV the same 10 s before calling it missing.
  if ((int8_t) now_linked != linked_ && (tx_end || now > 10000)) {
    linked_ = now_linked;
    h_.value(Out::CONNECTED, now_linked);
    if (!now_linked) {
      log_(1, "no state from the ERV for 10 s: reporting it off");
      power_known_ = on_ = moving_ = boosting_ = false;
      h_.value(Out::POWER, false);
      h_.value(Out::VENTILATING, false);
      h_.value(Out::BOOST, false);
      h_.value(Out::POWER_SWITCH, false);
      h_.value(Out::BOOST_SWITCH, false);
      h_.value(Out::SUPPLY_CFM, NAN);
      h_.value(Out::EXHAUST_CFM, NAN);
      h_.value(Out::ASHRAE, NAN);
      h_.value(Out::BOOST_MIN, NAN);
      regs_[TXD].clear();
    }
  }

  // Publish B1 once air has moved for 1 min; unavailable while off, resting or settling. B1 is
  // only meaningful with air moving: while off or resting, the sensors drift to the air sitting
  // in the unit (2026-10-30: "outside" 16 °C with 8 °C outside).
  bool b1_ok = moving_ && now - moving_since_ > 60000;
  if (b1_ok != b1_shown_ || (b1_ok && b1_dirty_)) {
    h_.value(Out::INSIDE_TEMP, b1_ok ? (int8_t) b1_[0] : NAN);
    h_.value(Out::INSIDE_RH, b1_ok ? b1_[1] : NAN);
    h_.value(Out::OUTSIDE_TEMP, b1_ok ? (int8_t) b1_[2] : NAN);
    h_.value(Out::OUTSIDE_RH, b1_ok ? b1_[3] : NAN);
    b1_shown_ = b1_ok;
    b1_dirty_ = false;
  }

  // Follow the ERV's power state, outside a resume episode, once the wall control has been
  // heard: until then a board that has just booted can't tell whether the wall control is
  // booting too (a shared outage, and the ERV's "off" is not a choice).
  if (power_known_ && !powerup_ && lines_[RXD].last_end) remember_(on_);

  // Resume after a power failure: the ERV came back off, but it should be on. Up to 3 tries,
  // 30 s apart, in case the ERV wasn't ready (a try counts once its relay goes in); waits up to
  // 2 min for the ERV's state and gives up after 5 min.
  if (powerup_ && (int32_t) (now - next_try_) >= 0) {
    if (power_known_ && on_) {
      end_resume_("the ERV is on");
    } else if (!h_.resume_enabled()) {
      end_resume_("resume is switched off");
    } else if (!want_on_) {
      end_resume_("the ERV was off before");
    } else if (!power_known_) {
      if (now - powerup_at_ > 120000) end_resume_("no state from the ERV");
      else next_try_ = now + 1000;
    } else if (tries_ >= 3 || now - powerup_at_ > 300000) {
      end_resume_("gave up, the ERV stays off");
    } else if (phase_ != IDLE || req_power_ >= 0) {
      next_try_ = now + 1000;
    } else {
      next_try_ = now + 30000;
      log_(1, "ERV off after power-up but it was on: turning it back on");
      req_power_ = 1;
    }
  }

  // Save the settings for Power on once 01/11/21 have been unchanged for 5 s with the ERV on
  // and not boosting (boost reports 200/200 in 01, which isn't a setting).
  if (on_ && !boosting_ && now - settled_since_ > 5000 && valid_airflow(cur_supply_) &&
      valid_airflow(cur_exhaust_) && cur_ashrae_ >= 1 && cur_ashrae_ <= 6 &&
      (cur_supply_ != saved_.supply || cur_exhaust_ != saved_.exhaust ||
       cur_ashrae_ != saved_.ashrae)) {
    saved_ = {cur_supply_, cur_exhaust_, cur_ashrae_};
    h_.save_settings(saved_);
    log_(2, "saved settings for Power on: supply %u, exhaust %u, ASHRAE %u", cur_supply_,
         cur_exhaust_, cur_ashrae_);
  }

  // ---- Intercept ----
  // The wall control sends its 5 RXD frames back to back (~150 ms) every 200 ms, then leaves
  // a ~51 ms gap; the ERV's TXD cycle ends in a ~81 ms gap (longer after F1). The relay
  // switches both lines at once and needs a few ms, so it switches only inside a gap, judged
  // from when the last cycle's final frame completed. UART receive latency (a few ms) is
  // covered by the 20 ms margin.
  int rx_left = gap_left_(RXD, "F0", nullptr, 51, now);
  int tx_left = gap_left_(TXD, "31", "F1", 81, now);
  // The wall control's cycle start, from its 00 frame (30 ms long).
  if (strcmp(lines_[RXD].last, "00") == 0) t_rx00_ = lines_[RXD].last_end - 30;

  if (phase_ == IDLE && (req_power_ >= 0 || req_boost_ >= 0 || req_supply_ || req_exhaust_ ||
                         req_ashrae_ || req_test_))
    start_request_(now);

  if (phase_ == WAIT_IN) {
    // Prefer a gap on both lines. They drift through each other over ~400 s and a shared gap
    // can be missing for a few minutes, so after 30 s settle for an RXD gap only: the ERV still
    // gets clean frames, the wall control may get one cut-off state frame (it fails BCC).
    bool shared = rx_left >= 20 && tx_left >= 20;
    bool late = now - t_req_ > 30000;
    if (shared || (late && rx_left >= 20)) {
      set_relay_(true);
      t_on_ = now;
      if (powerup_ && strcmp(what_, "Power on") == 0) tries_++;
      // Continue the wall control's cadence: the next cycle would start 200 ms after the last.
      t_next_ = t_rx00_ + 200;
      while ((int32_t) (t_next_ - now) < 15) t_next_ += 200;
      phase_ = ACTIVE;
      log_(2, "relay in (RXD gap %d ms, TXD gap %d ms left)", rx_left, tx_left);
    } else if (now - t_req_ > 60000) {
      phase_ = IDLE;
      log_(1, "intercept abandoned: no RXD gap found in 60 s");
    }
  }

  if (phase_ == ACTIVE && sent_ == cycles_) {
    // The last cycle started at t_next - 200 and takes ~150 ms to send.
    if ((int32_t) (now - (t_next_ - 200 + 160)) >= 0) phase_ = WAIT_OUT;
  } else if (phase_ == ACTIVE && (int32_t) (now - t_next_) >= 0) {
    bool cmd = has_cmd_ && sent_ < 3;
    std::vector<uint8_t> out;
    frame(out, "00", cmd ? c00_ : IDLE00);
    frame(out, "10", cmd ? c10_ : IDLE10);
    frame(out, "20", cmd ? c20_ : IDLE20);
    frame(out, "30", ZERO);
    frame(out, "F0", ZERO);
    h_.send(out.data(), out.size());
    if (sent_ == 0 && cmd)
      log_(2, "sending %s: 00 %s/ 10 %s/ 20 %s", what_, dump(c00_, 4).c_str(), dump(c10_, 4).c_str(),
           dump(c20_, 4).c_str());
    sent_++;
    t_next_ += 200;
  }

  if (phase_ == WAIT_OUT) {
    // Our cycles kept the wall control's phase, so its RXD gap lines up with ours and this is
    // normally true at once. Waiting longer wouldn't find a TXD gap either: the two lines'
    // phase barely moves in a second.
    if (rx_left >= 20 || now - t_on_ > 4000) {
      set_relay_(false);
      phase_ = IDLE;
      log_(2, "relay out after %u ms (RXD gap %d ms, TXD gap %d ms left)", (unsigned) (now - t_on_),
           rx_left, tx_left);
    }
  }

  // Last resort: never hold the link for more than 5 s.
  if (phase_ != IDLE && phase_ != WAIT_IN && now - t_on_ > 5000) {
    set_relay_(false);
    phase_ = IDLE;
    log_(0, "intercept timed out; relay forced out");
  }

  // Commit want_on only while not holding the link.
  if (save_want_ && phase_ == IDLE) {
    save_want_ = false;
    h_.save_want(want_on_);
  }
}

int Core::gap_left_(Line l, const char *end1, const char *end2, int gap, uint32_t now) const {
  const LineState &L = lines_[l];
  if (L.len != 0) return -1;
  if (strcmp(L.last, end1) == 0 || (end2 && strcmp(L.last, end2) == 0))
    return gap - (int) (now - L.last_end);
  return -1;
}

void Core::set_relay_(bool v) {
  relay_ = v;
  h_.relay(v);
  h_.value(Out::INTERCEPTING, v);
}

// Merge the pending requests into one command, as the wall control would send it
// (2026-10-29 session, CLAUDE.md), and start an intercept if there is anything to send.
void Core::start_request_(uint32_t now) {
  int8_t rp = req_power_, rb = req_boost_;
  uint8_t rs = req_supply_, re = req_exhaust_, ra = req_ashrae_;
  bool test = req_test_;
  req_power_ = req_boost_ = -1;
  req_supply_ = req_exhaust_ = req_ashrae_ = 0;
  req_test_ = false;
  memcpy(c00_, IDLE00, 4);
  memcpy(c10_, IDLE10, 4);
  memcpy(c20_, IDLE20, 4);
  has_cmd_ = false;
  what_ = "Intercept test";
  auto snapshot = [&](uint8_t power) {
    const uint8_t a[4] = {power, saved_.supply, saved_.exhaust, 0}, b[4] = {saved_.ashrae, 0xFF, 0, 0};
    memcpy(c00_, a, 4);
    memcpy(c10_, b, 4);
    memcpy(c20_, ZERO, 4);
    has_cmd_ = true;
  };
  // Off while the ERV is off or unknown (say, after an outage) sends nothing, but it is a
  // choice: no resume.
  if (rp == 0 && !(power_known_ && on_)) {
    end_resume_("Power off requested");
    remember_(false);
  }
  if (!power_known_) {
    log_(1, "request ignored: ERV state not known yet");
    test = false;
  } else if (!on_) {
    // Settings while off only change what Power on will send.
    if (rs) saved_.supply = rs;
    if (re) saved_.exhaust = re;
    if (ra) saved_.ashrae = ra;
    if (rs || re || ra) h_.save_settings(saved_);
    if (rb == 1) log_(1, "Boost ignored: the ERV is off");
    if (rp == 1) {
      snapshot(1);
      what_ = "Power on";
    }
  } else if (rp == 0) {
    snapshot(0);
    what_ = "Power off";
  } else if (rb == 1 && !boosting_) {
    const uint8_t a[4] = {1, 0xFF, 0xFF, 0}, b[4] = {cur_ashrae_, 6, 0, 0}, c[4] = {1, 0, 0, 0};
    memcpy(c00_, a, 4);
    memcpy(c10_, b, 4);
    memcpy(c20_, c, 4);
    has_cmd_ = true;
    what_ = "Boost on";
  } else if (rb == 0 && boosting_) {
    // The wall control's cancel: boost off plus an explicit airflow restore.
    const uint8_t a[4] = {1, saved_.supply, saved_.exhaust, 0}, b[4] = {cur_ashrae_, 6, 0, 0};
    memcpy(c00_, a, 4);
    memcpy(c10_, b, 4);
    memcpy(c20_, ZERO, 4);
    has_cmd_ = true;
    what_ = "Boost off";
  } else {
    what_ = "Settings";
    if ((rs || re) && boosting_) {
      log_(1, "airflow change ignored while boosting");
    } else {
      if (rs && rs != cur_supply_) {
        c00_[1] = rs;
        has_cmd_ = true;
      }
      if (re && re != cur_exhaust_) {
        c00_[2] = re;
        has_cmd_ = true;
      }
    }
    if (ra && ra != cur_ashrae_) {
      c10_[0] = ra;
      has_cmd_ = true;
    }
  }
  publish_controls_();
  bool fresh = now - lines_[TXD].last_end < 1000 && now - lines_[RXD].last_end < 1000;
  if (!has_cmd_ && !test) {
    // Nothing to send (already in that state, or ignored above).
  } else if (!fresh) {
    log_(1, "%s refused: no live traffic on both lines", what_);
  } else {
    // A command: 3 cycles (the wall control holds each command for 3), then 2 idle.
    // Test: 3 idle cycles.
    cycles_ = has_cmd_ ? 5 : 3;
    sent_ = 0;
    t_req_ = now;
    phase_ = WAIT_IN;
    log_(2, "%s: waiting for a gap on both lines", what_);
  }
}

}  // namespace ervlink
