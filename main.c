#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <ctype.h>
#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

#include "ini.h"

#define MAX_INVERTERS 10
#define MAX_LINE 2048

#define CONFIG_FILE "/etc/vz/config.ini"

typedef struct {
    char host[256];
    char uuid[128];
    char user[64];
    char pass[64];
    char name[64];
} Inverter;

typedef struct {
    int logging_enabled;
    char logfile[256];
    int DB_direct;
    char DB_Host[256];
    Inverter inverters[MAX_INVERTERS];
    int inverter_count;
} Config;

Config config = {0};

void log_msg(const char *fmt, ...) {
    if (!config.logging_enabled) return;

    FILE *fp = fopen(config.logfile, "a");
    if (!fp) return;

    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);

    fprintf(fp, "[%04d-%02d-%02d %02d:%02d:%02d] ",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec);

    va_list args;
    va_start(args, fmt);
    vfprintf(fp, fmt, args);
    va_end(args);

    fprintf(fp, "\n");
    fflush(fp);
    fclose(fp);
}

static int config_handler(void* user, const char* section, const char* name, const char* value) {
    Config* pconfig = (Config*)user;

    if (strcmp(section, "general") == 0) {
        if (strcmp(name, "logging") == 0) {
            pconfig->logging_enabled = atoi(value);
        }
        else if (strcmp(name, "logfile") == 0) {
            strncpy(pconfig->logfile, value, sizeof(pconfig->logfile) - 1);
        }
        else if (strcmp(name, "DB_direct") == 0) {
            pconfig->DB_direct = atoi(value);
        }
        else if (strcmp(name, "DB_Host") == 0) {
            strncpy(pconfig->DB_Host, value, sizeof(pconfig->DB_Host) - 1);
        }
    } else if (strncmp(section, "inverter", 8) == 0) {
        int idx = pconfig->inverter_count;
        if (idx >= MAX_INVERTERS) return 0;
        Inverter* inv = &pconfig->inverters[idx];

        if (strcmp(name, "host") == 0) {
            strncpy(inv->host, value, sizeof(inv->host)-1);
        } else if (strcmp(name, "uuid") == 0) {
            strncpy(inv->uuid, value, sizeof(inv->uuid)-1);
        } else if (strcmp(name, "user") == 0) {
            strncpy(inv->user, value, sizeof(inv->user)-1);
        } else if (strcmp(name, "pass") == 0) {
            strncpy(inv->pass, value, sizeof(inv->pass)-1);
        } else if (strcmp(name, "name") == 0) {
            strncpy(inv->name, value, sizeof(inv->name)-1);
            pconfig->inverter_count++;
        }
    }

    return 1;
}

struct MemoryStruct {
    char *memory;
    size_t size;
};

static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t real_size = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;

    char *ptr = realloc(mem->memory, mem->size + real_size + 1);
    if (!ptr) return 0;

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, real_size);
    mem->size += real_size;
    mem->memory[mem->size] = 0;

    return real_size;
}

int extract_webdata_now_p(const char* html) {
    const char* pattern = "webdata_now_p = \"";
    const char* found = strstr(html, pattern);
    if (!found) return -1;

    found += strlen(pattern);
    int value = 0;
    while (isdigit(*found)) {
        value = value * 10 + (*found - '0');
        found++;
    }

    return value;
}

int main() {
    // defaults
    config.logging_enabled = 0;
    snprintf(config.logfile, sizeof(config.logfile), "/var/log/deye2vzlogger.log");

    if (ini_parse(CONFIG_FILE, config_handler, &config) < 0) {
        fprintf(stderr, "Can't load config file: %s\n", CONFIG_FILE);
        log_msg("Failed to load config file, using defaults");
    }

    log_msg("Starting deye2vzlogger (DB_direct=%d, inverters=%d)", config.DB_direct, config.inverter_count);

    CURL *curl;
    CURLcode res;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl = curl_easy_init();
    if (!curl) {
        log_msg("Failed to initialize CURL");
        return 1;
    }

    int error_count;

    do {
        error_count = 0;

        for (int i = 0; i < config.inverter_count; i++) {

            curl_easy_reset(curl);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, "deye2vzlogger/1.0");

            char url[256];
            snprintf(url, sizeof(url), "http://%s/status.html",
                     config.inverters[i].host);

            struct MemoryStruct chunk = { malloc(1), 0 };

            curl_easy_setopt(curl, CURLOPT_URL, url);
	    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
	    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
	    curl_easy_setopt(curl, CURLOPT_USERAGENT, "deye2vzlogger/1.0");
            curl_easy_setopt(curl, CURLOPT_USERNAME, config.inverters[i].user);
            curl_easy_setopt(curl, CURLOPT_PASSWORD, config.inverters[i].pass);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

            res = curl_easy_perform(curl);
            if (res != CURLE_OK) {
                log_msg("Request to %s failed: %s",
                        config.inverters[i].host,
                        curl_easy_strerror(res));
                free(chunk.memory);
                error_count++;
                continue;
            }

            int value = extract_webdata_now_p(chunk.memory);
            if (value < 0) {
                log_msg("Failed to parse power from %s",
                        config.inverters[i].host);
                error_count++;
            } else {
                if (config.DB_direct) {
                    char db_url[512];
                    snprintf(db_url, sizeof(db_url),
                             "http://%s/middleware/data/%s.json?operation=add&value=%d",
                             config.DB_Host,
                             config.inverters[i].uuid,
                             value);

                    curl_easy_reset(curl);
                    curl_easy_setopt(curl, CURLOPT_URL, db_url);
                    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

                    res = curl_easy_perform(curl);
                    if (res != CURLE_OK) {
                        log_msg("DB push failed: %s",
                                curl_easy_strerror(res));
                        error_count++;
                    } else {
    			log_msg("DB push OK: %s = %d", config.inverters[i].name, value);
		    }
                } else {
                    printf("%s = %d\n", config.inverters[i].name, value);
                    log_msg("%s = %d", config.inverters[i].name, value);
                }
            }

            free(chunk.memory);
        }

        if (config.DB_direct)
            sleep(10);

    } while (config.DB_direct);

    curl_easy_cleanup(curl);
    curl_global_cleanup();

    // always return 0, not the error code
    // TODO: error handling
    //return error_count ? 1 : 0;
    return 0;
}
