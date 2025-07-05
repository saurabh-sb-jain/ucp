#define _GNU_SOURCE

#include <liburing.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ioctl.h>

typedef enum oper_ {
    OPER_READ = 1,
    OPER_WRITE = 2,
} oper_e;

typedef struct buf_info_ {
    uint8_t index;
    uint8_t oper;
    uint8_t status;
    uint8_t done;
    char *buffer;
    off_t offset;
    size_t len;
} buf_info_t;

typedef struct file_oper_ {
    int in_fd;
    int out_fd;
    uint8_t num_bufs;
    uint8_t queue_sz;
    off_t rd_offset;
    off_t wr_offset;
    size_t bufsize;
    buf_info_t *ud;
} file_oper_t;

off_t
get_file_sz(int fd) {
    struct stat st;

    if (fstat(fd, &st) < 0) {
        perror("fstat error");
        return -1;
    }
    if (S_ISBLK(st.st_mode)) {
        unsigned long long bytes;
        if (ioctl(fd, BLKGETSIZE64, &bytes) != 0) {
            perror("block get size ioctl error");
            return -1;
        }
        return bytes;
    } else if (S_ISREG(st.st_mode)) {
        return st.st_size;
    }

    return -1;
}

void
print_usage(char *cmd) {
    fprintf(stderr,
            "Usage: %s [-n bufs] [-s buf size] <source_file> <dest_file>\n",
            cmd);
    exit(-1);
}

#define DFLT_NUM_BUFS   1
#define DFLT_Q_SZ       8
#define DFLT_BUF_SZ     (1024*1024)

#define DBG_PRINTF(fmt, ...)      \
    if (debug) {                  \
        printf(fmt, __VA_ARGS__); \
    }                             \

