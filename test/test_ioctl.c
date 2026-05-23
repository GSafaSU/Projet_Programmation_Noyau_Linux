#include <stdio.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>  


#define OUICHEFS_IOC_MAGIC 'W'
#define OUICHEFS_IOC_GET_EXTENTS _IO(OUICHEFS_IOC_MAGIC, 0)

int main(int argc, char *argv[])
{
    if (argc != 2) {
        printf("Usage: %s <fichier>\n", argv[0]);
        return 1;
    }

    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    if (ioctl(fd, OUICHEFS_IOC_GET_EXTENTS) < 0) {
        perror("ioctl");
        return 1;
    }

    printf("Résultat dans dmesg\n");
    close(fd);
    return 0;
}