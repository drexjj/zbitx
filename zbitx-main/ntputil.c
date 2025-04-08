#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "ntputil.h"
#include <time.h>
#include <wiringPi.h>
#include "sdr_ui.h"
#include  "i2cbb.h"
// This is a simple NTP client implimentation for the sBitx that will check the current computer time against the defined ntp server pool and adjust 
// the local time on the machine accordingly if it is +/- 1 second out of sync.
// 6/30/24 W2JON

static uint32_t time_delta = 0;
#define DS3231_I2C_ADD 0x68
#define NTP_TIMESTAMP_DELTA (2208988800ull)

#pragma pack(1)
struct ntp_packet {
    uint8_t li_vn_mode;
    uint8_t stratum;
    uint8_t poll;
    uint8_t precision;
    uint32_t rootDelay;
    uint32_t rootDispersion;
    uint32_t refId;
    uint32_t refTm_s;
    uint32_t refTm_f;
    uint32_t origTm_s;
    uint32_t origTm_f;
    uint32_t rxTm_s;
    uint32_t rxTm_f;
    uint32_t txTm_s;
    uint32_t txTm_f;
};
#pragma pack(0)


static uint8_t dec2bcd(uint8_t val){
	return ((val/10 * 16) + (val %10));
}

static uint8_t bcd2dec(uint8_t val){
	return ((val/16 * 10) + (val %16));
}

time_t time_sbitx(){
	if (!time_delta)
		return time(NULL);
	else
		return time_delta + (long)(millis()/1000l);
}

void rtc_write_ntp(int year, int month, int day, int hours, int minutes, int seconds){
	uint8_t rtc_time[10];

	rtc_time[0] = dec2bcd(seconds);
	rtc_time[1] = dec2bcd(minutes);
	rtc_time[2] = dec2bcd(hours);
	rtc_time[3] = 0;
	rtc_time[4] = dec2bcd(day);
	rtc_time[5] = dec2bcd(month);
	rtc_time[6] = dec2bcd(year - 2000);

	printf("Updating the RTC with network time\n");
	
	for (uint8_t i = 0; i < 7; i++){
  	int e = i2cbb_write_byte_data(DS3231_I2C_ADD, i, rtc_time[i]);
		if (e)
			printf("rtc_write: error writing DS3231 register at %d index\n", i);
	}
}

void rtc_read(){
	uint8_t rtc_time[10];
	char buff[100];
	struct tm t;
	time_t gm_now;

	i2cbb_write_i2c_block_data(DS3231_I2C_ADD, 0, 0, NULL);

	int e =  i2cbb_read_i2c_block_data(DS3231_I2C_ADD, 0, 8, rtc_time);
	if (e <= 0){
		printf("RTC not detected\n");
		//go with the system time
		time_delta = 0; // this forces time_sbitx() to return the system time
		return;
	}
	for (int i = 0; i < 7; i++)
		rtc_time[i] = bcd2dec(rtc_time[i]);


	t.tm_year 	= rtc_time[6] + 2000;
	t.tm_mon 	= rtc_time[5];
	t.tm_mday 	= rtc_time[4];
	t.tm_hour 	= rtc_time[2];
	t.tm_min		= rtc_time[1];
	t.tm_sec		= rtc_time[0];		

	printf("RTC read as %d/%d/%d %02d:%02d:%02d\n",
		t.tm_year, t.tm_mon, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);

	//convert to julian
	t.tm_year -= 1900;
	t.tm_mon -= 1;
	setenv("TZ", "UTC", 1);	
	gm_now = mktime(&t);
	time_delta =(long)gm_now -(((long)millis())/1000l);
}

long getaddress(const char* host) {
    int i, dotcount = 0;
    char* p = (char*)host;
    struct hostent* pent;

    while (*p) {
        for (i = 0; i < 3; i++, p++)
            if (!isdigit(*p))
                break;
        if (*p != '.')
            break;
        p++;
        dotcount++;
    }

    if (dotcount == 3 && i > 0 && i <= 3)
        return inet_addr(host);

    pent = gethostbyname(host);
    if (!pent)
        return 0;

    return *((long*)(pent->h_addr));
}

int ntp_request(const char* ntp_server) {
    struct sockaddr_in addr;
    int retryAfter = 500, i, len, ret;
    int nretries = 10;
    int sock;
    struct timeval tv;
    fd_set fd;
    struct ntp_packet reply;

    printf("Resolving NTP server at %s\n", ntp_server);
    uint32_t address = getaddress(ntp_server);
    if (!address) {
        printf("NTP server is not reachable right now\n");
        return -1;
    }

    struct ntp_packet request;
    memset(&request, 0, sizeof(struct ntp_packet));
    request.li_vn_mode = 0x1b;

    sock = (int)socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
    addr.sin_addr.s_addr = address;
    addr.sin_port = htons(123);
    addr.sin_family = AF_INET;

    ret = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
    if (ret < 0) {
        perror("connect");
        return -1;
    }

    ret = send(sock, &request, sizeof(struct ntp_packet), 0);
    if (ret < 0) {
        perror("send");
        close(sock);
        return -1;
    }

    tv.tv_sec = retryAfter / 1000;
    tv.tv_usec = (retryAfter % 1000) * 1000;
    FD_ZERO(&fd);
    FD_SET(sock, &fd);

    ret = select(sock + 1, &fd, NULL, NULL, &tv);
    if (ret <= 0) {
        puts("Timeout or select error\n");
        close(sock);
        return -1;
    }

    len = sizeof(addr);
    ret = recv(sock, &reply, sizeof(struct ntp_packet), 0);
    if (ret <= 0) {
        puts("recvfrom error\n");
        close(sock);
        return -1;
    }

    close(sock);

    reply.txTm_s = ntohl(reply.txTm_s);
    reply.txTm_f = ntohl(reply.txTm_f);
    time_t txTm = (time_t)(reply.txTm_s - NTP_TIMESTAMP_DELTA);
    struct tm* utc = gmtime(&txTm);

		char buff[200];
    sprintf(buff, "Network Time: %d-%d-%d %02d:%02d:%02d\n", 
			utc->tm_year + 1900, utc->tm_mon + 1, utc->tm_mday,
       utc->tm_hour, utc->tm_min, utc->tm_sec);

		write_console(FONT_LOG, buff);
		rtc_write_ntp(utc->tm_year + 1900, utc->tm_mon + 1, utc->tm_mday,
       	utc->tm_hour, utc->tm_min, utc->tm_sec);
    return txTm;
}

int sync_sbitx_time(const char* ntp_server) {
  time_t current_time;
  time(&current_time);  // Get current system time

  time_t ntp_time = ntp_request(ntp_server);
  if (ntp_time == -1) {
    return - 1;
  }
	printf("Time synchronized with the network.\n");
	return 0;
}
