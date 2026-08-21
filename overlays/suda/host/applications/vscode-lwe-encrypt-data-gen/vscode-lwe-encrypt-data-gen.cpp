#include <libnvme.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <fstream>

namespace {

constexpr size_t kLbaSize = 4096;
const char* kDefaultInputPath = "testdata/plaintext_u8_4k.bin";

struct Options {
    const char* io_device = "nvmq0n1";
    const char* input_path = kDefaultInputPath;
    uint32_t storage_nsid = 1;
    uint64_t ssd_lba = 0;
    bool ssd_lba_set = false;
};

void print_usage(const char* argv0)
{
    fprintf(
        stderr,
        "Usage: %s [options]\n"
        "  --input FILE  4096-byte u8 plaintext file\n"
        "  --ssd-nsid N  destination SSD namespace id (default: 1)\n"
        "  --ssd-lba N   destination SSD logical block (required, overwritten)\n"
        "  --io DEV      NVMe I/O device (default: nvmq0n1)\n"
        "  --help        show this message\n",
        argv0);
}

bool parse_u64(const char* text, uint64_t* value)
{
    if (text == nullptr || *text == '\0') {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    unsigned long long parsed = strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }
    *value = static_cast<uint64_t>(parsed);
    return true;
}

bool parse_options(int argc, char** argv, Options* options)
{
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (strcmp(arg, "--help") == 0) {
            print_usage(argv[0]);
            exit(0);
        }
        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for %s\n", arg);
            return false;
        }

        const char* text = argv[++i];
        uint64_t value = 0;
        if (strcmp(arg, "--input") == 0) {
            options->input_path = text;
        } else if (strcmp(arg, "--io") == 0) {
            options->io_device = text;
        } else if (strcmp(arg, "--ssd-nsid") == 0) {
            if (!parse_u64(text, &value) || value == 0 || value > UINT32_MAX) {
                fprintf(stderr, "Invalid SSD namespace id: %s\n", text);
                return false;
            }
            options->storage_nsid = static_cast<uint32_t>(value);
        } else if (strcmp(arg, "--ssd-lba") == 0) {
            if (!parse_u64(text, &options->ssd_lba)) {
                fprintf(stderr, "Invalid SSD LBA: %s\n", text);
                return false;
            }
            options->ssd_lba_set = true;
        } else {
            fprintf(stderr, "Unknown option: %s\n", arg);
            return false;
        }
    }

    if (!options->ssd_lba_set) {
        fprintf(stderr, "--ssd-lba is required because the block is overwritten\n");
        return false;
    }
    return true;
}

bool read_plaintext(const char* path, void* buffer)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        fprintf(stderr, "Unable to open plaintext file: %s\n", path);
        return false;
    }

    file.read(static_cast<char*>(buffer), kLbaSize);
    if (file.gcount() != static_cast<std::streamsize>(kLbaSize)) {
        fprintf(stderr, "Plaintext file must contain exactly 4096 bytes: %s\n", path);
        return false;
    }
    char extra = 0;
    if (file.read(&extra, 1)) {
        fprintf(stderr, "Plaintext file is larger than 4096 bytes: %s\n", path);
        return false;
    }
    return true;
}

uint64_t fnv1a64(const void* buffer, size_t size)
{
    const uint8_t* bytes = static_cast<const uint8_t*>(buffer);
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

}  // namespace

int main(int argc, char** argv)
{
    Options options;
    if (!parse_options(argc, argv, &options)) {
        print_usage(argv[0]);
        return 1;
    }

    void* write_page = nullptr;
    void* read_page = nullptr;
    if (posix_memalign(&write_page, kLbaSize, kLbaSize) != 0 ||
        posix_memalign(&read_page, kLbaSize, kLbaSize) != 0) {
        fprintf(stderr, "Unable to allocate aligned 4KB buffers\n");
        free(write_page);
        free(read_page);
        return 1;
    }
    memset(read_page, 0, kLbaSize);

    if (!read_plaintext(options.input_path, write_page)) {
        free(write_page);
        free(read_page);
        return 1;
    }

    int io_fd = nvme_open(options.io_device);
    if (io_fd < 0) {
        fprintf(stderr, "Unable to open NVMe device: %s\n", options.io_device);
        free(write_page);
        free(read_page);
        return 1;
    }

    struct nvme_io_args args;
    memset(&args, 0, sizeof(args));
    args.args_size = sizeof(args);
    args.fd = io_fd;
    args.nsid = options.storage_nsid;
    args.slba = options.ssd_lba;
    args.data = write_page;
    args.data_len = kLbaSize;

    int ret = nvme_write(&args);
    if (ret != 0) {
        fprintf(stderr, "nvme_write failed: %d\n", ret);
        close(io_fd);
        free(write_page);
        free(read_page);
        return 1;
    }

    args.data = read_page;
    ret = nvme_read(&args);
    if (ret != 0) {
        fprintf(stderr, "nvme_read failed: %d\n", ret);
        close(io_fd);
        free(write_page);
        free(read_page);
        return 1;
    }

    if (memcmp(write_page, read_page, kLbaSize) != 0) {
        fprintf(stderr, "SSD readback verification failed\n");
        close(io_fd);
        free(write_page);
        free(read_page);
        return 1;
    }

    const uint8_t* bytes = static_cast<const uint8_t*>(write_page);
    printf("u8 plaintext SSD write passed\n");
    printf("input_file=%s\n", options.input_path);
    printf("ssd_nsid=%u ssd_lba=%llu bytes=%zu\n",
           options.storage_nsid,
           static_cast<unsigned long long>(options.ssd_lba),
           kLbaSize);
    printf("first_u8=%u\n", bytes[0]);
    printf("first_16_bytes=");
    for (size_t i = 0; i < 16; ++i) {
        printf("%02x", bytes[i]);
    }
    printf("\n");
    printf("fnv1a64=0x%016llx\n",
           static_cast<unsigned long long>(fnv1a64(write_page, kLbaSize)));
    printf("readback_verified=yes\n");

    close(io_fd);
    free(write_page);
    free(read_page);
    return 0;
}
