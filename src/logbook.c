#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h> 
#include <math.h>
#include <complex.h>
#include <fftw3.h>
#include <unistd.h>
#include <wiringPi.h>
#include <wiringSerial.h>
#include <linux/types.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include <stdbool.h>
#include <sys/types.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <ctype.h>
#include <arpa/inet.h>
#include "sdr.h"
#include "sdr_ui.h"
#include "logbook.h"

#include <sqlite3.h>

static int rc;
static sqlite3 *db=NULL;
/* writes the output to data/result_rows.txt
	if the from_id is negative, it returns the later 50 records (higher id)
	if the from_id is positive, it returns the prior 50 records (lower id) */

int logbook_query(char *query, int from_id, char *result_file){
	sqlite3_stmt *stmt;
	char statement[200], json[10000], param[2000];

	if (db == NULL)
		logbook_open();

	//add to the bottom of the logbook
	if (from_id > 0){
		if (query)
			sprintf(statement, "select * from logbook "
				"where (callsign_recv LIKE '%s%%' AND id < %d) ",
				query, from_id);
		else
			sprintf(statement, "select * from logbook where id < %d ", from_id);
	}
	//last 50 QSOs
	else if (from_id == 0){
		if (query)
			sprintf(statement, "select * from logbook "
				"where callsign_recv LIKE '%s%%' ", query);
		else
			strcpy(statement, "select * from logbook ");
	}
	//latest QSOs after from_id (top of the log)
	else {
		if (query)
			sprintf(statement, "select * from logbook "
				"where (callsign_recv LIKE '%s%%' AND id > %d) ",
				query, -from_id);
		else 
			sprintf(statement, "select * from logbook where id > %d ", -from_id); 
	}
	strcat(statement, "ORDER BY id DESC LIMIT 50;");

	//printf("[%s]\n", statement);
	sqlite3_prepare_v2(db, statement, -1, &stmt, NULL);

	char output_path[200];	//dangerous, find the MAX_PATH and replace 200 with it
	sprintf(output_path, "%s/sbitx/data/result_rows.txt", getenv("HOME"));
	strcpy(result_file, output_path);
	
	FILE *pf = fopen(output_path, "w");
	if (!pf)
		return -1;

	int rec = 0;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		int i;
		int num_cols = sqlite3_column_count(stmt);
		for (i = 0; i < num_cols; i++){
			switch (sqlite3_column_type(stmt, i))
			{
			case (SQLITE3_TEXT):
				strcpy(param, sqlite3_column_text(stmt, i));
				break;
			case (SQLITE_INTEGER):
				sprintf(param, "%d", sqlite3_column_int(stmt, i));
				break;
			case (SQLITE_FLOAT):
				sprintf(param, "%g", sqlite3_column_double(stmt, i));
				break;
			case (SQLITE_NULL):
				break;
			default:
				sprintf(param, "%d", sqlite3_column_type(stmt, i));
				break;
			}
			//printf("%s|", param);
			fprintf(pf, "%s|", param);
		}
		//printf("\n");
		fprintf(pf, "\n");
	}
	sqlite3_finalize(stmt);
	fclose(pf);
	return rec;
}

int logbook_count_dup(const char *callsign, int last_seconds){
	char date_str[100], time_str[100], statement[1000];
	sqlite3_stmt *stmt;

	time_t log_time = time_sbitx() - last_seconds;
	struct tm *tmp = gmtime(&log_time);
	sprintf(date_str, "%04d-%02d-%02d", tmp->tm_year + 1900, tmp->tm_mon + 1, tmp->tm_mday);
	sprintf(time_str, "%02d%02d", tmp->tm_hour, tmp->tm_min);
	
	sprintf(statement, "select * from logbook where "
		"callsign_recv=\"%s\" AND qso_date >= \"%s\" AND qso_time >= \"%s\"",
		callsign, date_str, time_str);

	sqlite3_prepare_v2(db, statement, -1, &stmt, NULL);
	int rec = 0;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		rec++;
	}
	sqlite3_finalize(stmt);
	return rec;
}

