#ifndef NTPUTIL_H
#define NTPUTIL_H

// Function declarations
int sync_sbitx_time(const char* ntp_server);
void rtc_write_ntp(int year, int month, int day, int hours, int minutes, int seconds);
void rtc_read();
#endif  // NTPUTIL_H

