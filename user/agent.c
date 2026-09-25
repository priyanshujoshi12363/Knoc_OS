#include "ulib.h"
#include "assist.h"

static int starts_with(const char *text, const char *prefix)
{
    while (*prefix && *text == *prefix)
    {
        text++;
        prefix++;
    }

    return *prefix == 0;
}

static int run_model(const char *request)
{
    unsigned long started = uptime();

    if (assist_load_model() != 0)
    {
        return 1;
    }

    print("[agent] thinking...\n");
    assist_begin(request, 1);

    int result = assist_reply();

    if (result < 0)
    {
        print("[agent] out of context space, stopping\n");
        return 1;
    }

    if (result > 0)
    {
        print("[agent] stopped after too many steps\n");
        return 1;
    }

    print("[agent] done in ");
    print_uint((uptime() - started) / 100);
    print(" s\n");
    return 0;
}

int main(void)
{
    char request[ARGS_MAX];
    static assist_call_t calls[ASSIST_CALLS_MAX];

    getargs(request, sizeof(request));
    assist_init("[agent] ");

    if (request[0] == 0)
    {
        print("usage: agent TASK | agent --llm TASK | agent --call JSON | agent --tools\n");
        return 1;
    }

    if (strcmp(request, "--tools") == 0)
    {
        assist_list_tools();
        return 0;
    }

    if (starts_with(request, "--call "))
    {
        if (assist_parse_call(request + 7, &calls[0]) != 0)
        {
            print("[agent] bad tool call, expected {\"name\": ..., \"arguments\": {...}}\n");
            return 1;
        }

        return assist_run_calls(calls, 1);
    }

    if (starts_with(request, "--llm "))
    {
        return run_model(request + 6);
    }

    int count = assist_route(request, calls);

    if (count > 0)
    {
        print("[agent] plan (rules, no model needed):");

        for (int i = 0; i < count; i++)
        {
            print(" ");
            print(calls[i].name);
        }

        print("\n");
        assist_run_calls(calls, count);
        print("[agent] done\n");
        return 0;
    }

    return run_model(request);
}
