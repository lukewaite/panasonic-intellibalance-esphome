// Host tests for ervlink::Core. Run: firmware/test/run.sh (VERBOSE=1 prints every core log).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <vector>

#include "sim.h"

using namespace sim;

// ---- A minimal test runner ----

struct Test {
  const char *name;
  std::function<void()> fn;
};
static std::vector<Test> &tests() {
  static std::vector<Test> t;
  return t;
}
struct Register {
  Register(const char *n, std::function<void()> f) { tests().push_back({n, f}); }
};
#define TEST(name)                                 \
  static void name();                              \
  static Register reg_##name(#name, name);         \
  static void name()

static int failures = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      failures++;                                                            \
      printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
    }                                                                        \
  } while (0)

// A board on its own supply, the ERV running at 100/100 CFM (code 5), ASHRAE 30 min/h, with
// that remembered in flash.
static void steady(Sim &s, uint32_t erv_ms = 84) {
  s.flash = Flash{true, {5, 5, 3}};
  s.mains_on(erv_ms);
  s.run_ms(3000);
  s.erv_running(5, 5, 3);
  s.board_boot();
  s.run_ms(20000);
}

static bool logged(const Sim &s, const char *text) {
  for (auto &l : s.logs)
    if (l.find(text) != std::string::npos) return true;
  return false;
}

// A Core fed directly, for decoder tests.
struct Direct {
  std::map<Out, float> values;
  std::map<Out, std::vector<std::string>> texts;
  std::unique_ptr<ervlink::Core> core;
  uint32_t t = 1000;
  Direct() {
    ervlink::Hooks h;
    h.value = [this](Out o, float v) { values[o] = v; };
    h.text = [this](Out o, const std::string &s) { texts[o].push_back(s); };
    h.reg = [](Line, const std::string &, const std::string &) {};
    h.relay = [](bool) {};
    h.send = [](const uint8_t *, size_t) {};
    h.save_want = [](bool) {};
    h.save_settings = [](const ervlink::Settings &) {};
    h.save_resume = [](uint8_t) {};
    h.resume_enabled = [] { return true; };
    h.log = [](int, const char *m) { test_log().push_back(m); };
    core.reset(new ervlink::Core(h, true, {5, 5, 3}, 0, false, t));
  }
  void bytes(Line l, const std::vector<uint8_t> &b) {
    for (uint8_t c : b) {
      t += 2;
      core->feed(l, c, t);
      core->tick(t);
    }
  }
  void frame(Line l, const char *addr, std::vector<uint8_t> v) { bytes(l, make_frame(addr, v.data())); }
};

// ---- Decoding ----

TEST(decodes_clean_traffic) {
  Sim s;
  steady(s);
  s.run_ms(10000);
  auto &rx = s.core->stats(RXD), &tx = s.core->stats(TXD);
  CHECK(rx.bad == 0 && tx.bad == 0);
  CHECK(rx.frames > 240 && tx.frames > 180);  // ~25/s and ~19/s
  CHECK(s.value(Out::POWER) == 1);
  CHECK(s.value(Out::SUPPLY_CFM) == 100);
  CHECK(s.value(Out::ASHRAE) == 30);
  CHECK(s.value(Out::CONNECTED) == 1);
  CHECK(s.texts[Out::SUPPLY_SELECT] == "100 CFM");
  CHECK(s.texts[Out::ASHRAE_SELECT] == "30 min/h");
  CHECK(s.relay_ins() == 0);
}

TEST(counts_a_corrupted_frame) {
  Sim s;
  steady(s);
  s.corrupt_next_rxd_frame = true;
  s.run_ms(1000);
  CHECK(s.core->stats(RXD).bad == 1);
}

TEST(b1_waits_for_a_minute_of_airflow) {
  Sim s;
  s.flash = Flash{false, {5, 5, 3}};
  s.mains_on();
  s.board_boot();
  s.run_ms(5000);
  s.wc.press_power(true);
  s.run_ms(30000);
  CHECK(std::isnan(s.value(Out::INSIDE_TEMP)));
  s.run_ms(40000);
  CHECK(s.value(Out::INSIDE_TEMP) == 19);
  CHECK(s.value(Out::INSIDE_RH) == 48);
  CHECK(s.value(Out::OUTSIDE_TEMP) == 15);
  CHECK(s.value(Out::OUTSIDE_RH) == 75);
}

