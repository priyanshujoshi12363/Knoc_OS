#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include <assert.h>

static int passed;
static int failed;

static void check(int ok, const char *what)
{
    if (ok)
    {
        passed++;
    }
    else
    {
        failed++;
        printf("  FAIL %s\n", what);
    }
}

static void check_text(const char *got, const char *want, const char *what)
{
    if (strcmp(got, want) == 0)
    {
        passed++;
    }
    else
    {
        failed++;
        printf("  FAIL %s: got \"%s\", want \"%s\"\n", what, got, want);
    }
}

static int near(double a, double b)
{
    return fabs(a - b) < 1e-9 * (fabs(b) > 1 ? fabs(b) : 1);
}

static int compare_ints(const void *a, const void *b)
{
    int x = *(const int *)a;
    int y = *(const int *)b;

    return (x > y) - (x < y);
}

static void test_printf(void)
{
    char s[128];

    snprintf(s, sizeof(s), "%d|%5d|%-5d|%05d", 42, 42, 42, 42);
    check_text(s, "42|   42|42   |00042", "printf integers and widths");
    snprintf(s, sizeof(s), "%x|%X|%#x|%o|%u", 255, 255, 255, 8, 3000000000u);
    check_text(s, "ff|FF|0xff|10|3000000000", "printf hex, octal, unsigned");
    snprintf(s, sizeof(s), "%ld|%lld|%zu|%d", -1234567890123L, -9000000000000000000LL, (size_t)77, -5);
    check_text(s, "-1234567890123|-9000000000000000000|77|-5", "printf long sizes");
    snprintf(s, sizeof(s), "%c%c|%s|%.3s|%6s|%-6s|", 'O', 'K', "text", "abcdef", "ab", "ab");
    check_text(s, "OK|text|abc|    ab|ab    |", "printf chars and strings");
    snprintf(s, sizeof(s), "%.2f|%f|%.0f|%8.3f", 3.14159, 2.5, 2.5, -1.5);
    check_text(s, "3.14|2.500000|3|  -1.500", "printf fixed floats");
    snprintf(s, sizeof(s), "%e|%.2E|%g|%g|%g", 12345.678, 0.00123, 0.0001, 123456.0, 1e20);
    check_text(s, "1.234568e+04|1.23E-03|0.0001|123456|1e+20", "printf exponent floats");
    snprintf(s, sizeof(s), "%+d|% d|%%|%.5d", 7, 7, 42);
    check_text(s, "+7| 7|%|00042", "printf flags and precision");
    check(snprintf(s, 5, "%s", "truncated") == 9 && strcmp(s, "trun") == 0, "snprintf truncates and counts");
    snprintf(s, sizeof(s), "%p", (void *)0x1234);
    check_text(s, "0x1234", "printf pointer");
}

static void test_strings(void)
{
    char s[64];
    char t[64] = "a,b;;c";

    strcpy(s, "Knoc");
    strcat(s, "OS");
    check_text(s, "KnocOS", "strcpy and strcat");
    strncat(s, "12345", 2);
    check_text(s, "KnocOS12", "strncat");
    check(strlen(s) == 8 && strnlen(s, 3) == 3, "strlen and strnlen");
    check(strcmp("abc", "abd") < 0 && strncmp("abcx", "abcy", 3) == 0, "strcmp and strncmp");
    check(strcasecmp("HeLLo", "hello") == 0 && strncasecmp("ABx", "aby", 2) == 0, "case-insensitive compare");
    check(strchr(s, 'O') == s + 4 && strrchr("a/b/c", '/') != NULL && strchr(s, 'z') == NULL, "strchr and strrchr");
    check(strstr("hello world", "wor") != NULL && strstr("hello", "xyz") == NULL, "strstr");
    check(strspn("aaab", "a") == 3 && strcspn("abc,d", ",") == 3, "strspn and strcspn");

    char *first = strtok(t, ",;");
    char *second = strtok(NULL, ",;");
    char *third = strtok(NULL, ",;");

    check(first && second && third && strcmp(first, "a") == 0 && strcmp(second, "b") == 0 &&
              strcmp(third, "c") == 0 && strtok(NULL, ",;") == NULL,
          "strtok");

    char overlap[16] = "123456789";

    memmove(overlap + 2, overlap, 5);
    check_text(overlap, "121234589", "memmove with overlap");
    check(memcmp("abc", "abd", 3) < 0 && memchr("abcdef", 'd', 6) != NULL, "memcmp and memchr");

    char *copy = strdup("duplicate");

    check(copy && strcmp(copy, "duplicate") == 0, "strdup");
    free(copy);
    check(strcmp(strerror(ENOENT), "no such file or folder") == 0, "strerror");
}

