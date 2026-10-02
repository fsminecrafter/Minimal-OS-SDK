#include "dlr_port.h"
#include "dlr_server.h"

#include "minimalos.h"
#include "net.h"
#include "string.h"
#include <stdio.h>

int main(int argc, char** argv) {
    printf("dlrconnect: entered argc=%d\n", argc);
    if (argc < 4 || strcmp(argv[1], "--conn") != 0) return 2;
    printf("dlrconnect: starting child conn=%s ip=%s\n", argv[2], argv[3]);
    if (dlr_port_init() != 0) return 1;
    return dlr_server_child(argc, argv);
}