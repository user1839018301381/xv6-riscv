#define SBRK_ERROR ((char *)-1)

struct stat;

// システムコール
int fork(void);
int exit(int status) __attribute__((noreturn));
int wait(int *status_out);
int pipe(int pipe_fds[2]);
int write(int fd, const void *buffer, int byte_count);
int read(int fd, void *buffer, int byte_count);
int close(int fd);
int kill(int pid);
int exec(const char *path, char **argv);
int open(const char *path, int mode);
int mknod(const char *path, short major, short minor);
int unlink(const char *path);
int fstat(int fd, struct stat *status_out);
int link(const char *existing_path, const char *new_path);
int mkdir(const char *path);
int chdir(const char *path);
int dup(int fd);
int getpid(void);
char *sys_sbrk(int byte_count, int allocation_mode);
int pause(int tick_count);
int uptime(void);
int sync(void);

// ulib.c（ユーザ用ライブラリ）
int stat(const char *path, struct stat *status_out);
char *strcpy(char *destination, const char *source);
void *memmove(void *destination, const void *source, int byte_count);
char *strchr(const char *string, char character);
int strcmp(const char *left, const char *right);
char *gets(char *buffer, int max_length);
uint strlen(const char *string);
void *memset(void *destination, int value, uint byte_count);
int atoi(const char *string);
int memcmp(const void *left, const void *right, uint byte_count);
void *memcpy(void *destination, const void *source, uint byte_count);
char *sbrk(int byte_count);
char *sbrklazy(int byte_count);

// printf.c（書式付き出力）
void fprintf(int fd, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
void printf(const char *format, ...) __attribute__((format(printf, 1, 2)));

// umalloc.c（メモリアロケータ）
void *malloc(uint byte_count);
void free(void *address);