static void test_ctype(void)
{
    check(isalpha('a') && !isalpha('1') && isdigit('7') && isspace('\t') && isxdigit('F'), "ctype classes");
    check(toupper('a') == 'A' && tolower('Q') == 'q' && toupper('1') == '1' && ispunct('!'), "ctype conversions");
}

static void test_memory(void)
{
    void *blocks[200];
    int ok = 1;

    for (int i = 0; i < 200; i++)
    {
        blocks[i] = malloc((size_t)(i * 13 + 1));
        ok &= blocks[i] != NULL && ((unsigned long)blocks[i] & 15) == 0;
        memset(blocks[i], i & 0xFF, (size_t)(i * 13 + 1));
    }

    for (int i = 0; i < 200 && ok; i++)
    {
        unsigned char *p = blocks[i];

        for (int j = 0; j < i * 13 + 1; j++)
        {
            ok &= p[j] == (unsigned char)(i & 0xFF);
        }
    }

    check(ok, "malloc: 200 aligned blocks keep their contents");

    void *before = blocks[100];

    for (int i = 0; i < 200; i++)
    {
        free(blocks[i]);
    }

    void *again = malloc(100 * 13 + 1);

    check(again != NULL && (char *)again <= (char *)before, "free gives memory back for reuse");
    free(again);

    int *zeros = calloc(1000, sizeof(int));
    int all_zero = zeros != NULL;

    for (int i = 0; zeros && i < 1000; i++)
    {
        all_zero &= zeros[i] == 0;
    }

    check(all_zero, "calloc returns zeroed memory");

    char *grow = malloc(8);

    strcpy(grow, "keep");
    grow = realloc(grow, 100000);
    check(grow && strcmp(grow, "keep") == 0, "realloc keeps the contents");
    free(grow);
    free(zeros);

    char *big = malloc(3 * 1024 * 1024);

    check(big != NULL, "malloc of 3 MiB");

    if (big)
    {
        big[3 * 1024 * 1024 - 1] = 1;
    }

    free(big);
}

static void test_numbers(void)
{
    char *end;

    check(atoi("  -123abc") == -123 && atol("99999999999") == 99999999999L, "atoi and atol");
    check(strtol("0x1F", &end, 0) == 31 && *end == 0, "strtol base 0 hex");
    check(strtol("777", NULL, 8) == 511 && strtol("z", NULL, 36) == 35, "strtol bases");
    check(strtoul("4000000000", NULL, 10) == 4000000000UL, "strtoul");
    check(strtol("abc", &end, 10) == 0 && strcmp(end, "abc") == 0, "strtol with no digits");
    check(near(strtod("3.25e2", &end), 325.0) && *end == 0 && near(atof("-0.5"), -0.5), "strtod and atof");
    check(abs(-4) == 4 && labs(-5L) == 5 && div(17, 5).quot == 3 && div(17, 5).rem == 2, "abs and div");

    int values[1000];

    srand(7);

    for (int i = 0; i < 1000; i++)
    {
        values[i] = rand() % 100000;
    }

    qsort(values, 1000, sizeof(int), compare_ints);

    int sorted = 1;

    for (int i = 1; i < 1000; i++)
    {
        sorted &= values[i - 1] <= values[i];
    }

    check(sorted, "qsort sorts 1000 numbers");

    int key = values[500];

    check(bsearch(&key, values, 1000, sizeof(int), compare_ints) != NULL, "bsearch finds a number");

    srand(1);

    int a = rand();

    srand(1);
    check(rand() == a, "rand repeats after srand");

    int x;
    long y;
    double z;
    char word[16];

    check(sscanf("12 -7 2.5 knoc", "%d %ld %lf %s", &x, &y, &z, word) == 4 && x == 12 && y == -7 && near(z, 2.5) &&
              strcmp(word, "knoc") == 0,
          "sscanf");

    char small[4];

    check(sscanf("sqrt 2", "%3s", small) == 1 && strcmp(small, "sqr") == 0 &&
              sscanf("sqrt 2", "%3s %d", small, &x) == 1,
          "sscanf field width");
}

