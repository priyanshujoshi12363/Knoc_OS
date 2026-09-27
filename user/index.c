#include <stdio.h>
#include <string.h>
#include "search.h"
#include "ulib.h"

static embed_model_t model;
static search_index_t index_data;

int main(int argc, char **argv)
{
    const char *command = argc > 1 ? argv[1] : "status";

    if (strcmp(command, "rebuild") == 0)
    {
        int added = 0;
        int removed = 0;

        if (embed_load(&model, EMBED_MODEL) != 0)
        {
            printf("index: cannot load the search model %s\n", EMBED_MODEL);
            return 1;
        }

        remove(INDEX_FILE);
        search_refresh(&index_data, &model, &added, &removed);
        search_save(&index_data);
        printf("index: %d files in %s indexed by meaning\n", added, INDEX_ROOT);
        return 0;
    }

    if (strcmp(command, "status") != 0)
    {
        printf("usage: index [status|rebuild]\n");
        return 1;
    }

    if (search_load(&index_data) != 0)
    {
        printf("index: not built yet (indexd builds it in the background, or run: index rebuild)\n");
        return 0;
    }

    int types[TYPE_COUNT] = {0};

    for (int i = 0; i < index_data.count; i++)
    {
        types[index_data.entries[i].type < TYPE_COUNT ? index_data.entries[i].type : 0]++;
    }

    printf("index: %d files from %s, model %s\n  ", index_data.count, INDEX_ROOT, EMBED_MODEL);

    for (int t = 0; t < TYPE_COUNT; t++)
    {
        if (types[t])
        {
            printf("%s %d  ", search_type_name((unsigned int)t), types[t]);
        }
    }

    printf("\n");
    return 0;
}