// The 2025-12-14 capture, replayed byte for byte: ERV mains power-on at 6.83 s (the wall
// control's burst), then the panel's On/Off press at 14.24 s.
TEST(replays_the_2025_12_14_power_on) {
  std::ifstream in(FIXTURES "/2025-12-14-frames.txt");
  CHECK(in.good());
  struct F {
    double t;
    Line l;
    std::string addr, data;
  };
  std::vector<F> frames;
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream ss(line);
    F f;
    std::string l;
    ss >> f.t >> l >> f.addr >> f.data;
    f.l = l == "RXD" ? RXD : TXD;
    frames.push_back(f);
  }
  CHECK(frames.size() == 596);

  for (bool want : {true, false}) {
    static std::vector<std::string> logs;
    logs.clear();
    bool saved_want = want;
    ervlink::Hooks h;
    std::map<Out, float> v;
    int relays = 0;
    h.value = [&](Out o, float x) { v[o] = x; };
    h.text = [](Out, const std::string &) {};
    h.reg = [](Line, const std::string &, const std::string &) {};
    h.relay = [&](bool r) { relays += r; };
    h.send = [](const uint8_t *, size_t) {};
    h.save_want = [&](bool w) { saved_want = w; };
    h.save_settings = [](const ervlink::Settings &) {};
    h.save_resume = [](uint8_t) {};
    h.resume_enabled = [] { return true; };
    h.log = [&](int, const char *m) {
      logs.push_back(m);
      test_log().push_back(m);
    };
    // The board has been up for a minute when the capture starts.
    ervlink::Core core(h, want, {1, 1, 2}, 0, false, 60000);
    // Every byte completes 2.292 ms after the previous one in its frame; the two lines'
    // frames overlap, so merge them in time order. Tick every byte, like a fast loop.
    struct B {
      uint32_t t;
      Line l;
      uint8_t c;
    };
    std::vector<B> bytes;
    for (auto &f : frames) {
      std::vector<uint8_t> raw = {0x02, (uint8_t) f.addr[0], (uint8_t) f.addr[1]};
      for (char c : f.data) raw.push_back(c);
      raw.push_back(0x03);
      uint8_t x = 0;
      for (size_t i = 1; i < raw.size(); i++) x ^= raw[i];
      raw.push_back((uint8_t) ~x);
      for (size_t k = 0; k < raw.size(); k++)
        bytes.push_back({60000 + (uint32_t) ((f.t + (k + 1) * 0.002292) * 1000), f.l, raw[k]});
    }
    std::stable_sort(bytes.begin(), bytes.end(), [](const B &a, const B &b) { return a.t < b.t; });
    for (auto &b : bytes) {
      core.feed(b.l, b.c, b.t);
      core.tick(b.t);
    }
    CHECK(core.stats(RXD).frames == 317 && core.stats(TXD).frames == 279);
    CHECK(core.stats(RXD).bad == 0 && core.stats(TXD).bad == 0);
    bool armed = false;
    for (auto &l : logs) armed |= l.find("mains power-up (wall control burst)") == 0;
    CHECK(armed);
    // The panel turned it on before the resume's first try: the episode ends, and the
    // board learns "on" whatever it remembered before.
    CHECK(!core.resuming());
    CHECK(core.want_on() && saved_want);
    CHECK(v[Out::POWER] == 1);
    CHECK(v[Out::SUPPLY_CFM] == 80);
    CHECK(relays == 0);
  }
}

// ---- Resume after a mains outage ----

TEST(resumes_after_an_outage) {
  Sim s;
  steady(s);
  s.mains_off();
  s.run_ms(15000);
  CHECK(s.value(Out::CONNECTED) == 0);
  CHECK(s.value(Out::POWER) == 0);
  CHECK(std::isnan(s.value(Out::SUPPLY_CFM)));
  s.run_ms(45000);
  s.mains_on();
  s.run_ms(9000);
  CHECK(!s.erv.on);
  CHECK(s.relay_ins() == 0);
  s.run_ms(4000);
  CHECK(s.erv.on);
  CHECK(s.erv.supply == 5 && s.erv.exhaust == 5 && s.erv.ashrae == 3);
  CHECK(s.relay_ins() == 1);
  CHECK(!s.relay);
  s.run_ms(60000);
  CHECK(s.relay_ins() == 1);
  CHECK(!s.core->resuming());
  CHECK(s.flash.want_on);
  CHECK(s.value(Out::POWER) == 1);
}

