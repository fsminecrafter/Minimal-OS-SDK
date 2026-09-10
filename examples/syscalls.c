#include "minimalos.h"
#include "stdio.h"

int main(void) {
    printf("pid=%ld uptime=%ld\n", mos_getpid(), mos_uptime());

    const char* path = "0:/programs/hello.run";
    printf("%s: exists=%ld dir=%ld\n", path, mos_exists(path), mos_is_dir(path));

    int fd = (int)mos_open(path, SYS_O_RDONLY);
    if (fd < 0) {
        printf("open failed: %ld\n", (long)fd);
        return 1;
    }

    long file_size = mos_size(fd);
    char header[17];
    long bytes_read = mos_read(fd, header, sizeof(header) - 1);
    if (bytes_read > 0) {
        header[bytes_read] = '\0';
        printf("size=%ld first-bytes=%s\n", file_size, header);
    }

    mos_seek(fd, 0);
    mos_close(fd);
    return 0;
}
