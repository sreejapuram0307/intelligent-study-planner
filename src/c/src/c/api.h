/*
 * api.c - WebAssembly-facing API of the Intelligent Study Planner.
 *
 * This file owns the single Planner instance and turns planner.c results
 * into JSON text for the browser. It contains no scoring or scheduling
 * logic of its own: everything is delegated to planner.c.
 */
#include "api.h"
#include "planner.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE   /* native builds (unit tests) ignore it */
#endif

#if defined(__GNUC__) || defined(__clang__)
#define PRINTF_LIKE(a, b) __attribute__((format(printf, a, b)))
#else
#define PRINTF_LIKE(a, b)
#endif

#define JSON_CAPACITY (1024 * 1024)
#define UPCOMING_LIMIT 5
#define PRIORITY_LIMIT 5
#define WORKLOAD_DAYS 14

static Planner g_planner;
static int     g_ready = 0;
static char    g_json[JSON_CAPACITY];

static Planner *P(void)
{
    if (!g_ready) {
        planner_init(&g_planner);
        g_ready = 1;
    }
    return &g_planner;
}

/* =====================================================================
 *  Tiny JSON writer (appends into g_json)
 * ===================================================================== */

typedef struct {
    size_t len;
    int    overflow;
} Json;

static void json_begin(Json *j)
{
    j->len = 0;
    j->overflow = 0;
    g_json[0] = '\0';
}

static void json_printf(Json *j, const char *fmt, ...) PRINTF_LIKE(2, 3);

static void json_printf(Json *j, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (j->overflow) {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(g_json + j->len, JSON_CAPACITY - j->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= JSON_CAPACITY - j->len) {
        j->overflow = 1;
        return;
    }
    j->len += (size_t)n;
}

static void json_putc(Json *j, char c)
{
    if (j->overflow || j->len + 1 >= JSON_CAPACITY) {
        j->overflow = 1;
        return;
    }
    g_json[j->len++] = c;
    g_json[j->len] = '\0';
}

/* Writes a quoted, escaped JSON string. */
static void json_string(Json *j, const char *s)
{
    json_putc(j, '"');
    for (; *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
            case '"':  json_printf(j, "\\\""); break;
            case '\\': json_printf(j, "\\\\"); break;
            case '\n': json_printf(j, "\\n");  break;
            case '\r': json_printf(j, "\\r");  break;
            case '\t': json_printf(j, "\\t");  break;
            default:
                if (c < 0x20) {
                    json_printf(j, "\\u%04x", (unsigned)c);
                } else {
                    json_putc(j, (char)c);
                }
        }
    }
    json_putc(j, '"');
}

static void json_date_days(Json *j, long day_number)
{
    int y, m, d;
    planner_civil_from_days(day_number, &y, &m, &d);
    json_printf(j, "\"%04d-%02d-%02d\"", y, m, d);
}

static const char *json_finish(Json *j)
{
    if (j->overflow) {
        snprintf(g_json, JSON_CAPACITY, "{\"error\":\"Output too large\"}");
    }
    return g_json;
}

static const char *bool_text(int v)
{
    return v ? "true" : "false";
}

/* =====================================================================
 *  Shared object writers
 * ===================================================================== */

static void write_subject(Json *j, const Planner *p, const Subject *s)
{
    int done, total, i;
    int days = planner_days_until_exam(p, s);
    double hours_left = 0.0;
    double percent;

    planner_subject_progress(p, s->id, &done, &total);
    for (i = 0; i < p->topic_count; i++) {
        if (p->topics[i].subject_id == s->id && !p->topics[i].completed) {
            hours_left += planner_estimated_hours(p->topics[i].difficulty);
        }
    }
    percent = (total > 0) ? 100.0 * done / total : 0.0;

    json_printf(j, "{\"id\":%d,\"name\":", s->id);
    json_string(j, s->name);
    json_printf(j, ",\"exam\":\"%04d-%02d-%02d\",\"color\":%d,\"days_left\":%d,"
                   "\"urgency_points\":%d,\"expired\":%s,\"topics_total\":%d,"
                   "\"topics_done\":%d,\"percent\":%.1f,\"hours_remaining\":%.1f}",
                s->exam.year, s->exam.month, s->exam.day, s->color, days,
                planner_urgency_points(days), bool_text(days < 0), total, done,
                percent, hours_left);
}

