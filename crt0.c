#include "minimalos.h"

extern int main(int argc, char** argv);

void _start(long argc, char** argv) {
    int rc = main((int)argc, argv);
    mos_exit(rc);
}
