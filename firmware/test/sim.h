#pragma once
// A simulated ERV, wall control and board around ervlink::Core, at byte-level timing.
//
// Time runs in microseconds. Both devices free-run their cycles as on the real link (wall
// control 5 frames every 200.1 ms, ERV 4 every 200.2 ms with A1 B1 F1 every 5th cycle,
// frames 29.7 ms apart, 2.292 ms per byte). The board's taps see the wall control's RXD and
// the ERV's TXD; the ERV hears the wall control while the relay is out and the board while
// it is in, and applies a command 0.3 s after hearing it. The board's loop runs every 16 ms,
// or every 1 ms while an intercept is busy, and feeds all buffered RXD bytes, then all TXD
// bytes, as the YAML does; each UART buffers at most 512 bytes.
//
// The ERV runs its ASHRAE cycle (rest for the rest of each hour, reported as FE/FE) and times
// boost (60 min, then back to the setting and a fresh cycle), with `minute` of simulated time
// per ERV minute so tests can run them quickly.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ervlink_core.h"

namespace sim {

using ervlink::Line;
using ervlink::Out;
using ervlink::RXD;
using ervlink::TXD;

constexpr uint64_t MS = 1000;
constexpr uint64_t BYTE_US = 2292;
constexpr uint64_t FRAME_SPACING_US = 29700;
constexpr size_t UART_BUFFER = 512;

// Every core log line of the current test, printed by the runner when it fails.
inline std::vector<std::string> &test_log() {
  static std::vector<std::string> v;
  return v;
}

inline std::vector<uint8_t> make_frame(const char *addr, const uint8_t *v) {
  static const char hex[] = "0123456789ABCDEF";
  std::vector<uint8_t> f = {0x02, (uint8_t) addr[0], (uint8_t) addr[1]};
  for (int k = 0; k < 4; k++) {
    f.push_back(hex[v[k] & 0xF]);
    f.push_back(hex[v[k] >> 4]);
  }
  f.push_back(0x03);
  uint8_t x = 0;
  for (size_t i = 1; i < f.size(); i++) x ^= f[i];
  f.push_back((uint8_t) ~x);
  return f;
}

struct Erv {
  bool powered = false, deaf = false;
  bool on = false, boost = false;
  uint8_t supply = 0, exhaust = 0, ashrae = 0;
  uint8_t b1[4] = {0x13, 0x30, 0x0F, 0x4B};  // 19 °C 48 %, 15 °C 75 %
  uint64_t minute = 60000 * MS;
  uint64_t cycle_start = 0, boost_until = 0;
  uint64_t next = 0;
  int cycle = 0;
  int commands = 0;  // frames that changed something

  bool resting(uint64_t t) const {
    if (!on || boost || ashrae < 1 || ashrae >= 6) return false;
    return (t - cycle_start) % (60 * minute) >= ashrae * 10 * minute;
  }

  void apply(const std::vector<uint8_t> &f, uint64_t t) {
    if (!powered || deaf || f.size() != 13) return;
    auto hv = [](uint8_t c) -> uint8_t { return c <= '9' ? c - '0' : c - 'A' + 10; };
    uint8_t b[4];
    for (int k = 0; k < 4; k++) b[k] = hv(f[3 + 2 * k]) | hv(f[4 + 2 * k]) << 4;
    std::string a{(char) f[1], (char) f[2]};
    bool was_on = on, was_boost = boost;
    uint8_t s0 = supply, e0 = exhaust, a0 = ashrae;
    if (a == "00") {
      if (b[0] == 0) {
        on = boost = false;
      } else if (b[0] == 1) {
        on = true;
      }
      if (on && b[1] != 0xFF) supply = b[1];
      if (on && b[2] != 0xFF) exhaust = b[2];
    } else if (a == "10") {
      if (on && b[0] != 0xFF) ashrae = b[0];
    } else if (a == "20") {
      if (on && b[0] != 0xFF) boost = b[0] == 1;
    }
    if (on && !was_on) cycle_start = t;
    if (ashrae != a0) cycle_start = t;
    if (boost && !was_boost) boost_until = t + 60 * minute;
    if (!boost && was_boost) cycle_start = t;  // a fresh run period after boost
    if (on != was_on || boost != was_boost || supply != s0 || exhaust != e0 || ashrae != a0) commands++;
  }

