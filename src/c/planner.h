/*
 * planner.h - Core data model and logic of the Intelligent Study Planner.
 * Pure C11: no I/O, no globals, no Emscripten-specific code.
 * Every function receives a Planner pointer, so the logic can be tested
 * natively with gcc and reused unchanged inside WebAssembly.
 */
#ifndef PLANNER_H
#define PLANNER_H

#include <stddef.h>

/* ---------- Capacity limits (fixed-size arrays) ---------- */
#define MAX_SUBJECTS        50
#define MAX_TOPICS          500
#define MAX_SCHEDULE_ITEMS  2000
#define MAX_PLAN_DAYS       90
#define NAME_LEN            64
#define COLOR_COUNT         8
#define MIN_DAILY_HOURS     0.5
#define MAX_DAILY_HOURS     16.0

/* ---------- Enumerations ---------- */
typedef enum {
    PL_OK            =  0,
    PL_ERR_FULL      = -1,   /* array capacity reached         */
    PL_ERR_INVALID   = -2,   /* bad name / difficulty / value  */
    PL_ERR_NOT_FOUND = -3,   /* id does not exist              */
    PL_ERR_DATE      = -4,   /* impossible calendar date       */
    PL_ERR_DUPLICATE = -5    /* subject name already used      */
} PlannerStatus;

typedef enum { DIFF_EASY = 1, DIFF_MEDIUM = 2, DIFF_HARD = 3 } Difficulty;

typedef enum {
    LEVEL_LOW = 0, LEVEL_MEDIUM = 1, LEVEL_HIGH = 2, LEVEL_CRITICAL = 3
} PriorityLevel;

typedef enum {
    SORT_PRIORITY = 0, SORT_NAME = 1, SORT_EXAM = 2,
    SORT_DIFFICULTY = 3, SORT_STATUS = 4
} SortKey;

typedef enum { STATUS_ALL = 0, STATUS_PENDING = 1, STATUS_DONE = 2 } StatusFilter;

/* ---------- Structures ---------- */
typedef struct {
    int year, month, day;
} Date;

typedef struct {
    int  id;
    char name[NAME_LEN];
    Date exam;
    long exam_day;           /* exam date as a day number (see days_from_civil) */
    int  color;              /* 0 .. COLOR_COUNT-1, used by the UI             */
} Subject;

typedef struct {
    int  id;
    int  subject_id;
    char name[NAME_LEN];
    int  difficulty;         /* Difficulty                                      */
    int  completed;          /* 0 or 1                                          */
} Topic;

typedef struct {
    int    day_offset;       /* 0 = today, 1 = tomorrow, ...                    */
    long   day_number;       /* absolute day number of the study day            */
    int    topic_id;
    int    subject_id;
    double hours;            /* length of this study block                      */
    int    late;             /* 1 if the block falls after the exam date        */
} ScheduleItem;

typedef struct {
    int    subjects, topics, completed, pending;
    double percent_complete;
    double hours_remaining, hours_completed;
    int    high_priority_pending;
    int    next_exam_subject_id;   /* 0 if no upcoming exam            */
    int    next_exam_days;         /* -1 if no upcoming exam           */
} PlannerStats;

typedef struct {
    int         subject_id;   /* 0 = all subjects                               */
    int         status;       /* StatusFilter                                   */
    int         difficulty;   /* 0 = all, otherwise Difficulty                  */
    const char *text;         /* case-insensitive search text, may be NULL/""   */
    int         sort;         /* SortKey                                        */
} TopicQuery;

typedef struct {
    Subject      subjects[MAX_SUBJECTS];
    int          subject_count;
    int          next_subject_id;
    Topic        topics[MAX_TOPICS];
    int          topic_count;
    int          next_topic_id;
    ScheduleItem schedule[MAX_SCHEDULE_ITEMS];
    int          schedule_count;
    int          schedule_truncated;   /* 1 if the plan did not fit in the horizon */
    long         today;                /* day number of "today"                    */
    double       daily_hours;
} Planner;

/* ---------- Date helpers ---------- */
long planner_days_from_civil(int y, int m, int d);
void planner_civil_from_days(long days, int *y, int *m, int *d);
int  planner_date_is_valid(int y, int m, int d);

/* ---------- Lifecycle / settings ---------- */
void planner_init(Planner *p);
int  planner_set_today(Planner *p, int y, int m, int d);
int  planner_set_daily_hours(Planner *p, double hours);

/* ---------- Subjects (pass id = 0 to auto-assign; restore uses explicit id) ---------- */
int  planner_add_subject(Planner *p, const char *name, int y, int m, int d, int color);
int  planner_restore_subject(Planner *p, int id, const char *name, int y, int m, int d, int color);
int  planner_update_subject(Planner *p, int id, const char *name, int y, int m, int d, int color);
int  planner_delete_subject(Planner *p, int id);   /* also deletes its topics */
const Subject *planner_find_subject(const Planner *p, int id);   /* binary search */

/* ---------- Topics ---------- */
int  planner_add_topic(Planner *p, int subject_id, const char *name, int difficulty);
int  planner_restore_topic(Planner *p, int id, int subject_id, const char *name,
                           int difficulty, int completed);
int  planner_update_topic(Planner *p, int id, int subject_id, const char *name, int difficulty);
int  planner_set_topic_completed(Planner *p, int id, int completed);
int  planner_delete_topic(Planner *p, int id);
const Topic *planner_find_topic(const Planner *p, int id);       /* binary search */

/* ---------- Priority algorithm ---------- */
int    planner_urgency_points(int days_left);
int    planner_difficulty_points(int difficulty);
double planner_estimated_hours(int difficulty);
int    planner_days_until_exam(const Planner *p, const Subject *s);
int    planner_topic_priority(const Planner *p, const Topic *t);
int    planner_priority_level(int score);

/* ---------- Search, filter, sort ---------- */
int  planner_query_topics(const Planner *p, const TopicQuery *q, int *out, int max_out);
void planner_sort_topics(const Planner *p, int *idx, int n, int sort_key);
int  planner_sort_subjects_by_exam(const Planner *p, int *out);

/* ---------- Statistics ---------- */
void planner_compute_stats(const Planner *p, PlannerStats *st);
void planner_subject_progress(const Planner *p, int subject_id, int *done, int *total);

/* ---------- Schedule generation ---------- */
int  planner_generate_schedule(Planner *p);   /* returns number of blocks */

#endif /* PLANNER_H */