TEST(resumes_after_a_short_blip_without_the_burst) {
  Sim s;
  steady(s);
  s.mains_off();
  s.run_ms(3000);  // under the 10 s link-loss threshold
  s.corrupt_next_rxd_frame = true;  // the burst's 00 frame
  s.mains_on();
  s.run_ms(15000);
  CHECK(s.erv.on);
  CHECK(s.relay_ins() == 1);
  CHECK(s.flash.want_on);
}

TEST(resumes_with_the_whole_burst_lost) {
  Sim s;
  steady(s);
  s.mains_off();
  s.run_ms(20000);
  s.tap_dead[RXD] = true;  // the board misses the burst, then sees the wall control boot
  s.mains_on();
  s.run_ms(500);
  s.tap_dead[RXD] = false;
  s.run_ms(15000);
  CHECK(s.erv.on);
  CHECK(s.relay_ins() == 1);
}

TEST(stays_off_after_an_outage_if_it_was_off) {
  Sim s;
  steady(s);
  s.wc.press_power(false);
  s.run_ms(5000);
  CHECK(!s.erv.on);
  CHECK(!s.flash.want_on);
  s.mains_off();
  s.run_ms(30000);
  s.mains_on();
  s.run_ms(120000);
  CHECK(!s.erv.on);
  CHECK(s.relay_ins() == 0);
  CHECK(!s.core->resuming());
}

TEST(resume_switch_off_does_nothing) {
  Sim s;
  steady(s);
  s.resume_enabled = false;
  s.mains_off();
  s.run_ms(30000);
  s.mains_on();
  s.run_ms(60000);
  CHECK(!s.erv.on);
  CHECK(s.relay_ins() == 0);
  // The episode ends recording the ERV's actual state.
  CHECK(!s.core->resuming());
  CHECK(!s.flash.want_on);
}

TEST(power_off_during_a_pending_resume_wins) {
  Sim s;
  steady(s);
  s.mains_off();
  s.run_ms(30000);
  s.mains_on();
  s.run_ms(3000);
  CHECK(s.core->resuming());
  s.core->request_power(false);
  s.run_ms(120000);
  CHECK(!s.erv.on);
  CHECK(s.relay_ins() == 0);
  CHECK(!s.flash.want_on);
}

TEST(gives_up_after_three_tries_and_remembers_off) {
  Sim s;
  steady(s);
  s.mains_off();
  s.run_ms(30000);
  s.mains_on();
  s.erv.deaf = true;
  s.run_ms(200000);
  CHECK(s.relay_ins() == 3);
  CHECK(!s.core->resuming());
  CHECK(!s.flash.want_on);
  // Nothing fires later.
  s.erv.deaf = false;
  s.run_ms(600000);
  CHECK(s.relay_ins() == 3);
  CHECK(!s.erv.on);
}

TEST(board_and_erv_lose_power_together) {
  for (uint32_t listen : {200u, 800u, 1200u}) {
    Sim s;
    steady(s);
    s.mains_off();
    s.board_off();
    s.run_ms(30000);
    s.mains_on();
    s.board_boot(listen, true);
    s.run_ms(20000);
    CHECK(s.erv.on);
    CHECK(s.relay_ins() == 1);
  }
}

TEST(board_and_erv_lose_power_together_slow_board) {
  // Listening only after the wall control's boot: falls back on the board's power-on reset.
  Sim s;
  steady(s);
  s.mains_off();
  s.board_off();
  s.run_ms(30000);
  s.mains_on();
  s.board_boot(2500, true);
  s.run_ms(20000);
  CHECK(s.erv.on);
  CHECK(s.relay_ins() == 1);
}

TEST(board_reboot_leaves_an_off_erv_off) {
  for (bool power_reset : {false, true}) {
    Sim s;
    steady(s);
    s.wc.press_power(false);
    s.run_ms(5000);
    s.board_off();
    s.run_ms(5000);
    s.board_boot(300, power_reset);
    s.run_ms(120000);
    CHECK(!s.erv.on);
    CHECK(s.relay_ins() == 0);
  }
}

