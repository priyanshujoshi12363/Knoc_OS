#ifndef ASSIST_H
#define ASSIST_H

#define ASSIST_CALLS_MAX 4
#define ASSIST_ARGS 6
#define ASSIST_KEY_MAX 24
#define ASSIST_VALUE_MAX 512

struct assist_arg
{
    char key[ASSIST_KEY_MAX];
    char value[ASSIST_VALUE_MAX];
};

typedef struct assist_call
{
    char name[32];
    int count;
    struct assist_arg args[ASSIST_ARGS];
} assist_call_t;

void assist_init(const char *label);
void assist_list_tools(void);
int assist_parse_call(const char *json, assist_call_t *call);
int assist_run_calls(assist_call_t *calls, int count);
int assist_route(const char *request, assist_call_t *calls);
int assist_load_model(void);
int assist_begin(const char *message, int with_facts);
int assist_continue(const char *message, int with_facts);
int assist_reply(void);
const char *assist_last_reply(void);

#endif