static void write_topic(Json *j, const Planner *p, const Topic *t)
{
    const Subject *s = planner_find_subject(p, t->subject_id);
    int days = (s != NULL) ? planner_days_until_exam(p, s) : 0;
    int score = planner_topic_priority(p, t);

    json_printf(j, "{\"id\":%d,\"subject_id\":%d,\"subject\":", t->id, t->subject_id);
    json_string(j, (s != NULL) ? s->name : "");
    json_printf(j, ",\"color\":%d,\"name\":", (s != NULL) ? s->color : 0);
    json_string(j, t->name);
    json_printf(j, ",\"difficulty\":%d,\"completed\":%s,\"days_left\":%d,"
                   "\"expired\":%s,\"urgency_points\":%d,\"difficulty_points\":%d,"
                   "\"priority\":%d,\"level\":%d,\"hours\":%.1f}",
                t->difficulty, bool_text(t->completed), days, bool_text(days < 0),
                planner_urgency_points(days), planner_difficulty_points(t->difficulty),
                score, planner_priority_level(score), planner_estimated_hours(t->difficulty));
}

static void write_block(Json *j, const Planner *p, const ScheduleItem *it)
{
    const Topic *t = planner_find_topic(p, it->topic_id);
    const Subject *s = planner_find_subject(p, it->subject_id);

    json_printf(j, "{\"topic_id\":%d,\"topic\":", it->topic_id);
    json_string(j, (t != NULL) ? t->name : "");
    json_printf(j, ",\"subject_id\":%d,\"subject\":", it->subject_id);
    json_string(j, (s != NULL) ? s->name : "");
    json_printf(j, ",\"color\":%d,\"difficulty\":%d,\"hours\":%.1f,\"priority\":%d,\"late\":%s}",
                (s != NULL) ? s->color : 0, (t != NULL) ? t->difficulty : 0, it->hours,
                (t != NULL) ? planner_topic_priority(p, t) : 0, bool_text(it->late));
}

/* =====================================================================
 *  Lifecycle and settings
 * ===================================================================== */

EMSCRIPTEN_KEEPALIVE
void sp_init(void)
{
    planner_init(&g_planner);
    g_ready = 1;
}

EMSCRIPTEN_KEEPALIVE
void sp_clear_data(void)
{
    Planner *p = P();
    long today = p->today;
    double hours = p->daily_hours;

    planner_init(p);
    p->today = today;
    p->daily_hours = hours;
}

EMSCRIPTEN_KEEPALIVE
int sp_set_today(int y, int m, int d)
{
    return planner_set_today(P(), y, m, d);
}

EMSCRIPTEN_KEEPALIVE
int sp_set_daily_hours(double hours)
{
    return planner_set_daily_hours(P(), hours);
}

/* =====================================================================
 *  Subjects and topics (thin wrappers)
 * ===================================================================== */

EMSCRIPTEN_KEEPALIVE
int sp_add_subject(const char *name, int y, int m, int d, int color)
{
    return planner_add_subject(P(), name, y, m, d, color);
}

EMSCRIPTEN_KEEPALIVE
int sp_restore_subject(int id, const char *name, int y, int m, int d, int color)
{
    return planner_restore_subject(P(), id, name, y, m, d, color);
}

EMSCRIPTEN_KEEPALIVE
int sp_update_subject(int id, const char *name, int y, int m, int d, int color)
{
    return planner_update_subject(P(), id, name, y, m, d, color);
}

EMSCRIPTEN_KEEPALIVE
int sp_delete_subject(int id)
{
    return planner_delete_subject(P(), id);
}

EMSCRIPTEN_KEEPALIVE
int sp_add_topic(int subject_id, const char *name, int difficulty)
{
    return planner_add_topic(P(), subject_id, name, difficulty);
}

EMSCRIPTEN_KEEPALIVE
int sp_restore_topic(int id, int subject_id, const char *name, int difficulty, int completed)
{
    return planner_restore_topic(P(), id, subject_id, name, difficulty, completed);
}