TEST(board_reboot_with_stale_flash_learns_the_erv) {
  // Flash says on (say, a fresh install), the ERV is off by choice: nothing is sent and the
  // board learns "off".
  Sim s;
  s.flash = Flash{true, {5, 5, 3}};
  s.mains_on();
  s.run_ms(5000);
  s.board_boot(300, true);
  s.run_ms(120000);
  CHECK(!s.erv.on);
  CHECK(s.relay_ins() == 0);
  CHECK(!s.flash.want_on);
}

TEST(txd_tap_dropout_does_not_undo_a_panel_off) {
  Sim s;
  steady(s);
  s.tap_dead[TXD] = true;
  s.run_ms(12000);
  CHECK(s.value(Out::CONNECTED) == 0);
  s.wc.press_power(false);
  s.run_ms(20000);
  s.tap_dead[TXD] = false;
  s.run_ms(120000);
  CHECK(!s.erv.on);
  CHECK(s.relay_ins() == 0);
  CHECK(!s.flash.want_on);
}

TEST(rxd_tap_dropout_is_harmless) {
  Sim s;
  steady(s);
  s.tap_dead[RXD] = true;
  s.run_ms(5000);
  s.tap_dead[RXD] = false;
  s.run_ms(60000);
  CHECK(s.erv.on);
  CHECK(s.relay_ins() == 0);
  CHECK(!s.core->resuming());
  CHECK(s.flash.want_on);
}

// ---- Controls ----

TEST(power_off_and_on) {
  Sim s;
  steady(s);
  s.core->request_power(false);
  s.run_ms(3000);
  CHECK(!s.erv.on);
  CHECK(s.value(Out::POWER) == 0);
  CHECK(!s.flash.want_on);
  s.core->request_power(true);
  s.run_ms(3000);
  CHECK(s.erv.on);
  CHECK(s.erv.supply == 5 && s.erv.ashrae == 3);
  CHECK(s.flash.want_on);
  CHECK(s.relay_ins() == 2);
  CHECK(s.relay_in_wc_frame == 0);
  CHECK(s.longest_intercept < 2000 * MS);
}

TEST(boost_on_and_cancel) {
  Sim s;
  steady(s);
  s.core->request_boost(true);
  s.run_ms(3000);
  CHECK(s.erv.boost);
  CHECK(s.value(Out::BOOST) == 1);
  CHECK(s.value(Out::BOOST_MIN) == 60);
  s.run_ms(10000);
  // Boost's 200/200 is not saved as a setting.
  CHECK(s.flash.settings.supply == 5);
  s.core->request_boost(false);
  s.run_ms(3000);
  CHECK(!s.erv.boost);
  CHECK(s.erv.supply == 5 && s.erv.exhaust == 5);
}

TEST(airflow_and_ashrae_changes_are_sent_and_saved) {
  Sim s;
  steady(s);
  s.core->request_supply(9);
  s.core->request_ashrae(4);
  s.run_ms(3000);
  CHECK(s.erv.supply == 9 && s.erv.exhaust == 5 && s.erv.ashrae == 4);
  CHECK(s.relay_ins() == 1);
  s.run_ms(6000);
  CHECK(s.flash.settings.supply == 9 && s.flash.settings.ashrae == 4);
  CHECK(s.texts[Out::SUPPLY_SELECT] == "140 CFM");
}

TEST(settings_while_off_only_change_power_on) {
  Sim s;
  steady(s);
  s.core->request_power(false);
  s.run_ms(3000);
  s.core->request_exhaust(13);
  s.run_ms(3000);
  CHECK(s.relay_ins() == 1);  // only the Power off
  CHECK(s.flash.settings.exhaust == 13);
  s.core->request_power(true);
  s.run_ms(3000);
  CHECK(s.erv.on && s.erv.exhaust == 13);
}

TEST(learns_panel_changes) {
  Sim s;
  steady(s);
  s.wc.press_supply(11);
  s.run_ms(8000);
  CHECK(s.erv.supply == 11);
  CHECK(s.flash.settings.supply == 11);
  s.wc.press_power(false);
  s.run_ms(3000);
  CHECK(!s.flash.want_on);
  CHECK(s.relay_ins() == 0);
}

TEST(requests_need_a_known_erv) {
  Sim s;
  s.mains_on();
  s.erv_running(5, 5, 3);
  s.erv.deaf = true;
  s.board_boot();
  s.erv.powered = false;  // no state line at all
  s.run_ms(2000);
  s.core->request_power(false);
  s.run_ms(5000);
  CHECK(s.relay_ins() == 0);
}

