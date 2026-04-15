struct S {
    int a;
    union {
        int b;
        int c;
    };
};

int main(void)
{
    struct S s;
    s.a = 1;
    s.b = 42;
    return s.b - 42;
}