EMSCRIPTEN_KEEPALIVE
int sp_update_topic(int id, int subject_id, const char *name, int difficulty)
{
    return planner_update_topic(P(), id, subject_id, name, difficulty);
}

EMSCRIPTEN_KEEPALIVE
int sp_set_topic_completed(int id, int completed)
{
    return planner_set_topic_completed(P(), id, completed);
}

EMSCRIPTEN_KEEPALIVE
int sp_delete_topic(int id)
{
    return planner_delete_topic(P(), id);
}

EMSCRIPTEN_KEEPALIVE
const char *sp_error_message(int code)
{
    switch (code) {
        case PL_OK:            return "OK";
        case PL_ERR_FULL:      return "Storage limit reached (50 subjects / 500 topics).";
        case PL_ERR_INVALID:   return "Please enter a valid name and difficulty.";
        case PL_ERR_NOT_FOUND: return "That item no longer exists.";
        case PL_ERR_DATE:      return "Please enter a valid date (2000-2100).";
        case PL_ERR_DUPLICATE: return "A subject with this name already exists.";
        default:               return "Unknown error.";
    }
}

/* =====================================================================
 *  JSON: persistence snapshot
 * ===================================================================== */

EMSCRIPTEN_KEEPALIVE
const char *sp_get_state_json(void)
{
    const Planner *p = P();
    Json j;
    int i;

    json_begin(&j);
    json_printf(&j, "{\"version\":1,\"daily_hours\":%.1f,\"subjects\":[", p->daily_hours);
    for (i = 0; i < p->subject_count; i++) {
        const Subject *s = &p->subjects[i];
        json_printf(&j, "%s{\"id\":%d,\"name\":", (i > 0) ? "," : "", s->id);
        json_string(&j, s->name);
        json_printf(&j, ",\"year\":%d,\"month\":%d,\"day\":%d,\"color\":%d}",
                    s->exam.year, s->exam.month, s->exam.day, s->color);
    }
    json_printf(&j, "],\"topics\":[");
    for (i = 0; i < p->topic_count; i++) {
        const Topic *t = &p->topics[i];
        json_printf(&j, "%s{\"id\":%d,\"subject_id\":%d,\"name\":",
                    (i > 0) ? "," : "", t->id, t->subject_id);
        json_string(&j, t->name);
        json_printf(&j, ",\"difficulty\":%d,\"completed\":%d}", t->difficulty, t->completed);
    }
    json_printf(&j, "]}");
    return json_finish(&j);
}

/* =====================================================================
 *  JSON: subjects
 * ===================================================================== */

EMSCRIPTEN_KEEPALIVE
const char *sp_get_subjects_json(void)
{
    const Planner *p = P();
    int order[MAX_SUBJECTS];
    int n, i;
    Json j;

    n = planner_sort_subjects_by_exam(p, order);
    json_begin(&j);
    json_printf(&j, "{\"count\":%d,\"subjects\":[", n);
    for (i = 0; i < n; i++) {
        if (i > 0) {
            json_putc(&j, ',');
        }
        write_subject(&j, p, &p->subjects[order[i]]);
    }
    json_printf(&j, "]}");
    return json_finish(&j);
}

/* =====================================================================
 *  JSON: topic search / filter / sort
 * ===================================================================== */

EMSCRIPTEN_KEEPALIVE
const char *sp_query_topics_json(int subject_id, int status, int difficulty,
                                 const char *text, int sort)
{
    const Planner *p = P();
    TopicQuery q;
    int out[MAX_TOPICS];
    int n, i;
    Json j;

    q.subject_id = subject_id;
    q.status     = status;
    q.difficulty = difficulty;
    q.text       = text;
    q.sort       = sort;
    n = planner_query_topics(p, &q, out, MAX_TOPICS);

    json_begin(&j);
    json_printf(&j, "{\"count\":%d,\"total\":%d,\"topics\":[", n, p->topic_count);
    for (i = 0; i < n; i++) {
        if (i > 0) {
            json_putc(&j, ',');
        }
        write_topic(&j, p, &p->topics[out[i]]);
    }
    json_printf(&j, "]}");
    return json_finish(&j);
}