int logbook_get_grids(void (*f)(char *,int)) {
	sqlite3_stmt *stmt;
	char *statement = "SELECT exch_recv, COUNT(*) AS n FROM logbook "
		"GROUP BY exch_recv order by exch_recv";
	int res = sqlite3_prepare_v2(db, statement, -1, &stmt, NULL);
	int cnt = 0;
	char grid[10];
	int n = 0;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		int num_cols = sqlite3_column_count(stmt);
		for (int i = 0; i < num_cols; i++){
			char const *col_name = sqlite3_column_name(stmt, i);
			if (!strcmp(col_name, "exch_recv")) { 
				strcpy(grid, sqlite3_column_text(stmt, i));
			} else
			if (!strcmp(col_name, "n")) { 
				n = sqlite3_column_int(stmt, i);
			}
		}
		f(grid,n);
		cnt++;
	}
	sqlite3_finalize(stmt);
	return cnt;
}
bool logbook_caller_exists(char * id) {
	sqlite3_stmt *stmt;
	char * statement = "SELECT EXISTS(SELECT 1 FROM logbook WHERE callsign_recv=?)";
	int res = sqlite3_prepare_v2(db, statement, -1, &stmt, NULL);
	if (res != SQLITE_OK) return false;
	bool exists = false;
	res = sqlite3_bind_text(stmt, 1, id, strlen(id), SQLITE_STATIC);
	if (res == SQLITE_OK) {
		res = sqlite3_step(stmt);
		int i = sqlite3_column_int(stmt, 0);
		exists = ( res == SQLITE_ROW && i != 0);
	}
	sqlite3_finalize(stmt);
	return exists;
}
bool logbook_grid_exists(char *id) {
	sqlite3_stmt *stmt;
	char * statement = "SELECT EXISTS(SELECT 1 FROM logbook WHERE exch_recv=?)";
	int res = sqlite3_prepare_v2(db, statement, -1, &stmt, NULL);
	if (res != SQLITE_OK) return false;
	bool exists = false;
	res = sqlite3_bind_text(stmt, 1, id, strlen(id), SQLITE_STATIC);
	if (res == SQLITE_OK) {
		res = sqlite3_step(stmt);
		int i = sqlite3_column_int(stmt, 0);
		exists = ( res == SQLITE_ROW && i != 0);
	}
	sqlite3_finalize(stmt);
	return exists;
}
int logbook_prev_log(const char *callsign, char *result){
	char statement[1000], param[2000];
	sqlite3_stmt *stmt;
	sprintf(statement, "select * from logbook where "
		"callsign_recv=\"%s\" ORDER BY id DESC",
		callsign);
	strcpy(result, callsign);
	strcat(result, ": ");
	int res = sqlite3_prepare_v2(db, statement, -1, &stmt, NULL);
	int rec = 0;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		int i;
		int num_cols = sqlite3_column_count(stmt);
		if (rec == 0) {
			for (i = 0; i < num_cols; i++){
				char const *col_name = sqlite3_column_name(stmt, i);
			    if (!strcmp(col_name, "id")) { continue; }
				if (!strcmp(col_name, "callsign_recv")) { continue; }
				switch (sqlite3_column_type(stmt, i))
				{
				case (SQLITE3_TEXT):
					strcpy(param, sqlite3_column_text(stmt, i));
					break;
				case (SQLITE_INTEGER):
					sprintf(param, "%d", sqlite3_column_int(stmt, i));
					break;
				case (SQLITE_FLOAT):
					sprintf(param, "%g", sqlite3_column_double(stmt, i));
					break;
				case (SQLITE_NULL):
					break;
				default:
					sprintf(param, "%d", sqlite3_column_type(stmt, i));
					break;
				}
				strcat(result, param);
				if (!strcmp(col_name, "qso_date")) strcat(result, "_");
				else strcat(result, " ");
			}
		}
		rec++;
	}
	sqlite3_finalize(stmt);
	sprintf(param, ": %d", rec);
	strcat(result, param);
	return rec;
}
int row_count_callback(void *data, int argc, char **argv, char **azColName) {
    int *count = (int*)data;
    (*count)++;
    return 0;
}
void logbook_open(){
	char db_path[200];	//dangerous, find the MAX_PATH and replace 200 with it
    char *zErrMsg = 0;
	sprintf(db_path, "%s/sbitx/data/sbitx.db", getenv("HOME"));

	rc = sqlite3_open(db_path, &db);
	if( rc != SQLITE_OK ){
		fprintf(stderr, "Failed to open logbook. SQL error: %s\n", zErrMsg);
		sqlite3_free(zErrMsg);
		return;
	}
	char *sql = "SELECT name FROM sqlite_master WHERE type='index' AND tbl_name='logbook' AND name IN ('gridIx', 'callIx');";
    int index_count = 0;
    rc = sqlite3_exec(db, sql, row_count_callback, &index_count, &zErrMsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "logbook index check failed: %s\n", zErrMsg);
        sqlite3_free(zErrMsg);
		return;
    }
    if (index_count == 2) {
        printf("Logbook indexes are OK.\n");
    } else {
		char *sql1 = "CREATE INDEX gridIx ON logbook (exch_recv);";
		char *sql2 = "CREATE INDEX callIx ON logbook (callsign_recv);";
		rc = sqlite3_exec(db, sql1, NULL, NULL, &zErrMsg);
		if (rc != SQLITE_OK) {
			fprintf(stderr, "SQL-error creating gridIx: %s\n", zErrMsg);
			sqlite3_free(zErrMsg);
			return;
		}
		rc = sqlite3_exec(db, sql2, NULL, NULL, &zErrMsg);
		if (rc != SQLITE_OK) {
			fprintf(stderr, "SQL-error creating callIx: %s\n", zErrMsg);
			sqlite3_free(zErrMsg);
			return;
		}
		printf("Logbook indexes created.\n");
    }
}

