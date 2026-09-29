/*
 * pci_resource.c
 *
 * PCI sysfs resource(BAR) read/write utility
 *
 * Features
 *   - PCI BDF 지정 (01:00.0 / 0000:01:00.0)
 *   - resource 번호 지정
 *   - 8/16/32/64-bit access
 *   - BAR 일부만 mmap()
 *   - mmap page alignment 처리
 *   - BAR physical address / size 표시
 *   - read-only dump
 *   - register write
 *   - offset/range/alignment 검사
 *
 * Build:
 *   gcc -Wall -Wextra -O2 -o pci_resource pci_resource.c
 *
 * Examples:
 *
 *   32-bit dump:
 *     sudo ./pci_resource 01:00.0 0 0x10000 64
 *
 *   8-bit dump:
 *     sudo ./pci_resource -b 8 01:00.0 0 0x10000 64
 *
 *   16-bit dump:
 *     sudo ./pci_resource -b 16 01:00.0 0 0x10000 64
 *
 *   64-bit dump:
 *     sudo ./pci_resource -b 64 01:00.0 0 0x10000 64
 *
 *   Write:
 *     sudo ./pci_resource -b 32 -w 0x12345678 01:00.0 0 0x10000
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>
#include <inttypes.h>
#include <getopt.h>
#include <limits.h>

#define SYSFS_PCI_PATH "/sys/bus/pci/devices"
#define PATH_SIZE 256

static void usage(const char *prog)
{
    printf(
        "PCI resource read/write utility\n"
        "\n"
        "READ:\n"
        "  %s [-b 8|16|32|64] [-l 4|8|16|32] "
        "<BDF> <resource> <offset> <count>\n"
        "\n"
        "WRITE:\n"
        "  %s [-b 8|16|32|64] -w <value> "
        "<BDF> <resource> <offset>\n"
        "\n"
        "Options:\n"
        "  -b <bits>     Access width (8,16,32,64), default=32\n"
        "  -l <words>    Words per line (4,8,16,32), default=4\n"
        "  -w <value>    Write value\n"
        "  -h            Help\n"
        "\n"
        "Examples:\n"
        "  %s 01:00.0 0 0x0000 64\n"
        "  %s -l 8 01:00.0 0 0x0000 64\n"
        "  %s -b 16 -l 16 01:00.0 2 0x100 64\n"
        "  %s -b 32 -l 32 01:00.0 0 0x1000 256\n"
        "  %s -b 32 -w 0x12345678 01:00.0 0 0x100\n"
        "\n",
        prog, prog,
        prog, prog, prog, prog, prog);
}

static int parse_ulong(const char *str, unsigned long *value)
{
    char *end;

    errno = 0;

    *value = strtoul(str, &end, 0);

    if (errno != 0 || *end != '\0')
    {
        return -1;
    }

    return 0;
}

static int parse_u64(const char *str, uint64_t *value)
{
    char *end;

    errno = 0;

    *value = strtoull(str, &end, 0);

    if (errno != 0 || *end != '\0')
    {
        return -1;
    }

    return 0;
}

/*
 * /sys/bus/pci/devices/<BDF>/resource 파일에서
 * BAR physical start/end/flags 읽기
 */
static int get_resource_info(const char *pci_bdf,
                             unsigned long resource_num,
                             uint64_t *start,
                             uint64_t *end,
                             uint64_t *flags)
{
    char path[PATH_SIZE];
    FILE *fp;
    unsigned long long s;
    unsigned long long e;
    unsigned long long f;
    unsigned long index = 0;

    snprintf(path,
             sizeof(path),
             SYSFS_PCI_PATH "/%s/resource",
             pci_bdf);

    fp = fopen(path, "r");

    if (!fp)
    {
        perror("fopen(resource)");
        return -1;
    }

    while (fscanf(fp,
                  "%llx %llx %llx",
                  &s,
                  &e,
                  &f) == 3)
    {

        if (index == resource_num)
        {
            *start = s;
            *end = e;
            *flags = f;

            fclose(fp);
            return 0;
        }

        index++;
    }

    fclose(fp);

    fprintf(stderr,
            "Error: resource%lu not found\n",
            resource_num);

    return -1;
}