  // The frames of the cycle starting at t.
  std::vector<std::vector<uint8_t>> cycle_frames(uint64_t t) {
    if (boost && t >= boost_until) {
      boost = false;
      cycle_start = t;
    }
    std::vector<std::vector<uint8_t>> out;
    if (cycle++ % 5 == 4) {
      const uint8_t z[4] = {0, 0, 0, 0}, f1[4] = {0, 0, 0xE3, 0};
      out.push_back(make_frame("A1", z));
      out.push_back(make_frame("B1", b1));
      out.push_back(make_frame("F1", f1));
    } else {
      uint8_t r01[4] = {0, 0, 0, 0}, r11[4] = {0, 0, 0, 0}, r21[4] = {0, 0, 0, 0}, r31[4] = {0, 0, 0, 0};
      if (on) {
        // Boost reports 200/200 (code 16) as the airflow, the rest FE/FE.
        bool rest = resting(t);
        r01[0] = 1;
        r01[1] = boost ? 16 : rest ? 0xFE : supply;
        r01[2] = boost ? 16 : rest ? 0xFE : exhaust;
        r11[0] = ashrae;
        r11[1] = boost ? 6 : 0;
        r21[0] = boost;
      }
      out.push_back(make_frame("01", r01));
      out.push_back(make_frame("11", r11));
      out.push_back(make_frame("21", r21));
      out.push_back(make_frame("31", r31));
    }
    return out;
  }
};

struct WallControl {
  bool powered = false;
  uint64_t next = 0;
  bool burst = false;  // send the power-up FFFFFFFF burst at `next`
  uint8_t supply = 7, exhaust = 7, ashrae = 3;
  uint8_t c00[4], c10[4], c20[4];
  int cmd_cycles = 0;

  void press_power(bool p) {
    const uint8_t a[4] = {(uint8_t) p, supply, exhaust, 0}, b[4] = {ashrae, 0xFF, 0, 0}, z[4] = {0, 0, 0, 0};
    memcpy(c00, a, 4);
    memcpy(c10, b, 4);
    memcpy(c20, z, 4);
    cmd_cycles = 3;
  }
  void press_supply(uint8_t code) {
    supply = code;
    const uint8_t a[4] = {0xFF, code, 0xFF, 0}, b[4] = {0xFF, 0xFF, 0, 0}, c[4] = {0xFF, 0, 0, 0};
    memcpy(c00, a, 4);
    memcpy(c10, b, 4);
    memcpy(c20, c, 4);
    cmd_cycles = 3;
  }

  std::vector<std::vector<uint8_t>> cycle_frames() {
    std::vector<std::vector<uint8_t>> out;
    const uint8_t ff[4] = {0xFF, 0xFF, 0xFF, 0xFF}, i00[4] = {0xFF, 0xFF, 0xFF, 0},
                  i10[4] = {0xFF, 0xFF, 0, 0}, i20[4] = {0xFF, 0, 0, 0}, z[4] = {0, 0, 0, 0};
    bool cmd = cmd_cycles > 0;
    if (cmd) cmd_cycles--;
    out.push_back(make_frame("00", burst ? ff : cmd ? c00 : i00));
    out.push_back(make_frame("10", burst ? ff : cmd ? c10 : i10));
    out.push_back(make_frame("20", burst ? ff : cmd ? c20 : i20));
    out.push_back(make_frame("30", burst ? ff : z));
    out.push_back(make_frame("F0", burst ? ff : z));
    return out;
  }
};

struct Flash {
  bool want_on = true;
  ervlink::Settings settings{7, 7, 3};
  uint8_t resume_mark = 0;
  int want_writes = 0, settings_writes = 0, resume_writes = 0;
  int writes_while_relay_in = 0;
};

struct Sim {
  uint64_t t = 0;
  Erv erv;
  WallControl wc;
  Flash flash;
  bool resume_enabled = true;

  // Board.
  std::unique_ptr<ervlink::Core> core;
  uint64_t boot_at = 0, listen_at = UINT64_MAX;  // powered off until board_boot()
  uint32_t clock_offset = 0;                      // millis() at boot
  bool relay = false;
  bool tap_dead[2] = {false, false};
  bool corrupt_next_rxd_frame = false;
  bool brownout_on_relay = false;  // the board resets whenever the relay goes in
  uint32_t brownout_listen_ms = 300;
  int resets = 0;
  uint64_t next_loop = 0;

  // What the board reported.
  std::map<Out, float> values;
  std::map<Out, std::string> texts;
  std::vector<std::pair<uint64_t, bool>> relay_log;
  std::vector<std::string> logs;
  int relay_in_wc_frame = 0;  // relay switched while a wall control frame was on the wire
  uint64_t longest_intercept = 0, relay_since = 0;

