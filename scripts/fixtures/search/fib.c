#include <stdio.h>

int fib(int n)
{
    return n < 2 ? n : fib(n - 1) + fib(n - 2);
}

int main(void)
{
    printf("%d\n", fib(20));
    return 0;
}