int main(int argc, char *argv[])
{
    int opt;
    int fd = -1;

    unsigned int bits = 32;
    size_t access_size = 4;

    int write_mode = 0;
    uint64_t write_value = 0;

    char pci_bdf[32];
    char resource_path[PATH_SIZE];

    unsigned long resource_num;
    unsigned long offset;
    unsigned long count = 1;
    unsigned int words_per_line = 4;

    uint64_t bar_start;
    uint64_t bar_end;
    uint64_t bar_flags;
    uint64_t bar_size;

    long page_size;

    uint64_t map_offset;
    size_t page_delta;
    size_t requested_size;
    size_t map_size;

    void *map_base = MAP_FAILED;
    volatile uint8_t *access_base;

    unsigned long i;

    /*
     * Options
     */
    while ((opt = getopt(argc, argv, "b:l:w:h")) != -1)
    {

        switch (opt)
        {

        case 'b':

            bits = (unsigned int)strtoul(optarg, NULL, 0);

            if (bits != 8 &&
                bits != 16 &&
                bits != 32 &&
                bits != 64)
            {

                fprintf(stderr,
                        "Error: access width must be "
                        "8, 16, 32 or 64\n");

                return 1;
            }

            break;

        case 'l':

            words_per_line =
                (unsigned int)strtoul(optarg, NULL, 0);

            if (words_per_line != 4 &&
                words_per_line != 8 &&
                words_per_line != 16 &&
                words_per_line != 32)
            {

                fprintf(stderr,
                        "Error: words per line must be "
                        "4, 8, 16 or 32\n");

                return 1;
            }

            break;

        case 'w':

            if (parse_u64(optarg, &write_value) < 0)
            {

                fprintf(stderr,
                        "Invalid write value: %s\n",
                        optarg);

                return 1;
            }

            write_mode = 1;
            break;

        case 'h':

            usage(argv[0]);
            return 0;

        default:

            usage(argv[0]);
            return 1;
        }
    }
    access_size = bits / 8;

    /*
     * Argument count
     */
    if (write_mode)
    {

        if ((argc - optind) != 3)
        {
            usage(argv[0]);
            return 1;
        }
    }
    else
    {

        if ((argc - optind) != 4)
        {
            usage(argv[0]);
            return 1;
        }
    }

    /*
     * BDF
     *
     * 01:00.0
     * ->
     * 0000:01:00.0
     */
    if (strlen(argv[optind]) == 7)
    {

        snprintf(pci_bdf,
                 sizeof(pci_bdf),
                 "0000:%s",
                 argv[optind]);
    }
    else
    {

        snprintf(pci_bdf,
                 sizeof(pci_bdf),
                 "%s",
                 argv[optind]);
    }

    /*
     * resource number
     */
    if (parse_ulong(argv[optind + 1],
                    &resource_num) < 0)
    {

        fprintf(stderr,
                "Invalid resource number\n");

        return 1;
    }

    /*
     * offset
     */
    if (parse_ulong(argv[optind + 2],
                    &offset) < 0)
    {

        fprintf(stderr,
                "Invalid offset\n");

        return 1;
    }

    /*
     * count
     */
    if (!write_mode)
    {

        if (parse_ulong(argv[optind + 3],
                        &count) < 0)
        {

            fprintf(stderr,
                    "Invalid count\n");

            return 1;
        }

        if (count == 0)
        {

            fprintf(stderr,
                    "Error: count must be > 0\n");

            return 1;
        }
    }

    /*
     * Alignment check
     */
    if (offset % access_size)
    {

        fprintf(stderr,
                "Error: offset 0x%lX is not "
                "%zu-byte aligned\n",
                offset,
                access_size);

        return 1;
    }

    /*
     * resource path
     */
    snprintf(resource_path,
             sizeof(resource_path),
             SYSFS_PCI_PATH "/%s/resource%lu",
             pci_bdf,
             resource_num);

    /*
     * BAR physical information
     */
    if (get_resource_info(pci_bdf,
                          resource_num,
                          &bar_start,
                          &bar_end,
                          &bar_flags) < 0)
    {

        return 1;
    }

    if (bar_start == 0 && bar_end == 0)
    {

        fprintf(stderr,
                "Error: resource%lu is not assigned\n",
                resource_num);

        return 1;
    }

    bar_size = bar_end - bar_start + 1;

    /*
     * Requested byte size
     */
    if (write_mode)
    {

        requested_size = access_size;
    }
    else
    {

        if (count > SIZE_MAX / access_size)
        {

            fprintf(stderr,
                    "Error: requested size overflow\n");

            return 1;
        }

        requested_size = count * access_size;
    }

    /*
     * BAR range check
     */
    if ((uint64_t)offset >= bar_size ||
        requested_size >
            bar_size - (uint64_t)offset)
    {

        fprintf(stderr,
                "Error: requested range exceeds BAR\n");

        fprintf(stderr,
                "BAR size : 0x%" PRIX64 "\n",
                bar_size);

        return 1;
    }

    /*
     * Write value range check
     */
    if (write_mode && bits < 64)
    {

        uint64_t max_value =
            (1ULL << bits) - 1;

        if (write_value > max_value)
        {

            fprintf(stderr,
                    "Error: value 0x%" PRIX64
                    " exceeds %u-bit range\n",
                    write_value,
                    bits);

            return 1;
        }
    }

    /*
     * Page size
     */
    page_size = sysconf(_SC_PAGESIZE);

    if (page_size <= 0)
    {

        perror("sysconf");
        return 1;
    }

    /*
     * mmap offset must be page aligned.
     *
     * Example:
     *
     * offset       = 0x1234
     * page_size    = 0x1000
     *
     * map_offset   = 0x1000
     * page_delta   = 0x0234
     */
    map_offset =
        ((uint64_t)offset / page_size) * page_size;

    page_delta =
        (size_t)((uint64_t)offset - map_offset);

    /*
     * Actual mmap size
     */
    if (requested_size > SIZE_MAX - page_delta)
    {

        fprintf(stderr,
                "Error: mmap size overflow\n");

        return 1;
    }

    map_size =
        page_delta + requested_size;

    /*
     * Open resource
     *
     * READ  -> O_RDONLY
     * WRITE -> O_RDWR
     */
    if (write_mode)
    {

        fd = open(resource_path,
                  O_RDWR | O_SYNC);
    }
    else
    {

        fd = open(resource_path,
                  O_RDONLY | O_SYNC);
    }

    if (fd < 0)
    {

        perror(resource_path);
        return 1;
    }

    /*
     * mmap
     */
    map_base = mmap(NULL,
                    map_size,
                    write_mode
                        ? (PROT_READ | PROT_WRITE)
                        : PROT_READ,
                    MAP_SHARED,
                    fd,
                    (off_t)map_offset);

    if (map_base == MAP_FAILED)
    {

        perror("mmap");
        close(fd);

        return 1;
    }

    /*
     * Actual requested offset
     */
    access_base =
        (volatile uint8_t *)map_base + page_delta;

    /*
     * Information
     */
    printf("\n");
    printf("PCI Resource Information\n");
    printf("----------------------------------------\n");

    printf("PCI device     : %s\n",
           pci_bdf);

    printf("Resource       : resource%lu\n",
           resource_num);

    printf("Resource path  : %s\n",
           resource_path);

    printf("BAR phys start : 0x%016" PRIX64 "\n",
           bar_start);

    printf("BAR phys end   : 0x%016" PRIX64 "\n",
           bar_end);

    printf("BAR size       : 0x%" PRIX64
           " (%" PRIu64 " bytes)\n",
           bar_size,
           bar_size);

    printf("BAR flags      : 0x%016" PRIX64 "\n",
           bar_flags);

    printf("Access width   : %u-bit\n",
           bits);

    printf("Offset         : 0x%08lX\n",
           offset);

    printf("Physical addr  : 0x%016" PRIX64 "\n",
           bar_start + offset);

    printf("Page size      : 0x%lX\n",
           page_size);

    printf("mmap offset    : 0x%016" PRIX64 "\n",
           map_offset);

    printf("mmap size      : 0x%zX\n",
           map_size);
           
    printf("Words per line : %u\n",
           words_per_line);
    printf("----------------------------------------\n\n");

    /*
     * WRITE
     */
    if (write_mode)
    {

        printf("WRITE\n");
        printf("----------------------------------------\n");

        printf("Offset : 0x%08lX\n",
               offset);

        printf("Value  : ");

        switch (bits)
        {

        case 8:

            printf("0x%02" PRIX8 "\n",
                   (uint8_t)write_value);

            *(volatile uint8_t *)access_base =
                (uint8_t)write_value;

            break;

        case 16:

            printf("0x%04" PRIX16 "\n",
                   (uint16_t)write_value);

            *(volatile uint16_t *)access_base =
                (uint16_t)write_value;

            break;

        case 32:

            printf("0x%08" PRIX32 "\n",
                   (uint32_t)write_value);

            *(volatile uint32_t *)access_base =
                (uint32_t)write_value;

            break;

        case 64:

            printf("0x%016" PRIX64 "\n",
                   write_value);

            *(volatile uint64_t *)access_base =
                write_value;

            break;
        }

        /*
         * Ensure compiler does not move memory
         * accesses across this point.
         */
        __sync_synchronize();

        printf("\nWrite completed.\n");
    }

    /*
     * READ / DUMP
     */
    else
    {
        printf("DUMP\n");
        printf("----------------------------------------\n");

        for (i = 0; i < count; i++)
        {

            if ((i % words_per_line) == 0)
            {

                printf("%08lX: ",
                       offset + i * access_size);
            }

            switch (bits)
            {

            case 8:

                printf("%02" PRIX8 " ",
                       ((volatile uint8_t *)
                            access_base)[i]);

                break;

            case 16:

                printf("%04" PRIX16 " ",
                       ((volatile uint16_t *)
                            access_base)[i]);

                break;

            case 32:

                printf("%08" PRIX32 " ",
                       ((volatile uint32_t *)
                            access_base)[i]);

                break;

            case 64:

                printf("%016" PRIX64 " ",
                       ((volatile uint64_t *)
                            access_base)[i]);

                break;
            }

            if ((i % words_per_line) ==
                (words_per_line - 1))
            {

                printf("\n");
            }
        }

        if ((i % words_per_line) != 0)
        {
            printf("\n");
        }
    }

    /*
     * Cleanup
     */
    munmap(map_base,
           map_size);

    close(fd);

    return 0;
}
