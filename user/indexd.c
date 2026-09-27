#include <stdio.h>
#include "search.h"
#include "ulib.h"

#define POLL_TICKS 1000
#define START_DELAY_TICKS 2000

static embed_model_t model;
static search_index_t index_data;

int main(void)
{
    int added = 0;
    int removed = 0;

    setvbuf(stdout, 0, _IONBF, 0);
    sleep(START_DELAY_TICKS);

    if (embed_load(&model, EMBED_MODEL) != 0)
    {
        printf("[INDEX] no search model (%s): search by meaning is off\n", EMBED_MODEL);
        return 1;
    }

    search_load(&index_data);

    if (search_refresh(&index_data, &model, &added, &removed))
    {
        search_save(&index_data);
    }

    printf("[INDEX] ready: %d files in %s indexed by meaning\n", index_data.count, INDEX_ROOT);

    while (1)
    {
        sleep(POLL_TICKS);
        search_load(&index_data);

        if (search_refresh(&index_data, &model, &added, &removed))
        {
            search_save(&index_data);
        }
    }
}