  struct Byte {
    Line line;
    uint8_t c;
  };
  std::multimap<uint64_t, Byte> tap_bytes;
  struct Delivery {
    bool from_board;
    std::vector<uint8_t> frame;
  };
  std::multimap<uint64_t, Delivery> erv_rx;  // frames completing on the ERV's input
  std::multimap<uint64_t, std::vector<uint8_t>> erv_apply;  // heard, applied after 0.3 s
  std::deque<std::pair<uint64_t, uint64_t>> wc_frames;  // on-the-wire windows

  uint32_t core_ms() const { return clock_offset + (uint32_t) ((t - boot_at) / MS); }

  // ---- Scenario controls ----

  // The ERV's first cycle starts erv_ms after the wall control's burst. 48 ms in the
  // 2025-12-14 capture, which leaves the two lines without a shared gap for minutes (the core
  // then settles for an RXD gap after 30 s); the default starts them with one, which lasts
  // ~3 min as the cycles drift 0.5 ms/s against each other.
  void mains_on(uint32_t erv_ms = 84) {
    wc.powered = true;
    wc.burst = true;
    wc.next = t;
    wc.cmd_cycles = 0;
    erv.powered = true;
    erv.on = erv.boost = false;
    erv.supply = erv.exhaust = erv.ashrae = 0;
    erv.next = t + erv_ms * MS;
    erv.cycle = 0;
    // A few junk bytes as the lines come up.
    tap_bytes.insert({t + 2 * MS, {RXD, 0x00}});
    tap_bytes.insert({t + 3 * MS, {TXD, 0x80}});
  }
  // Frames on the wire are cut short.
  void mains_off() {
    wc.powered = erv.powered = false;
    erv.on = erv.boost = false;
    tap_bytes.erase(tap_bytes.upper_bound(t), tap_bytes.end());
    for (auto it = erv_rx.begin(); it != erv_rx.end();)
      it = it->second.from_board ? std::next(it) : erv_rx.erase(it);
    erv_apply.clear();
  }
  // The wall control alone restarts (the ERV keeps running).
  void wall_control_reset() {
    wc.burst = true;
    wc.next = t + 50 * MS;
    wc.cmd_cycles = 0;
  }
  // The board starts listening listen_ms after boot.
  void board_boot(uint32_t listen_ms = 300, bool power_reset = false) {
    core.reset();
    relay = false;
    boot_at = t;
    listen_at = t + listen_ms * MS;
    pending_power_reset = power_reset;
  }
  void board_off() {
    core.reset();
    if (relay) set_relay(false);
    listen_at = UINT64_MAX;
  }
  // The board's loop doesn't run for ms (a flash commit, the radio).
  void stall(uint32_t ms) { next_loop = t + ms * MS; }
  // ERV running at the given settings, as if set from the wall control long ago.
  void erv_running(uint8_t s, uint8_t e, uint8_t a) {
    erv.on = true;
    erv.supply = wc.supply = s;
    erv.exhaust = wc.exhaust = e;
    erv.ashrae = wc.ashrae = a;
    erv.cycle_start = t;
  }
  // The panel's On/Off button, with the settings it shows (the ERV's, when running).
  void panel_power(bool p) {
    if (erv.on && !erv.boost) {
      wc.supply = erv.supply;
      wc.exhaust = erv.exhaust;
      wc.ashrae = erv.ashrae;
    }
    wc.press_power(p);
  }

  float value(Out o) const {
    auto it = values.find(o);
    return it == values.end() ? NAN : it->second;
  }
  int relay_ins() const {
    int n = 0;
    for (auto &r : relay_log) n += r.second;
    return n;
  }

  // ---- Running ----

  void run_ms(uint64_t ms) {
    uint64_t end = t + ms * MS;
    while (t < end) step();
  }

  void step() {
    t += MS;
    emit_cycles();
    // Frames reach the ERV when complete, from whoever the relay connects it to.
    while (!erv_rx.empty() && erv_rx.begin()->first <= t) {
      auto d = erv_rx.begin()->second;
      erv_rx.erase(erv_rx.begin());
      if (d.from_board == relay) erv_apply.insert({t + 300 * MS, d.frame});
    }
    while (!erv_apply.empty() && erv_apply.begin()->first <= t) {
      erv.apply(erv_apply.begin()->second, t);
      erv_apply.erase(erv_apply.begin());
    }
    while (!wc_frames.empty() && wc_frames.front().second < t) wc_frames.pop_front();
    if (t < listen_at) {
      while (!tap_bytes.empty() && tap_bytes.begin()->first <= t) tap_bytes.erase(tap_bytes.begin());
      return;
    }
    if (!core) start_core();
    if (t < next_loop) return;
    uint32_t now = core_ms();
    // The UARTs' buffers, RXD first: at most 512 bytes each, later bytes are lost.
    std::vector<uint8_t> buf[2];
    while (!tap_bytes.empty() && tap_bytes.begin()->first <= t) {
      auto b = tap_bytes.begin()->second;
      tap_bytes.erase(tap_bytes.begin());
      if (!tap_dead[b.line] && buf[b.line].size() < UART_BUFFER) buf[b.line].push_back(b.c);
    }
    for (Line l : {RXD, TXD})
      for (uint8_t c : buf[l]) core->feed(l, c, now);
    core->tick(now);
    if (pending_brownout) {
      pending_brownout = false;
      resets++;
      board_off();
      board_boot(brownout_listen_ms, true);
      return;
    }
    next_loop = t + (core->busy() ? 1 : 16) * MS;
  }

