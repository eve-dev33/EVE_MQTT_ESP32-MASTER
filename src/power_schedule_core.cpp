#include "power_schedule_core.h"

#include <ctype.h>
#include <sstream>

namespace {

void skipWs(const std::string &s, size_t &i) {
  while (i < s.size() && isspace((unsigned char)s[i])) i++;
}

bool expect(const std::string &s, size_t &i, char c) {
  skipWs(s, i);
  if (i >= s.size() || s[i] != c) return false;
  i++;
  return true;
}

bool parseQuoted(const std::string &s, size_t &i, std::string &out) {
  skipWs(s, i);
  if (i >= s.size() || s[i] != '"') return false;
  i++;
  out.clear();
  while (i < s.size() && s[i] != '"') {
    out.push_back(s[i]);
    i++;
  }
  if (i >= s.size()) return false;
  i++;
  return true;
}

bool parseRuleObject(const std::string &s, size_t &i, PowerScheduleRule &rule, std::string &error) {
  if (!expect(s, i, '{')) {
    error = "Expected '{'";
    return false;
  }

  bool hasAt = false, hasState = false, hasDays = false;
  std::string at, state, days;

  while (true) {
    std::string key, value;
    if (!parseQuoted(s, i, key)) {
      error = "Expected key";
      return false;
    }
    if (!expect(s, i, ':')) {
      error = "Expected ':' after key";
      return false;
    }
    if (!parseQuoted(s, i, value)) {
      error = "Expected quoted value";
      return false;
    }

    if (key == "at") {
      at = value;
      hasAt = true;
    } else if (key == "state") {
      state = value;
      hasState = true;
    } else if (key == "days") {
      days = value;
      hasDays = true;
    }

    skipWs(s, i);
    if (i >= s.size()) {
      error = "Unexpected end object";
      return false;
    }
    if (s[i] == '}') {
      i++;
      break;
    }
    if (s[i] != ',') {
      error = "Expected ',' in object";
      return false;
    }
    i++;
  }

  if (!hasAt || !hasState || !hasDays) {
    error = "Missing at/state/days";
    return false;
  }

  if (at.size() != 5 || at[2] != ':') {
    error = "Invalid at format";
    return false;
  }
  int hh = (at[0] - '0') * 10 + (at[1] - '0');
  int mm = (at[3] - '0') * 10 + (at[4] - '0');
  if (!isdigit(at[0]) || !isdigit(at[1]) || !isdigit(at[3]) || !isdigit(at[4])) {
    error = "Invalid at digits";
    return false;
  }
  if (hh < 0 || hh > 23 || mm < 0 || mm > 59) {
    error = "at out of range";
    return false;
  }

  uint8_t st = 0;
  if (state == "ON") st = 1;
  else if (state == "OFF") st = 0;
  else {
    error = "Invalid state";
    return false;
  }

  if (days.size() != 7) {
    error = "days length must be 7";
    return false;
  }
  uint8_t mask = 0;
  for (size_t d = 0; d < 7; d++) {
    if (days[d] == '1') mask |= (1 << d);
    else if (days[d] != '0') {
      error = "days must be binary";
      return false;
    }
  }

  rule.hh = (uint8_t)hh;
  rule.mm = (uint8_t)mm;
  rule.state = st;
  rule.daysMask = mask;
  return true;
}

} // namespace

bool parseScheduleJson(const std::string &json, PowerRelaySchedule &out, std::string &error) {
  out.count = 0;
  size_t i = 0;
  if (!expect(json, i, '[')) {
    error = "Expected '['";
    return false;
  }

  skipWs(json, i);
  if (i < json.size() && json[i] == ']') {
    i++;
    return true;
  }

  while (true) {
    if (out.count >= POWER_MAX_SCHEDULE_RULES) {
      error = "Too many rules (max 10)";
      return false;
    }

    if (!parseRuleObject(json, i, out.rules[out.count], error)) return false;
    out.count++;

    skipWs(json, i);
    if (i >= json.size()) {
      error = "Unexpected end array";
      return false;
    }
    if (json[i] == ']') {
      i++;
      break;
    }
    if (json[i] != ',') {
      error = "Expected ',' between rules";
      return false;
    }
    i++;
  }

  skipWs(json, i);
  if (i != json.size()) {
    error = "Trailing chars";
    return false;
  }
  return true;
}

std::string scheduleToJson(const PowerRelaySchedule &schedule) {
  std::ostringstream oss;
  oss << "[";
  for (uint8_t i = 0; i < schedule.count; i++) {
    const PowerScheduleRule &r = schedule.rules[i];
    if (i) oss << ",";
    char at[8];
    snprintf(at, sizeof(at), "%02d:%02d", (int)r.hh, (int)r.mm);
    char days[8];
    for (int d = 0; d < 7; d++) days[d] = ((r.daysMask >> d) & 1) ? '1' : '0';
    days[7] = '\0';
    oss << "{\"at\":\"" << at << "\",\"state\":\"" << (r.state ? "ON" : "OFF") << "\",\"days\":\"" << days << "\"}";
  }
  oss << "]";
  return oss.str();
}

bool schedulesEqual(const PowerRelaySchedule &a, const PowerRelaySchedule &b) {
  if (a.count != b.count) return false;
  for (uint8_t i = 0; i < a.count; i++) {
    if (a.rules[i].hh != b.rules[i].hh || a.rules[i].mm != b.rules[i].mm ||
        a.rules[i].state != b.rules[i].state || a.rules[i].daysMask != b.rules[i].daysMask) {
      return false;
    }
  }
  return true;
}

bool buildRulesPacket(uint8_t relay, const PowerRelaySchedule &schedule, uint32_t nowMs, PowerRelayRulesPacket &out) {
  if (relay < 1 || relay > 3 || schedule.count > POWER_MAX_SCHEDULE_RULES) return false;
  out.type = 14;
  out.ch = relay;
  out.count = schedule.count;
  for (uint8_t i = 0; i < POWER_MAX_SCHEDULE_RULES; i++) {
    out.rules[i] = schedule.rules[i];
  }
  out.ms = nowMs;
  return true;
}