TEST(intercept_test_sends_only_idle) {
  Sim s;
  steady(s);
  s.core->request_test();
  s.run_ms(3000);
  CHECK(s.relay_ins() == 1);
  CHECK(s.erv.commands == 0);
  CHECK(s.erv.on && s.erv.supply == 5);
}

TEST(relay_switches_only_between_wall_control_frames) {
  // Many intercepts at different phases of the two cycles.
  Sim s;
  steady(s);
  for (int i = 0; i < 20; i++) {
    s.core->request_supply(i % 2 ? 5 : 9);
    s.run_ms(2000 + 37 * i);
  }
  CHECK(s.relay_ins() == 20);
  CHECK(s.relay_in_wc_frame == 0);
  CHECK(s.longest_intercept < 2000 * MS);
  CHECK(s.core->stats(RXD).bad == 0);
}

TEST(intercept_without_a_shared_gap_uses_an_rxd_gap_after_30_s) {
  // The capture's phase: no shared gap. The ERV still gets clean frames.
  Sim s;
  s.flash = Flash{true, {5, 5, 3}};
  s.mains_on(48);
  s.run_ms(3000);
  s.erv_running(5, 5, 3);
  s.board_boot();
  s.run_ms(5000);
  s.core->request_power(false);
  s.run_ms(25000);
  CHECK(s.relay_ins() == 0);
  s.run_ms(10000);
  CHECK(s.relay_ins() == 1);
  CHECK(!s.erv.on);
  CHECK(s.relay_in_wc_frame == 0);
}


// ---- Decoder details ----

TEST(truncated_frame_resyncs) {
  Direct d;
  auto f = make_frame("01", std::vector<uint8_t>{1, 5, 5, 0}.data());
  d.bytes(TXD, std::vector<uint8_t>(f.begin(), f.begin() + 6));  // cut short
  d.frame(TXD, "01", {1, 5, 5, 0});
  CHECK(d.core->stats(TXD).bad == 1);
  CHECK(d.core->stats(TXD).frames == 1);
  CHECK(d.values[Out::POWER] == 1);
}

TEST(junk_between_frames_is_counted) {
  Direct d;
  d.bytes(RXD, {0x00, 0xFF, 0x41});
  d.frame(RXD, "00", {0xFF, 0xFF, 0xFF, 0});
  CHECK(d.core->stats(RXD).junk == 3);
  CHECK(d.core->stats(RXD).frames == 1);
}

TEST(unknown_registers_are_reported_once_per_change) {
  Direct d;
  for (int i = 0; i < 5; i++) d.frame(TXD, "41", {1, 2, 3, 4});
  d.frame(TXD, "41", {1, 2, 3, 5});
  d.frame(RXD, "01", {0, 0, 0, 0});  // a state address on the command line
  auto &u = d.texts[Out::UNKNOWN_REG];
  CHECK(u.size() == 3);
  CHECK(u.size() == 3 && u[0] == "TXD 41 10203040" && u[1] == "TXD 41 10203050" && u[2] == "RXD 01 00000000");
}

TEST(ashrae_shows_when_11_arrives_before_01) {
  // At boot the board can start listening mid-cycle (fixed in v14).
  Direct d;
  d.frame(TXD, "11", {3, 0, 0, 0});
  d.frame(TXD, "01", {1, 5, 5, 0});
  CHECK(d.values[Out::ASHRAE] == 30);
}

TEST(sub_zero_temperatures) {
  Sim s;
  s.erv.b1[0] = 21;
  s.erv.b1[2] = (uint8_t) -12;
  steady(s);
  s.run_ms(70000);
  CHECK(s.value(Out::OUTSIDE_TEMP) == -12);
  CHECK(s.value(Out::INSIDE_TEMP) == 21);
}

// ---- The ERV's own timers ----

