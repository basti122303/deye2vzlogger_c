#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <ctype.h>
#include <unistd.h>

#include "ini.h"

#define MAX_INVERTERS 10
#define MAX_LINE 2048

typedef struct {
    char host[256];
    char uuid[128];
    char user[64];
    char pass[64];
    char name[64];
} Inverter;

typedef struct {
    int DB_direct;
    char DB_Host[256];
    Inverter inverters[MAX_INVERTERS];
    int inverter_count;
} Config;

Config config = {0};

static int config_handler(void* user, const char* section, const char* name, const char* value) {
    Config* pconfig = (Config*)user;

    if (strcmp(section, "general") == 0) {
        if (strcmp(name, "DB_direct") == 0) {
            pconfig->DB_direct = atoi(value);
        } else if (strcmp(name, "DB_Host") == 0) {
            strncpy(pconfig->DB_Host, value, sizeof(pconfig->DB_Host)-1);
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
    if (ini_parse("/etc/vz/config.ini", config_handler, &config) < 0) {
        fprintf(stderr, "Can't load '/etc/vz/config.ini'\n");
        return 1;
    }

    CURL *curl;
    CURLcode res;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl = curl_easy_init();

    int error_count = 0;

    if (curl) {
        for (int i = 0; i < config.inverter_count; i++) {
            char url[256];
            snprintf(url, sizeof(url), "http://%s/status.html", config.inverters[i].host);

            struct MemoryStruct chunk = {malloc(1), 0};

            curl_easy_setopt(curl, CURLOPT_URL, url);
            curl_easy_setopt(curl, CURLOPT_USERNAME, config.inverters[i].user);
            curl_easy_setopt(curl, CURLOPT_PASSWORD, config.inverters[i].pass);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

            res = curl_easy_perform(curl);
            if (res != CURLE_OK) {
                fprintf(stderr, "Request to %s failed: %s\n", config.inverters[i].host, curl_easy_strerror(res));
                error_count++;
                continue;
            }

            int value = extract_webdata_now_p(chunk.memory);
            if (value < 0) {
                fprintf(stderr, "Failed to parse power from %s\n", config.inverters[i].host);
                error_count++;
            } else {
                if (config.DB_direct) {
                    char db_url[512];
                    snprintf(db_url, sizeof(db_url),
                             "http://%s/middleware/data/%s.json?operation=add&value=%d",
                             config.DB_Host, config.inverters[i].uuid, value);

                    curl_easy_setopt(curl, CURLOPT_URL, db_url);
                    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
                    res = curl_easy_perform(curl);

                    if (res != CURLE_OK) {
                        fprintf(stderr, "DB push failed: %s\n", curl_easy_strerror(res));
                        error_count++;
                    }
                } else {
                    printf("%s = %d\n", config.inverters[i].name, value);
                }
            }

            free(chunk.memory);
        }

        curl_easy_cleanup(curl);
    }

    curl_global_cleanup();

    if (!config.DB_direct) {
        sleep(10);
    }

    return error_count ? 1 : 0;
}