static void test_math(void)
{
    check(near(sqrt(2.0), 1.4142135623730951), "sqrt");
    check(near(sin(M_PI / 6), 0.5) && near(cos(0), 1) && near(tan(M_PI / 4), 1), "sin, cos, tan");
    check(near(exp(1), M_E) && near(log(M_E), 1) && near(log10(1000), 3) && near(log2(8), 3), "exp and log");
    check(pow(2, 10) == 1024 && near(pow(2, 0.5), sqrt(2.0)) && near(pow(10, -2), 0.01), "pow");
    check(near(atan2(1, 1), M_PI / 4) && near(atan(1), M_PI / 4) && near(asin(1), M_PI / 2), "atan, atan2, asin");
    check(floor(-1.5) == -2 && ceil(1.2) == 2 && round(2.5) == 3 && round(-2.5) == -3 && near(fmod(7.5, 2), 1.5),
          "floor, ceil, round, fmod");
    check(near(tanh(0.5), 0.46211715726000974) && near(hypot(3, 4), 5), "tanh and hypot");
}

static void test_files(void)
{
    char line[64];
    FILE *f = fopen("/tmp/libctest.txt", "w");

    check(f != NULL, "fopen for writing");

    if (!f)
    {
        return;
    }

    fprintf(f, "line %d\n", 1);
    fputs("line 2\n", f);
    fputc('!', f);
    fclose(f);

    f = fopen("/tmp/libctest.txt", "a");
    fprintf(f, "\nappended\n");
    fclose(f);

    f = fopen("/tmp/libctest.txt", "r");
    check(f != NULL, "fopen for reading");

    if (!f)
    {
        return;
    }

    check(fgets(line, sizeof(line), f) && strcmp(line, "line 1\n") == 0, "fgets first line");
    check(fgets(line, sizeof(line), f) && strcmp(line, "line 2\n") == 0, "fgets second line");
    check(fgetc(f) == '!', "fgetc");
    check(ftell(f) == 15, "ftell");
    fseek(f, 0, SEEK_END);
    check(ftell(f) == 25, "fseek to the end");
    fseek(f, -9, SEEK_END);
    check(fgets(line, sizeof(line), f) && strcmp(line, "appended\n") == 0, "fseek back and append mode");
    check(fgetc(f) == EOF && feof(f), "end of file");
    rewind(f);

    char head[4] = {0};

    check(fread(head, 1, 3, f) == 3 && strcmp(head, "lin") == 0, "rewind and fread");
    fclose(f);
    check(remove("/tmp/libctest.txt") == 0, "remove");
    errno = 0;
    check(fopen("/tmp/nothing-here.txt", "r") == NULL && errno == ENOENT, "fopen of a missing file sets errno");
}

static void test_time(int argc, char **argv)
{
    char text[64];
    time_t zero = 0;
    time_t day = 86400 * 365 + 3600 * 5 + 61;

    check(time(NULL) >= 0 && clock() >= 0, "time and clock");
    strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S %a %b", gmtime(&zero));
    check_text(text, "1970-01-01 00:00:00 Thu Jan", "gmtime and strftime of 0");
    strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", gmtime(&day));
    check_text(text, "1971-01-01 05:01:01", "gmtime one year later");
    check(argc == 3 && strcmp(argv[1], "alpha") == 0 && atoi(argv[2]) == 42, "main gets argc and argv");
    check(getenv("HOME") && strcmp(getenv("HOME"), "/home") == 0 && getenv("NOPE") == NULL, "getenv");
    assert(passed > 0);
}

int main(int argc, char **argv)
{
    printf("libctest: testing the KnocOS C library\n");
    test_printf();
    test_strings();
    test_ctype();
    test_memory();
    test_numbers();
    test_math();
    test_files();
    test_time(argc, argv);
    printf("libctest: %d checks passed, %d failed\n", passed, failed);
    return failed;
}