TEST(ashrae_rest_cycle) {
  // 30 min/h with a 4 s ERV minute: runs 120 s, rests 120 s.
  Sim s;
  s.erv.minute = 4000 * MS;
  steady(s);  // the cycle started 20 s ago
  s.run_ms(90000);
  CHECK(s.value(Out::VENTILATING) == 1);
  CHECK(s.value(Out::INSIDE_TEMP) == 19);
  int writes = s.flash.settings_writes;
  s.run_ms(20000);  // resting
  CHECK(s.value(Out::POWER) == 1);
  CHECK(s.value(Out::VENTILATING) == 0);
  CHECK(s.value(Out::SUPPLY_CFM) == 0);
  CHECK(std::isnan(s.value(Out::INSIDE_TEMP)));
  CHECK(s.texts[Out::SUPPLY_SELECT] == "100 CFM");
  CHECK(s.flash.settings_writes == writes);  // FE is not a setting
  CHECK(s.flash.want_on);
  s.run_ms(130000);  // running again for ~20 s
  CHECK(s.value(Out::VENTILATING) == 1);
  CHECK(s.value(Out::SUPPLY_CFM) == 100);
  CHECK(std::isnan(s.value(Out::INSIDE_TEMP)));  // not for the first minute
  s.run_ms(50000);
  CHECK(s.value(Out::INSIDE_TEMP) == 19);
  CHECK(s.relay_ins() == 0);
}

TEST(boost_overrides_a_rest) {
  Sim s;
  s.erv.minute = 4000 * MS;
  steady(s);
  s.run_ms(110000);
  CHECK(s.value(Out::VENTILATING) == 0);
  s.core->request_boost(true);
  s.run_ms(3000);
  CHECK(s.erv.boost);
  CHECK(s.value(Out::VENTILATING) == 1);
  CHECK(s.value(Out::SUPPLY_CFM) == 200);
}

TEST(boost_ends_by_itself) {
  // A 1 s ERV minute: boost lasts 60 s.
  Sim s;
  s.erv.minute = 1000 * MS;
  steady(s);
  s.core->request_boost(true);
  s.run_ms(3000);
  CHECK(s.value(Out::BOOST) == 1 && s.value(Out::BOOST_SWITCH) == 1);
  s.run_ms(62000);
  CHECK(!s.erv.boost);
  CHECK(s.value(Out::BOOST) == 0 && s.value(Out::BOOST_SWITCH) == 0);
  CHECK(std::isnan(s.value(Out::BOOST_MIN)));
  CHECK(s.value(Out::SUPPLY_CFM) == 100);
  CHECK(s.relay_ins() == 1);
  CHECK(s.flash.settings.supply == 5);  // boost's 200 CFM never saved
  s.core->request_boost(false);  // already over: nothing to send
  s.run_ms(3000);
  CHECK(s.relay_ins() == 1);
}

TEST(airflow_change_while_boosting_is_refused) {
  Sim s;
  steady(s);
  s.core->request_boost(true);
  s.run_ms(3000);
  s.core->request_supply(9);
  s.run_ms(3000);
  CHECK(s.relay_ins() == 1);
  CHECK(s.texts[Out::SUPPLY_SELECT] == "200 CFM");
}

TEST(boost_before_ashrae_is_known_keeps_the_setting) {
  Sim s;
  steady(s);
  s.board_off();
  s.run_ms(1000);
  s.board_boot();
  // Request as soon as the power state is known, before 11 is decoded.
  s.values.clear();
  while (std::isnan(s.value(Out::POWER))) s.run_ms(1);
  CHECK(std::isnan(s.value(Out::ASHRAE)));
  s.core->request_boost(true);
  s.run_ms(3000);
  CHECK(s.erv.boost);
  CHECK(s.erv.ashrae == 3);
}

// ---- Regressions from the test-gap review (v15) ----

TEST(wall_control_restart_then_panel_off_stays_off) {
  // The burst starts a resume while the ERV keeps running; 01 never changes.
  Sim s;
  steady(s);
  s.wall_control_reset();
  s.run_ms(4000);
  s.panel_power(false);
  s.run_ms(60000);
  CHECK(!s.erv.on);
  CHECK(!s.flash.want_on);
  CHECK(s.relay_ins() == 0);
  CHECK(!s.core->resuming());
}

TEST(rxd_glitch_then_power_off_stays_off) {
  Sim s;
  steady(s);
  s.tap_dead[RXD] = true;
  s.run_ms(700);
  s.tap_dead[RXD] = false;
  s.run_ms(1000);
  s.core->request_power(false);
  s.run_ms(60000);
  CHECK(!s.erv.on);
  CHECK(!s.flash.want_on);
  CHECK(s.relay_ins() == 1);
}

