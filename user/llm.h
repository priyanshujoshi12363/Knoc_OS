#ifndef LLM_H
#define LLM_H

#define LLM_MODEL_PATH "/models/qwen.kllm"
#define LLM_MAX_PROMPT 700

typedef struct llm_prompt
{
    int ids[LLM_MAX_PROMPT];
    int count;
} llm_prompt_t;

typedef int (*llm_allow_t)(int token, const unsigned char *text, unsigned int length, void *context);

int llm_load(void);
void llm_prompt_start(llm_prompt_t *p, const char *system);
void llm_prompt_text(llm_prompt_t *p, const char *text);
void llm_prompt_end(llm_prompt_t *p, const char *answer_start);
void llm_read_prompt(const llm_prompt_t *p, int prefix, const char *cache_path);
int llm_best_token(llm_allow_t allow, void *context);
int llm_is_end(int token);
int llm_accept(int token);
const unsigned char *llm_token_text(int token, unsigned int *length);

#endif