int
main(int argc, char **argv) {
    struct io_uring ring;
    struct io_uring_sqe *sqe;
    struct io_uring_cqe *cqe;
    file_oper_t oper;
    int rc;
    long bytes_copied;
    off_t sz;
    char ch;
    char *src, *dst;
    bool debug = false;

    oper.num_bufs = DFLT_NUM_BUFS;
    oper.queue_sz = DFLT_Q_SZ;
    oper.bufsize = DFLT_BUF_SZ;

    if (argc < 3) {
        print_usage(argv[0]);
    }

    while ((ch = getopt(argc, argv, "dn:s:")) != -1) {
        switch(ch) {
            case 'n': oper.num_bufs = atoi(optarg); break;
            case 's': oper.bufsize = atol(optarg); break;
            case 'd': debug = true; break;
            default: print_usage(argv[0]);
        }
    }

    if (argc < optind + 2) {
        print_usage(argv[0]);
    }

    src = argv[optind];
    dst = argv[optind + 1];

    if (oper.bufsize < DFLT_BUF_SZ) { oper.bufsize = DFLT_BUF_SZ; }
    if (oper.num_bufs == 0) { oper.num_bufs = DFLT_NUM_BUFS; }
    if (oper.num_bufs > DFLT_NUM_BUFS) { oper.queue_sz = oper.num_bufs; }

    oper.in_fd = open(src, O_RDONLY | O_DIRECT);
    if (oper.in_fd < 0) {
        perror("open source file failed");
        exit(-1);
    }

    sz = get_file_sz(oper.in_fd);
    if (sz < 0) {
        fprintf(stderr, "get file:%s size error\n", src);
        close(oper.in_fd);
        exit(-1);
    }

    oper.out_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT, 0644);
    if (oper.out_fd < 0) {
        perror("open destination file failed");
        close(oper.in_fd);
        exit(-1);
    }

    if (sz < oper.bufsize) {
        oper.num_bufs = 1;
    }
    if (sz < (oper.num_bufs * oper.bufsize)) {
        oper.num_bufs = (sz/oper.bufsize);
	    if (sz % oper.bufsize > 0) {
            oper.num_bufs += 1;
	    }
    }

    DBG_PRINTF("src file: %s, dest file: %s\n", src, dst);
    DBG_PRINTF("Using bufs:%d of size:%lu\n", oper.num_bufs, oper.bufsize);

    oper.ud = (buf_info_t*)malloc(oper.num_bufs*sizeof(buf_info_t));
    for (int i = 0; i < oper.num_bufs; i++) {
        oper.ud[i].buffer = (char *)malloc(oper.bufsize);
        /* buffer needs to align at filesystem block size, using 4096 for now */
        rc = posix_memalign((void*)&(oper.ud[i].buffer), 4096, oper.bufsize);
    	if (rc != 0 || !oper.ud[i].buffer) {
            perror("buffer malloc error");
            goto quit;
        }
	    oper.ud[i].status = true;
	    oper.ud[i].index = i;
	    oper.ud[i].done = false;
    }

    oper.rd_offset = 0;
    oper.wr_offset = 0;

    rc = io_uring_queue_init(oper.queue_sz, &ring, 0);
    if (rc < 0) {
        perror("io_uring_queue_init");
        goto quit;
    }

    while (bytes_copied < sz) {
	    for (int i = 0; i < oper.num_bufs; i++) {
            if ((oper.ud[i].status == true) && (oper.ud[i].done == false) &&
                (oper.rd_offset < sz)) {

                sqe = io_uring_get_sqe(&ring);
                if (!sqe) {
                    fprintf(stderr, "io_uring_get_sqe failed\n");
                    goto quit;
                }

                off_t rem = sz - oper.rd_offset;
                size_t len = (rem > oper.bufsize ? oper.bufsize : rem);

                io_uring_prep_read(sqe, oper.in_fd, oper.ud[i].buffer, len,
                                   oper.rd_offset);

                oper.ud[i].index = i;
                oper.ud[i].oper = OPER_READ;
                oper.ud[i].offset = oper.rd_offset;
                oper.ud[i].len = len;
                oper.ud[i].done = false;

                io_uring_sqe_set_data(sqe, (void*)&(oper.ud[i]));
                oper.rd_offset += len;
                oper.ud[i].status = false;

                rc = io_uring_submit(&ring);
                if (rc < 0) {
                    perror("io_uring_submit read");
                    goto quit;
                }

                DBG_PRINTF("Read: buf:%d offset:%lu len:%ld\n", i,
                           oper.ud[i].offset, oper.ud[i].len);
            }
            /* Buffer finished prior read operation and ready for write */
            if (oper.ud[i].status == false && oper.ud[i].oper == OPER_READ &&
                oper.ud[i].done == true && oper.ud[i].offset == oper.wr_offset) {

                sqe = io_uring_get_sqe(&ring);
                if (!sqe) {
                    fprintf(stderr, "io_uring_get_sqe failed\n");
                    goto quit;
                }

                io_uring_prep_write(sqe, oper.out_fd, oper.ud[i].buffer,
                                    oper.ud[i].len, oper.wr_offset);

                oper.ud[i].oper = OPER_WRITE;
                oper.ud[i].offset = oper.wr_offset;
                oper.ud[i].done = false;

                io_uring_sqe_set_data(sqe, (void*)&(oper.ud[i]));
                rc = io_uring_submit(&ring);
                if (rc < 0) {
                    perror("io_uring_submit write");
                    goto quit;
                }

                DBG_PRINTF("Write: buf:%d offset:%lu len:%ld\n", i,
                           oper.ud[i].offset, oper.ud[i].len);
            }
        }

        rc = io_uring_wait_cqe(&ring, &cqe);
        if (rc < 0) {
            perror("io_uring_wait_cqe read");
            goto quit;
        }

	    int index = ((buf_info_t*)cqe->user_data)->index;
        if (cqe->res < 0) {
            fprintf(stderr, "comp err: %s, buf:%d oper:%d offset:%lu len:%ld\n",
			strerror(-cqe->res), index,
			((buf_info_t*)cqe->user_data)->oper,
			((buf_info_t*)cqe->user_data)->offset,
			((buf_info_t*)cqe->user_data)->len);
            io_uring_cqe_seen(&ring, cqe);
            goto quit;
        }

        if (((buf_info_t*)cqe->user_data)->oper == OPER_WRITE) {
            /* mark buffer available for new read */
            oper.ud[index].status = true;
            oper.ud[index].done = false;
            oper.wr_offset += cqe->res;
            bytes_copied += cqe->res;
            DBG_PRINTF("Write done: buf:%d offset:%lu exp_len:%lu len:%d\n",
                       index, oper.ud[index].offset, oper.ud[index].len,
                       cqe->res);
            io_uring_cqe_seen(&ring, cqe);
        }

        if (((buf_info_t*)cqe->user_data)->oper == OPER_READ) {
            DBG_PRINTF("Read done: buf:%d offset:%lu exp_len:%lu len:%d\n",
                       index, oper.ud[index].offset, oper.ud[index].len,
                       cqe->res);
            /* mark buffer ready for write */
            oper.ud[index].done = true;
            io_uring_cqe_seen(&ring, cqe);
        }
    }

    printf("Copied %ld bytes from '%s' to '%s'\n", bytes_copied, src, dst);

quit:
    for (int i = 0; i < oper.num_bufs; i++) {
        if (oper.ud[i].buffer) {
            free(oper.ud[i].buffer);
        }
    }
    free(oper.ud);
    close(oper.in_fd);
    fsync(oper.out_fd);
    close(oper.out_fd);
    io_uring_queue_exit(&ring);

    return 0;
}