TEST(brownout_as_the_relay_goes_in_is_bounded) {
  for (uint32_t listen : {300u, 2500u}) {
    Sim s;
    steady(s);
    s.brownout_on_relay = true;
    s.brownout_listen_ms = listen;
    s.mains_off();
    s.run_ms(30000);
    s.mains_on();
    s.run_ms(15000);
    CHECK(s.resets == 1);
    CHECK(s.flash.want_on);  // the reset didn't record the outage's "off"
    CHECK(s.core && s.core->resuming());
    s.run_ms(600000);
    CHECK(s.relay_ins() == 3);
    CHECK(s.resets == 3);
    CHECK(!s.core->resuming());
    CHECK(s.flash.resume_mark == 0);
  }
}

TEST(resume_mark_is_cleared_after_a_normal_resume) {
  Sim s;
  steady(s);
  s.mains_off();
  s.run_ms(30000);
  s.mains_on();
  s.run_ms(20000);
  CHECK(s.erv.on);
  CHECK(s.flash.resume_mark == 0);
  CHECK(s.flash.writes_while_relay_in == 0);
}

// ---- Resume paths ----

TEST(resumes_after_a_short_blip_with_the_whole_burst_lost) {
  Sim s;
  steady(s);
  s.mains_off();
  s.run_ms(3000);
  s.tap_dead[RXD] = true;
  s.mains_on();
  s.run_ms(600);
  s.tap_dead[RXD] = false;
  s.run_ms(15000);
  CHECK(s.erv.on);
  CHECK(s.relay_ins() == 1);
  CHECK(logged(s, "wall control silent while the ERV talks"));
}

TEST(resumes_at_the_capture_phase) {
  // No shared gap: the try waits 30 s, then uses an RXD gap.
  Sim s;
  steady(s);
  s.mains_off();
  s.run_ms(30000);
  s.mains_on(48);
  s.run_ms(50000);
  CHECK(s.erv.on);
  CHECK(s.relay_ins() == 1);
  CHECK(s.relay_in_wc_frame == 0);
}

TEST(resume_ends_if_the_erv_never_reports) {
  Sim s;
  steady(s);
  s.mains_off();
  s.run_ms(30000);
  s.mains_on();
  s.erv.powered = false;
  s.run_ms(130000);
  CHECK(!s.core->resuming());
  CHECK(logged(s, "no state from the ERV"));
  CHECK(s.relay_ins() == 0);
  CHECK(s.flash.want_on);  // nothing learned
}

TEST(a_refused_try_is_not_counted) {
  Sim s;
  steady(s);
  s.mains_off();
  s.run_ms(30000);
  s.mains_on();
  s.run_ms(9000);
  s.tap_dead[RXD] = true;  // the first try is refused: no live traffic
  s.run_ms(3000);
  s.tap_dead[RXD] = false;
  s.run_ms(40000);
  CHECK(logged(s, "refused: no live traffic"));
  CHECK(s.erv.on);
  CHECK(s.relay_ins() == 1);
}

TEST(resume_gives_up_after_5_min_of_refused_tries) {
  Sim s;
  steady(s);
  s.mains_off();
  s.run_ms(30000);
  s.mains_on();
  s.run_ms(1000);
  s.tap_dead[RXD] = true;
  s.run_ms(320000);
  CHECK(logged(s, "gave up"));
  CHECK(s.relay_ins() == 0);
  CHECK(!s.erv.on);
}

// ---- Timing ----

TEST(controls_work_at_every_phase) {
  // Sweep the two lines' relative phase over a whole cycle.
  for (uint32_t erv_ms = 0; erv_ms < 200; erv_ms += 20) {
    Sim s;
    steady(s, erv_ms);
    s.core->request_power(false);
    s.run_ms(40000);
    CHECK(!s.erv.on);
    s.core->request_power(true);
    s.run_ms(40000);
    CHECK(s.erv.on);
    CHECK(s.relay_ins() == 2);
    CHECK(s.relay_in_wc_frame == 0);
    CHECK(s.longest_intercept < 2000 * MS);
  }
}

