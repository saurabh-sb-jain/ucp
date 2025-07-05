# ucp
```
Tool to fast copy large regular and block device files.
Typically useful when one has to copy file of size 100s of GB or TB+ over parallel NFS.

Dependency: Install liburing

Compile: gcc -O2 -o ucp ucp.c -luring

Usage: ./ucp [-n bufs] [-s buf size] <source_file> <dest_file>

bufs is the number of parallel buffers to use for reading from source file
buf size is the size of each parallel buffer
product of bufs and buf size should be less than the available memory

Performance Comparison: Using file size ~1165 GiB, drop caches after each test

1) Linux cp - Time taken: 48m 23s = 2903s (411 MiB/s)

$ date; cp /mnt/cl1/training/model405b.tar /mnt/cl1/backup/model405b.tar; date
Fri Jul  1 07:29:22 PM UTC 2025
Fri Jul  1 08:17:45 PM UTC 2025

2) Linux dd - Time taken: 34m 48s = 2088s (572 MiB/s)

$ date; dd if=/mnt/cl1/training/model405b.tar of=/mnt/cl1/backup/model405b.tar bs=1G oflag=direct; date
Fri Jul  1 08:38:43 PM UTC 2025
Fri Jul  1 09:13:31 PM UTC 2025

3) ucp - Time taken: = 7m 51s = 471 sec (2534 MiB/s)
$ date; ./ucp -n 16 -s 1073741824 /mnt/cl1/training/model405b.tar /mnt/cl1/backup/model405b.tar; date
Sat Jul  1 12:18:22 AM UTC 2025
Sat Jul  1 12:26:13 AM UTC 2025

ucp vs. cp = 6.16x
ucp vs. dd = 4.43x
```