 private:
  bool pending_power_reset = false, pending_brownout = false;

  void start_core() {
    ervlink::Hooks h;
    h.value = [this](Out o, float v) { values[o] = v; };
    h.text = [this](Out o, const std::string &s) { texts[o] = s; };
    h.reg = [](Line, const std::string &, const std::string &) {};
    h.relay = [this](bool v) { set_relay(v); };
    h.send = [this](const uint8_t *p, size_t n) {
      // Back to back from now, one frame per 13 bytes.
      for (size_t i = 0; i + 13 <= n; i += 13)
        erv_rx.insert({t + (i + 13) * BYTE_US, {true, std::vector<uint8_t>(p + i, p + i + 13)}});
    };
    h.save_want = [this](bool v) {
      flash.want_on = v;
      flash.want_writes++;
      flash.writes_while_relay_in += relay;
    };
    h.save_settings = [this](const ervlink::Settings &s) {
      flash.settings = s;
      flash.settings_writes++;
      flash.writes_while_relay_in += relay;
    };
    h.save_resume = [this](uint8_t m) {
      flash.resume_mark = m;
      flash.resume_writes++;
      flash.writes_while_relay_in += relay;
    };
    h.resume_enabled = [this]() { return resume_enabled; };
    h.log = [this](int level, const char *m) {
      char line[256];
      snprintf(line, sizeof line, "%9.3f %c %s", (t / 1000) / 1000.0, "EWI"[level], m);
      logs.push_back(line);
      test_log().push_back(line);
    };
    core.reset(new ervlink::Core(h, flash.want_on, flash.settings, flash.resume_mark, pending_power_reset,
                                 core_ms()));
    next_loop = t;
  }

  // The relay's contacts move within RELAY_US of the coil switching (G6S-2: 4 ms max): a wall
  // control frame on the wire in that window is a violation, including one about to start.
  static constexpr uint64_t RELAY_US = 5 * MS;
  void set_relay(bool v) {
    if (v == relay) return;
    bool clash = wc.powered && wc.next < t + RELAY_US;
    for (auto &w : wc_frames)
      if (w.first < t + RELAY_US && t <= w.second) clash = true;
    relay_in_wc_frame += clash;
    if (v) relay_since = t;
    else if (t - relay_since > longest_intercept) longest_intercept = t - relay_since;
    relay = v;
    relay_log.push_back({t, v});
    if (v && brownout_on_relay) pending_brownout = true;
  }

  void send_frames(Line line, uint64_t start, const std::vector<std::vector<uint8_t>> &frames) {
    for (size_t i = 0; i < frames.size(); i++) {
      uint64_t fs = start + i * FRAME_SPACING_US;
      std::vector<uint8_t> f = frames[i];
      if (line == RXD && corrupt_next_rxd_frame) {
        f[12] ^= 0x55;
        corrupt_next_rxd_frame = false;
      }
      for (size_t k = 0; k < f.size(); k++) tap_bytes.insert({fs + (k + 1) * BYTE_US, {line, f[k]}});
      uint64_t fe = fs + f.size() * BYTE_US;
      if (line == RXD) {
        erv_rx.insert({fe, {false, f}});
        wc_frames.push_back({fs, fe});
      }
    }
  }

  void emit_cycles() {
    if (wc.powered && wc.next <= t) {
      send_frames(RXD, wc.next, wc.cycle_frames());
      // After the power-up burst the wall control boots for ~2.1 s.
      wc.next += wc.burst ? 2111 * MS : 200100;
      wc.burst = false;
    }
    if (erv.powered && erv.next <= t) {
      send_frames(TXD, erv.next, erv.cycle_frames(erv.next));
      erv.next += 200200;
    }
  }
};

}  // namespace sim