TEST(loop_stalls_dont_break_relay_timing) {
  Sim s;
  steady(s);
  uint32_t seed = 12345;
  for (int i = 0; i < 30; i++) {
    s.core->request_supply(i % 2 ? 5 : 9);
    for (int k = 0; k < 50; k++) {
      s.run_ms(50);
      seed = seed * 1103515245 + 12345;
      s.stall((seed >> 16) % 40);
    }
  }
  s.run_ms(3000);
  CHECK(s.relay_ins() == 30);
  CHECK(s.relay_in_wc_frame == 0);
  CHECK(s.erv.supply == 5);
  CHECK(s.longest_intercept < 2000 * MS);
}

TEST(a_stall_hiding_a_frame_end_doesnt_mislead_the_relay) {
  // While the board waits for a gap, the loop stalls from just before the wall control's
  // last frame (F0) ends until about when the next cycle starts, so it sees F0 complete
  // ~50 ms late and could think the 51 ms gap has just begun. Swept over the lines' phase so
  // that, somewhere, the ERV's own gap doesn't stop it.
  for (uint32_t erv_ms = 0; erv_ms < 200; erv_ms += 5)
    for (uint32_t stall = 46; stall <= 54; stall += 2) {
      Sim s;
      s.flash = Flash{true, {5, 5, 3}};
      s.mains_on(erv_ms);
      s.run_ms(2500);
      s.erv_running(5, 5, 3);
      s.board_boot();
      s.run_ms(1500);
      s.core->request_test();
      s.run_ms(1);
      while (!(s.wc_frames.size() && s.wc_frames.back().first <= s.t &&
               s.wc_frames.back().second > s.t + 1 * MS && s.wc_frames.back().second <= s.t + 3 * MS))
        s.run_ms(1);
      if (s.relay_ins()) continue;  // already went in
      s.stall(stall);
      s.run_ms(1000);
      CHECK(s.relay_in_wc_frame == 0);
    }
}

TEST(settings_are_not_saved_while_the_relay_is_in) {
  // A panel change, then an intercept just as the 5 s settle ends.
  for (uint32_t ms = 4000; ms <= 5600; ms += 100) {
    Sim s;
    steady(s);
    s.wc.press_supply(9);
    s.run_ms(ms);
    s.core->request_test();
    s.run_ms(10000);
    CHECK(s.flash.settings.supply == 9);
    CHECK(s.flash.writes_while_relay_in == 0);
  }
}

TEST(relay_is_forced_out_after_5_s) {
  Sim s;
  steady(s);
  s.core->request_power(false);
  while (!s.relay) s.run_ms(1);
  s.stall(6000);  // the loop hangs with the relay in
  s.run_ms(7000);
  CHECK(!s.relay);
  CHECK(logged(s, "relay forced out"));
}

TEST(survives_millis_wraparound) {
  Sim s;
  s.clock_offset = UINT32_MAX - 40000;  // wraps 40 s after boot
  steady(s);
  s.mains_off();
  s.run_ms(30000);
  s.mains_on();
  s.run_ms(20000);
  CHECK(s.erv.on);
  CHECK(s.relay_ins() == 1);
  s.core->request_supply(9);
  s.run_ms(10000);
  CHECK(s.erv.supply == 9);
  CHECK(s.flash.settings.supply == 9);
  s.run_ms(70000);
  CHECK(s.value(Out::INSIDE_TEMP) == 19);
}

TEST(no_flash_writes_in_steady_running) {
  Sim s;
  s.erv.minute = 4000 * MS;
  steady(s);
  int w = s.flash.want_writes, st = s.flash.settings_writes, r = s.flash.resume_writes;
  s.core->request_boost(true);
  s.run_ms(1200000);  // 20 min: rest cycles, boost start and end (at 4 s per ERV minute)
  CHECK(s.flash.want_writes == w);
  CHECK(s.flash.settings_writes == st);
  CHECK(s.flash.resume_writes == r);
  CHECK(s.flash.writes_while_relay_in == 0);
}

int main() {
  bool verbose = getenv("VERBOSE") != nullptr;
  int failed_tests = 0;
  for (auto &t : tests()) {
    int before = failures;
    test_log().clear();
    t.fn();
    bool ok = failures == before;
    printf("%s %s\n", ok ? "ok  " : "FAIL", t.name);
    if (!ok || verbose)
      for (auto &l : test_log()) printf("      %s\n", l.c_str());
    failed_tests += !ok;
  }
  printf("\n%zu tests, %d failed\n", tests().size(), failed_tests);
  return failed_tests != 0;
}
