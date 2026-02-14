#pragma once

#include <stdint.h>
#include <string>

static const uint8_t POWER_MAX_SCHEDULE_RULES = 10;

struct PowerScheduleRule {
  uint8_t hh;
  uint8_t mm;
  uint8_t state;    // 0 OFF, 1 ON
  uint8_t daysMask; // bit0 Mon ... bit6 Sun
};

struct PowerRelaySchedule {
  uint8_t count;
  PowerScheduleRule rules[POWER_MAX_SCHEDULE_RULES];
};

struct PowerRelayRulesPacket {
  uint8_t type;   // 14
  uint8_t ch;     // 1..3
  uint8_t count;  // 0..10
  PowerScheduleRule rules[POWER_MAX_SCHEDULE_RULES];
  uint32_t ms;
};

struct PowerScheduleAckPacket {
  uint8_t type;   // 15
  uint8_t ch;
  uint8_t ok;
  uint8_t count;
  uint32_t ms;
};

struct PowerExecutedPacket {
  uint8_t type;         // 16
  uint8_t ch;
  uint8_t state;
  uint16_t minuteOfDay;
  uint8_t weekdayMon0;
  uint32_t ms;
};

bool parseScheduleJson(const std::string &json, PowerRelaySchedule &out, std::string &error);
std::string scheduleToJson(const PowerRelaySchedule &schedule);
bool schedulesEqual(const PowerRelaySchedule &a, const PowerRelaySchedule &b);
bool buildRulesPacket(uint8_t relay, const PowerRelaySchedule &schedule, uint32_t nowMs, PowerRelayRulesPacket &out);