/* =====================================================================
 *  JSON: dashboard
 * ===================================================================== */

EMSCRIPTEN_KEEPALIVE
const char *sp_get_dashboard_json(void)
{
    Planner *p = P();
    PlannerStats st;
    int order[MAX_SUBJECTS];
    int idx[MAX_TOPICS];
    TopicQuery q;
    int n, i, shown;
    double today_hours = 0.0;
    Json j;

    planner_compute_stats(p, &st);
    planner_generate_schedule(p);

    json_begin(&j);
    json_printf(&j, "{\"stats\":{\"subjects\":%d,\"topics\":%d,\"completed\":%d,\"pending\":%d,"
                    "\"percent\":%.1f,\"hours_remaining\":%.1f,\"hours_completed\":%.1f,"
                    "\"high_priority\":%d,\"daily_hours\":%.1f},",
                st.subjects, st.topics, st.completed, st.pending, st.percent_complete,
                st.hours_remaining, st.hours_completed, st.high_priority_pending,
                p->daily_hours);

    /* next exam */
    if (st.next_exam_subject_id > 0) {
        const Subject *s = planner_find_subject(p, st.next_exam_subject_id);
        json_printf(&j, "\"next_exam\":{\"id\":%d,\"subject\":", s->id);
        json_string(&j, s->name);
        json_printf(&j, ",\"days\":%d},", st.next_exam_days);
    } else {
        json_printf(&j, "\"next_exam\":null,");
    }

    /* upcoming exams (nearest first, finished exams skipped) */
    n = planner_sort_subjects_by_exam(p, order);
    json_printf(&j, "\"upcoming\":[");
    shown = 0;
    for (i = 0; i < n && shown < UPCOMING_LIMIT; i++) {
        const Subject *s = &p->subjects[order[i]];
        if (planner_days_until_exam(p, s) < 0) {
            continue;
        }
        if (shown > 0) {
            json_putc(&j, ',');
        }
        write_subject(&j, p, s);
        shown++;
    }

    /* highest-priority pending topics */
    q.subject_id = 0;
    q.status     = STATUS_PENDING;
    q.difficulty = 0;
    q.text       = NULL;
    q.sort       = SORT_PRIORITY;
    n = planner_query_topics(p, &q, idx, MAX_TOPICS);
    json_printf(&j, "],\"top_priorities\":[");
    shown = 0;
    for (i = 0; i < n && shown < PRIORITY_LIMIT; i++) {
        const Topic *t = &p->topics[idx[i]];
        const Subject *s = planner_find_subject(p, t->subject_id);
        if (s == NULL || planner_days_until_exam(p, s) < 0) {
            continue;
        }
        if (shown > 0) {
            json_putc(&j, ',');
        }
        write_topic(&j, p, t);
        shown++;
    }

    /* today's blocks from the generated schedule */
    for (i = 0; i < p->schedule_count; i++) {
        if (p->schedule[i].day_offset == 0) {
            today_hours += p->schedule[i].hours;
        }
    }
    json_printf(&j, "],\"today\":{\"date\":");
    json_date_days(&j, p->today);
    json_printf(&j, ",\"hours\":%.1f,\"blocks\":[", today_hours);
    shown = 0;
    for (i = 0; i < p->schedule_count; i++) {
        if (p->schedule[i].day_offset == 0) {
            if (shown > 0) {
                json_putc(&j, ',');
            }
            write_block(&j, p, &p->schedule[i]);
            shown++;
        }
    }
    json_printf(&j, "]}}");
    return json_finish(&j);
}

/* =====================================================================
 *  JSON: schedule
 * ===================================================================== */

