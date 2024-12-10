/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <gtest/gtest.h>
#include <iostream>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wwrite-strings"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wformat"
#pragma GCC diagnostic ignored "-Wint-to-pointer-cast"
#pragma GCC diagnostic ignored "-Wpointer-arith"

#include "os.h"
#include "tlog.h"

TEST(osTimeTests, taosLocalTime) {
  // Test 1: Test when both timep and result are not NULL
  time_t     timep = 1617531000;  // 2021-04-04 18:10:00
  struct tm  result;
  struct tm* local_time = taosLocalTime(&timep, &result, NULL, 0, NULL);
  ASSERT_NE(local_time, nullptr);
  ASSERT_EQ(local_time->tm_year, 121);
  ASSERT_EQ(local_time->tm_mon, 3);
  ASSERT_EQ(local_time->tm_mday, 4);
  ASSERT_EQ(local_time->tm_hour, 18);
  ASSERT_EQ(local_time->tm_min, 10);
  ASSERT_EQ(local_time->tm_sec, 00);

  // Test 2: Test when timep is NULL
  local_time = taosLocalTime(NULL, &result, NULL, 0, NULL);
  ASSERT_EQ(local_time, nullptr);

  // Test 4: Test when timep is negative on Windows
#ifdef WINDOWS
  time_t pos_timep = 1609459200;  // 2021-01-01 08:00:00
  local_time = taosLocalTime(&pos_timep, &result, NULL, 0, NULL);
  ASSERT_NE(local_time, nullptr);
  ASSERT_EQ(local_time->tm_year, 121);
  ASSERT_EQ(local_time->tm_mon, 0);
  ASSERT_EQ(local_time->tm_mday, 1);
  ASSERT_EQ(local_time->tm_hour, 8);
  ASSERT_EQ(local_time->tm_min, 0);
  ASSERT_EQ(local_time->tm_sec, 0);

  time_t neg_timep = -1617531000;  // 1918-09-29 21:50:00
  local_time = taosLocalTime(&neg_timep, &result, NULL, 0, NULL);
  ASSERT_NE(local_time, nullptr);
  ASSERT_EQ(local_time->tm_year, 18);
  ASSERT_EQ(local_time->tm_mon, 8);
  ASSERT_EQ(local_time->tm_mday, 29);
  ASSERT_EQ(local_time->tm_hour, 21);
  ASSERT_EQ(local_time->tm_min, 50);
  ASSERT_EQ(local_time->tm_sec, 0);

  time_t neg_timep2 = -315619200;  // 1960-01-01 08:00:00
  local_time = taosLocalTime(&neg_timep2, &result, NULL, 0, NULL);
  ASSERT_NE(local_time, nullptr);
  ASSERT_EQ(local_time->tm_year, 60);
  ASSERT_EQ(local_time->tm_mon, 0);
  ASSERT_EQ(local_time->tm_mday, 1);
  ASSERT_EQ(local_time->tm_hour, 8);
  ASSERT_EQ(local_time->tm_min, 0);
  ASSERT_EQ(local_time->tm_sec, 0);

  time_t zero_timep = 0;  // 1970-01-01 08:00:00
  local_time = taosLocalTime(&zero_timep, &result, NULL, 0, NULL);
  ASSERT_NE(local_time, nullptr);
  ASSERT_EQ(local_time->tm_year, 70);
  ASSERT_EQ(local_time->tm_mon, 0);
  ASSERT_EQ(local_time->tm_mday, 1);
  ASSERT_EQ(local_time->tm_hour, 8);
  ASSERT_EQ(local_time->tm_min, 0);
  ASSERT_EQ(local_time->tm_sec, 0);

  time_t neg_timep3 = -78115158887;
  local_time = taosLocalTime(&neg_timep3, &result, NULL, 0, NULL);
  ASSERT_EQ(local_time, nullptr);
#endif
}