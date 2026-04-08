#include <iostream>

extern "C" int add(int a, int b);

int main()
{
    int s = add(3, 4);
    std::cout << "3 + 4 = " << s << std::endl;
    return 0;
}