EMSCRIPTEN_KEEPALIVE
const char *sp_generate_schedule_json(void)
{
    Planner *p = P();
    int i, k, m, pending = 0, expired = 0;
    double total = 0.0;
    Json j;

    planner_generate_schedule(p);

    for (i = 0; i < p->topic_count; i++) {
        const Topic *t = &p->topics[i];
        const Subject *s = planner_find_subject(p, t->subject_id);
        if (!t->completed) {
            pending++;
            if (s != NULL && planner_days_until_exam(p, s) < 0) {
                expired++;
            }
        }
    }
    for (i = 0; i < p->schedule_count; i++) {
        total += p->schedule[i].hours;
    }

    json_begin(&j);
    json_printf(&j, "{\"daily_hours\":%.1f,\"truncated\":%s,\"blocks\":%d,"
                    "\"pending_topics\":%d,\"expired_topics\":%d,\"total_hours\":%.1f,\"days\":[",
                p->daily_hours, bool_text(p->schedule_truncated), p->schedule_count,
                pending, expired, total);

    i = 0;
    while (i < p->schedule_count) {
        int offset = p->schedule[i].day_offset;
        double day_hours = 0.0;

        k = i;
        while (k < p->schedule_count && p->schedule[k].day_offset == offset) {
            day_hours += p->schedule[k].hours;
            k++;
        }
        if (i > 0) {
            json_putc(&j, ',');
        }
        json_printf(&j, "{\"offset\":%d,\"date\":", offset);
        json_date_days(&j, p->schedule[i].day_number);
        json_printf(&j, ",\"total_hours\":%.1f,\"blocks\":[", day_hours);
        for (m = i; m < k; m++) {
            if (m > i) {
                json_putc(&j, ',');
            }
            write_block(&j, p, &p->schedule[m]);
        }
        json_printf(&j, "]}");
        i = k;
    }
    json_printf(&j, "]}");
    return json_finish(&j);
}

/* =====================================================================
 *  JSON: progress (chart data)
 * ===================================================================== */

EMSCRIPTEN_KEEPALIVE
const char *sp_get_progress_json(void)
{
    Planner *p = P();
    PlannerStats st;
    double workload[WORKLOAD_DAYS] = {0.0};
    int diff_done[4] = {0, 0, 0, 0};
    int diff_total[4] = {0, 0, 0, 0};
    int i, level;
    Json j;

    planner_compute_stats(p, &st);
    planner_generate_schedule(p);

    for (i = 0; i < p->schedule_count; i++) {
        if (p->schedule[i].day_offset < WORKLOAD_DAYS) {
            workload[p->schedule[i].day_offset] += p->schedule[i].hours;
        }
    }
    for (i = 0; i < p->topic_count; i++) {
        int d = p->topics[i].difficulty;
        if (d >= DIFF_EASY && d <= DIFF_HARD) {
            diff_total[d]++;
            if (p->topics[i].completed) {
                diff_done[d]++;
            }
        }
    }

    json_begin(&j);
    json_printf(&j, "{\"overall\":{\"completed\":%d,\"total\":%d,\"percent\":%.1f,"
                    "\"hours_completed\":%.1f,\"hours_remaining\":%.1f},\"subjects\":[",
                st.completed, st.topics, st.percent_complete,
                st.hours_completed, st.hours_remaining);

    for (i = 0; i < p->subject_count; i++) {
        const Subject *s = &p->subjects[i];
        int done, total;
        planner_subject_progress(p, s->id, &done, &total);
        json_printf(&j, "%s{\"id\":%d,\"name\":", (i > 0) ? "," : "", s->id);
        json_string(&j, s->name);
        json_printf(&j, ",\"color\":%d,\"done\":%d,\"total\":%d,\"percent\":%.1f}",
                    s->color, done, total, (total > 0) ? 100.0 * done / total : 0.0);
    }

    json_printf(&j, "],\"difficulty\":[");
    for (level = DIFF_EASY; level <= DIFF_HARD; level++) {
        json_printf(&j, "%s{\"level\":%d,\"done\":%d,\"total\":%d}",
                    (level > DIFF_EASY) ? "," : "", level, diff_done[level], diff_total[level]);
    }

    json_printf(&j, "],\"workload\":[");
    for (i = 0; i < WORKLOAD_DAYS; i++) {
        json_printf(&j, "%s{\"offset\":%d,\"date\":", (i > 0) ? "," : "", i);
        json_date_days(&j, p->today + i);
        json_printf(&j, ",\"hours\":%.1f}", workload[i]);
    }
    json_printf(&j, "]}");
    return json_finish(&j);
}
