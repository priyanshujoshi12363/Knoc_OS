#include "ulib.h"
#include "llm.h"
#include "rag.h"

#define MAX_ANSWER 160
#define SYSTEM_PROMPT                                                                                     \
    "You are the KnocOS assistant, built into the KnocOS operating system. Answer briefly and clearly. " \
    "When facts about this computer are given, answer from them."
#define PREFIX_PATH "/tmp/ask-prefix.kv"

static llm_prompt_t prompt;
static rag_facts_t facts;

int main(void)
{
    char question[ARGS_MAX];
    unsigned long started = uptime();

    getargs(question, sizeof(question));

    if (question[0] == 0)
    {
        print("usage: ask QUESTION\n");
        return 1;
    }

    rag_collect(question, &facts);

    if (facts.count)
    {
        print("[ask] facts from the memory graph and the system:\n");

        for (int i = 0; i < facts.count; i++)
        {
            print("  - ");
            print(facts.text[i]);
            print("\n");
        }
    }

    if (llm_load() != 0)
    {
        print("ask: cannot load " LLM_MODEL_PATH " (make reset-disk DISK_MB=1024 puts it on the disk)\n");
        return 1;
    }

    llm_prompt_start(&prompt, SYSTEM_PROMPT);

    int prefix = prompt.count;

    if (facts.count)
    {
        llm_prompt_text(&prompt, "Facts about this computer:\n");

        for (int i = 0; i < facts.count; i++)
        {
            llm_prompt_text(&prompt, "- ");
            llm_prompt_text(&prompt, facts.text[i]);
            llm_prompt_text(&prompt, "\n");
        }

        llm_prompt_text(&prompt, "\nQuestion: ");
    }

    llm_prompt_text(&prompt, question);
    llm_prompt_end(&prompt, 0);

    unsigned long loaded = uptime();

    print("[ask] Qwen2.5-0.5B loaded in ");
    print_uint((loaded - started) / 100);
    print(" s, reading ");
    print_uint((unsigned long)prompt.count);
    print(" prompt tokens...\n");

    llm_read_prompt(&prompt, prefix, PREFIX_PATH);

    unsigned long thought = uptime();
    int produced = 0;

    while (produced < MAX_ANSWER)
    {
        int best = llm_best_token(0, 0);
        unsigned int length;

        if (llm_is_end(best))
        {
            break;
        }

        const unsigned char *text = llm_token_text(best, &length);

        write(FD_STDOUT, text, length);
        produced++;

        if (llm_accept(best) != 0)
        {
            break;
        }
    }

    unsigned long finished = uptime();

    print("\n[ask] ");
    print_uint((unsigned long)produced);
    print(" tokens in ");
    print_uint((finished - thought) / 100);
    print(" s (prompt ");
    print_uint((thought - loaded) / 100);
    print(" s)\n");
    return 0;
}
