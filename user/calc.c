#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int evaluate(const char *line, double *result)
{
    double a;
    double b;
    char op;
    char name[16];

    if (sscanf(line, "%15s %lf", name, &a) == 2 && strcmp(name, "sqrt") == 0)
    {
        *result = sqrt(a);
        return 0;
    }

    if (sscanf(line, "%lf %c %lf", &a, &op, &b) != 3)
    {
        return -1;
    }

    switch (op)
    {
    case '+':
        *result = a + b;
        return 0;
    case '-':
        *result = a - b;
        return 0;
    case '*':
        *result = a * b;
        return 0;
    case '/':
        *result = a / b;
        return 0;
    case '^':
        *result = pow(a, b);
        return 0;
    case '%':
        *result = fmod(a, b);
        return 0;
    default:
        return -1;
    }
}

int main(int argc, char **argv)
{
    char line[128];
    double result;

    if (argc > 1)
    {
        char joined[128] = "";

        for (int i = 1; i < argc; i++)
        {
            strncat(joined, argv[i], sizeof(joined) - strlen(joined) - 2);
            strcat(joined, " ");
        }

        if (evaluate(joined, &result) != 0)
        {
            fprintf(stderr, "calc: use A OP B (OP is + - * / ^ %%) or sqrt A\n");
            return 1;
        }

        printf("%g\n", result);
        return 0;
    }

    printf("calc: type A OP B (+ - * / ^ %%) or sqrt A, and quit to leave\n");

    while (1)
    {
        printf("calc> ");

        if (!fgets(line, sizeof(line), stdin) || strncmp(line, "quit", 4) == 0)
        {
            break;
        }

        if (evaluate(line, &result) == 0)
        {
            printf("= %g\n", result);
        }
        else if (line[0] != '\n')
        {
            printf("?  try 2 + 3 or sqrt 2\n");
        }
    }

    return 0;
}
