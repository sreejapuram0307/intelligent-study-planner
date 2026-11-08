/*
 * test_api.c - Native tests for the sp_* API used by JavaScript.
 * Build: gcc -std=c11 -Wall -Wextra -Isrc/c tests/test_api.c src/c/api.c src/c/planner.c -o test_api
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api.h"

static int checks = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            printf("FAILED line %d: %s\n", __LINE__, #cond);               \
            exit(1);                                                       \
        }                                                                  \
    } while (0)

#define HAS(text, needle) (strstr((text), (needle)) != NULL)

static char saved[65536];   /* copy of a JSON result (the API buffer is reused) */

static void keep(const char *json)
{
    strncpy(saved, json, sizeof saved - 1);
    saved[sizeof saved - 1] = '\0';
}

int main(void)
{
    int a, b, t1, t2;
    const char *json;

    sp_init();
    CHECK(sp_set_today(2026, 11, 8) == 0);
    CHECK(sp_set_daily_hours(3.0) == 0);
    CHECK(sp_set_daily_hours(99.0) < 0);

    a = sp_add_subject("Maths \"Advanced\"", 2026, 11, 10, 0);
    b = sp_add_subject("Physics", 2026, 11, 28, 1);
    CHECK(a == 1 && b == 2);
    CHECK(sp_add_subject("Physics", 2026, 12, 1, 1) < 0);
    CHECK(strcmp(sp_error_message(-5), "A subject with this name already exists.") == 0);

    t1 = sp_add_topic(a, "Integration", 3);
    t2 = sp_add_topic(b, "Optics", 2);
    CHECK(t1 == 1 && t2 == 2);

    /* subjects: names are escaped, nearest exam first */
    json = sp_get_subjects_json();
    CHECK(HAS(json, "\"name\":\"Maths \\\"Advanced\\\"\""));
    CHECK(HAS(json, "\"days_left\":2"));
    CHECK(strstr(json, "Maths") < strstr(json, "Physics"));

    /* query: priority 40 + 20 = 60, critical */
    json = sp_query_topics_json(0, 0, 0, "", 0);
    CHECK(HAS(json, "\"count\":2"));
    CHECK(HAS(json, "\"priority\":60"));
    CHECK(HAS(json, "\"level\":3"));
    json = sp_query_topics_json(0, 0, 0, "opt", 1);
    CHECK(HAS(json, "\"count\":1"));
    json = sp_query_topics_json(0, 0, 0, NULL, 0);
    CHECK(HAS(json, "\"count\":2"));

    /* schedule: 3h/day -> hard topic on day 0, medium topic on day 1 */
    json = sp_generate_schedule_json();
    CHECK(HAS(json, "\"offset\":0"));
    CHECK(HAS(json, "\"offset\":1"));
    CHECK(HAS(json, "\"date\":\"2026-11-08\""));
    CHECK(HAS(json, "\"truncated\":false"));

    json = sp_get_dashboard_json();
    CHECK(HAS(json, "\"pending\":2"));
    CHECK(HAS(json, "\"next_exam\":{\"id\":1"));
    json = sp_get_progress_json();
    CHECK(HAS(json, "\"workload\":["));

    /* completing a topic changes the stats */
    CHECK(sp_set_topic_completed(t1, 1) == 0);
    json = sp_get_dashboard_json();
    CHECK(HAS(json, "\"completed\":1"));

    /* persistence round trip: snapshot -> reset -> restore -> same snapshot */
    keep(sp_get_state_json());
    sp_init();
    CHECK(strcmp(sp_get_state_json(), "{\"version\":1,\"daily_hours\":2.0,\"subjects\":[],\"topics\":[]}") == 0);
    CHECK(sp_restore_subject(1, "Maths \"Advanced\"", 2026, 11, 10, 0) == 1);
    CHECK(sp_restore_subject(2, "Physics", 2026, 11, 28, 1) == 2);
    CHECK(sp_restore_topic(1, 1, "Integration", 3, 1) == 1);
    CHECK(sp_restore_topic(2, 2, "Optics", 2, 0) == 2);
    CHECK(sp_set_daily_hours(3.0) == 0);
    CHECK(strcmp(sp_get_state_json(), saved) == 0);

    /* clear keeps settings */
    sp_clear_data();
    CHECK(HAS(sp_get_state_json(), "\"daily_hours\":3.0"));
    CHECK(HAS(sp_get_state_json(), "\"subjects\":[]"));
    CHECK(HAS(sp_get_dashboard_json(), "\"next_exam\":null"));

    printf("All %d API checks passed.\n", checks);
    return 0;
}