void logbook_add(char *contact_callsign, char *rst_sent, char *exchange_sent, 

	char *rst_recv, char *exchange_recv, char *comments){
	char statement[1000], *err_msg, date_str[10], time_str[10];
	char freq[12], log_freq[12], mode[10], mycallsign[10];

	time_t log_time = time_sbitx();
	struct tm *tmp = gmtime(&log_time);
	get_field_value("r1:freq", freq);
	get_field_value("r1:mode", mode);
	get_field_value("#mycallsign", mycallsign);

	sprintf(log_freq, "%d", atoi(freq)/1000);
	
	sprintf(date_str, "%04d-%02d-%02d", tmp->tm_year + 1900, tmp->tm_mon + 1, tmp->tm_mday);
	sprintf(time_str, "%02d%02d", tmp->tm_hour, tmp->tm_min);

	sprintf(statement,
		"INSERT INTO logbook (freq, mode, qso_date, qso_time, callsign_sent,"
		"rst_sent, exch_sent, callsign_recv, rst_recv, exch_recv, comments) "
		"VALUES('%s', '%s', '%s', '%s',  '%s','%s','%s',  '%s','%s','%s','%s');",
			log_freq, mode, date_str, time_str, mycallsign,
			 rst_sent, exchange_sent, contact_callsign, rst_recv, exchange_recv, comments);

	if (db == NULL)
		logbook_open();

	sqlite3_exec(db, statement, 0,0, &err_msg);
	
}

/*
void import_logs(char *filename){
	char entry_text[1000], statement[1000];
	char freq[10], mode[10], date_str[10], time_str[10], mycall[10], rst_sent[10],
	exchange_sent[10], contact_callsign[10], rst_recv[10], exchange_recv[10];

	FILE *pf = fopen(filename, "r");
	while(fgets(entry_text, sizeof(entry_text), pf)){
		char *p = strtok(entry_text, "\t ");
		strcpy(freq, p);
		strcpy(mode, strtok(NULL, "\t "));
		strcpy(date_str, strtok(NULL, "\t "));
		strcpy(time_str, strtok(NULL, "\t "));
		strcpy(mycall, strtok(NULL, "\t "));
		strcpy(rst_sent, strtok(NULL, "\t "));
		strcpy(exchange_sent, strtok(NULL, "\t "));
		strcpy(contact_callsign, strtok(NULL, "\t "));
		strcpy(rst_recv, strtok(NULL, "\t "));
		strcpy(exchange_recv, strtok(NULL, "\t\n"));
		sprintf(statement,
		"INSERT INTO logbook (freq, mode, qso_date, qso_time, callsign_sent,"
		"rst_sent, exch_sent, callsign_recv, rst_recv, exch_recv) "
		"VALUES('%s', '%s', '%s', '%s',  '%s','%s','%s',  '%s','%s','%s');",
			freq, mode, date_str, time_str,
			 mycall, rst_sent, exchange_sent,
			contact_callsign, rst_recv, exchange_recv);
			
		puts(statement);
	}
	fclose(pf);
}
*/

