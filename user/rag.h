#ifndef RAG_H
#define RAG_H

#define RAG_FACTS_MAX 8
#define RAG_FACT_LENGTH 200

typedef struct rag_facts
{
    int count;
    char text[RAG_FACTS_MAX][RAG_FACT_LENGTH];
} rag_facts_t;

void rag_collect(const char *question, rag_facts_t *facts);

#endif
