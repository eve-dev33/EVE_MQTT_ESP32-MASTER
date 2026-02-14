#include <assert.h>
#include <iostream>
#include <string>

#include "power_schedule_core.h"

void test_validation() {
  PowerRelaySchedule s{};
  std::string err;
  assert(parseScheduleJson("[{\"at\":\"07:30\",\"state\":\"ON\",\"days\":\"1111111\"}]", s, err));
  assert(s.count == 1);
  assert(!parseScheduleJson("[{\"at\":\"25:30\",\"state\":\"ON\",\"days\":\"1111111\"}]", s, err));
  assert(!parseScheduleJson("[{\"at\":\"07:30\",\"state\":\"X\",\"days\":\"1111111\"}]", s, err));
}

void test_json_to_packet() {
  PowerRelaySchedule s{};
  std::string err;
  assert(parseScheduleJson("[{\"at\":\"06:10\",\"state\":\"OFF\",\"days\":\"1010101\"}]", s, err));
  PowerRelayRulesPacket pkt{};
  assert(buildRulesPacket(2, s, 1234, pkt));
  assert(pkt.type == 14 && pkt.ch == 2 && pkt.count == 1);
  assert(pkt.rules[0].hh == 6 && pkt.rules[0].mm == 10 && pkt.rules[0].state == 0);
}

void test_ack_and_executed_structs() {
  PowerScheduleAckPacket ack{15, 1, 1, 2, 777};
  assert(ack.type == 15 && ack.ok == 1);
  PowerExecutedPacket ex{16, 3, 1, 480, 0, 999};
  assert(ex.type == 16 && ex.state == 1 && ex.minuteOfDay == 480);
}

void test_retained_payload_shape() {
  PowerRelaySchedule s{};
  std::string err;
  assert(parseScheduleJson("[{\"at\":\"09:05\",\"state\":\"ON\",\"days\":\"1111100\"}]", s, err));
  std::string out = scheduleToJson(s);
  assert(out.find("09:05") != std::string::npos);
  assert(out.find("1111100") != std::string::npos);
}

int main() {
  test_validation();
  test_json_to_packet();
  test_ack_and_executed_structs();
  test_retained_payload_shape();
  std::cout << "All schedule tests passed\n";
  return 0;
}
