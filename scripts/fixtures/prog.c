#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct point
{
    char name[8];
    double x;
    double y;
} point_t;

static long fibonacci(int n)
{
    return n < 2 ? n : fibonacci(n - 1) + fibonacci(n - 2);
}

static int by_distance(const void *a, const void *b)
{
    const point_t *p = a;
    const point_t *q = b;
    double dp = sqrt(p->x * p->x + p->y * p->y);
    double dq = sqrt(q->x * q->x + q->y * q->y);

    return (dp > dq) - (dp < dq);
}

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 10;
    point_t points[3] = {{"far", 30, 40}, {"near", 3, 4}, {"mid", 6, 8}};
    char *buffer = malloc(64);
    FILE *f;

    printf("fibonacci(%d) = %ld\n", n, fibonacci(n));
    qsort(points, 3, sizeof(point_t), by_distance);
    printf("closest first: %s %s %s\n", points[0].name, points[1].name, points[2].name);

    f = fopen("result.txt", "w");
    fprintf(f, "compiled inside KnocOS\n");
    fclose(f);
    f = fopen("result.txt", "r");
    fgets(buffer, 64, f);
    fclose(f);
    printf("file says: %s", buffer);
    free(buffer);
    return 0;
}
