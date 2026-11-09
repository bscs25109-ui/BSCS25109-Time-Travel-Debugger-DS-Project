#include <iostream>
#include <unistd.h>        // Linux-only header (would fail on MSVC)
#include <sys/socket.h>    // Linux-only header
using namespace std;

int main()
{
    cout << "Hello from Linux, my process id is " << getpid() << endl;
    return 0;
}