// ADIF field headers, see note above
const static char *adif_names[]={"ID","MODE","FREQ","QSO_DATE","TIME_ON","OPERATOR","RST_SENT","STX_String","CALL","RST_RCVD","SRX_String","STX","COMMENTS"};

struct band_name {
	char *name;
	int from, to;
} bands[] = {
	{"160M", 1800, 2000},
	{"80M", 3500, 4000},
	{"60M", 5000, 5500},
	{"40M", 7000, 7300},
	{"30M", 10000, 10150},
	{"20M", 14000, 14350},
	{"17M", 18000, 18200},
	{"15M", 21000, 21450},
	{"12M", 24800, 25000},
	{"10M", 28000, 29700},
};

static void strip_chr(char *str, const char to_remove){
    int i, j, len;

    len = strlen(str);
    for(i=0; i<len; i++) {
        if(str[i] == to_remove) {
            for(j=i; j<len; j++)
                str[j] = str[j+1];
            len--;
            i--;
        }
    }
}

int export_adif(char *path, char *start_date, char *end_date){
	sqlite3_stmt *stmt;
	char statement[200], param[2000], qso_band[20];
	

	//add to the bottom of the logbook
	sprintf(statement, "select * from logbook where (qso_date >= '%s' AND  qso_date <= '%s')  ORDER BY id DESC;",
		start_date, end_date);

	FILE *pf = fopen(path, "w");
	sqlite3_prepare_v2(db, statement, -1, &stmt, NULL);
	fprintf(pf, "/ADIF file\n");
	fprintf(pf, "generated from sBITX log db by Log2ADIF program\n");	
	fprintf(pf, "<adif version:5>3.1.4\n");	
	fprintf(pf, "<EOH>\n");	

	int rec = 0;

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		int i;
		int num_cols = sqlite3_column_count(stmt);
		for (i = 0; i < num_cols; i++){
			switch (sqlite3_column_type(stmt, i))
			{
			case (SQLITE3_TEXT):
				strcpy(param, sqlite3_column_text(stmt, i));
				break;
			case (SQLITE_INTEGER):
				sprintf(param, "%d", sqlite3_column_int(stmt, i));
				break;
			case (SQLITE_FLOAT):
				sprintf(param, "%g", sqlite3_column_double(stmt, i));
				break;
			case (SQLITE_NULL):
				break;
			default:
				sprintf(param, "%d", sqlite3_column_type(stmt, i));
				break;
			}
			//If mode is FT8; set rec to 1 so we switch to use gridsquare instead of stx/srx fields - n1qm
			if (i == 1)  
				if (!strcmp("FT8",param))
					rec = 1;
			  else
					rec = 0;

			if (i == 2){
				long f = atoi(param);
				float ffreq=atof(param)/1000.0;  // convert kHz to MHz
				sprintf(param, "%.3f",ffreq); // write out with 3 decimal digits
				for (int j = 0 ; j < sizeof(bands)/sizeof(struct band_name); j++)
					if (bands[j].from <= f && f <= bands[j].to){
						fprintf(pf, "<BAND:%d>%s\n", strlen(bands[j].name), bands[j].name); 
					}
			}
			else if (i == 3) //it is the date
				strip_chr(param, '-');
		switch (i) {
			case 7:
				if (rec == 1)
					fprintf(pf, "<%s:%d>%s\n", "MY_GRIDSQUARE", strlen(param), param);
				else
					fprintf(pf, "<%s:%d>%s\n", adif_names[i], strlen(param), param);
				break;
			case 10:
				if (rec == 1)
					fprintf(pf, "<%s:%d>%s\n", "GRIDSQUARE", strlen(param), param);
				else
					fprintf(pf, "<%s:%d>%s\n", adif_names[i], strlen(param), param);
				break;
			default:
				fprintf(pf, "<%s:%d>%s\n", adif_names[i], strlen(param), param);
				break;
		}
	   	
		}
		fprintf(pf, "<EOR>\n");
		//printf("\n");
	}
	sqlite3_finalize(stmt);
	fclose(pf);
}

/*
int main(int argc, char **argv){
	database_open();
	query_log_to_text_file(argv[1], 50);
}
*/
