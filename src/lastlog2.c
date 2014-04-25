#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>
#include <string.h>
#include <lastlog.h>
#include <errno.h>
#include <stdio.h>
#include <sys/uio.h>

#include "lastlog2.h"


#define QUOTE(name) #name
#define STR(macro) QUOTE(macro)

#define LASTLOG_PATH "/tmp/var/log/lastlog2/"

#define LASTLOG_PATH_LEN (sizeof (LASTLOG_PATH) + sizeof (STR (UID_MAX)) - 1)
#define LASTLOG_FILE_LEN (LASTLOG_PATH_LEN + sizeof(STR (UID_MAX)) - 1)

/* +1 for slash char */
#define LASTLOG_PATH_LEN_PLUS (LASTLOG_PATH + 1)
#define LASTLOG_FILE_LEN_PLUS (LASTLOG_FILE_LEN + 1)

#define sizeof_strs(x, y, z)    ((sizeof((x)) - 1) + (sizeof((y)) - 1) + (sizeof((z))))

#define EXTENSION_MAGIC 1

/* Internal functions */
static int putlstlogent_impl (const uid_t uid, const struct lastlog *const ll, const struct ll_extension *const ll_ex);
static int getlstlogent_impl (const uid_t uid, struct lastlog *const ll, struct ll_extension *const ll_ex);

static int try_create_lastlog_dir (const char *const ll_path);

static inline uid_t get_uid_dir (uid_t uid)
{
    return (uid - (uid % 1000));
}

static int try_create_lastlog_dir (const char *const ll_path)
{
    int checked = 0;
    /* We like C right? ; is just empty statement... is better than empty block. */
repeat: ;
    /* Here is little race condition */
    const int dir_fd = open (ll_path, O_DIRECTORY | O_NOFOLLOW);
    const int saved_errno = errno;

    if (!checked && (dir_fd == -1)) {
        if (mkdir (ll_path, S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH) != 0) {
            /* What if another process created directory? */
            if (errno == EEXIST) {
                checked = 1;
                /* FIX? Well. Someone could remove directory while another round of checking. */
                goto repeat;
            }
            return -1;
        }
        checked = 1;
        goto repeat;
    }

    if (dir_fd == -1) {
        errno = saved_errno;
        return -1;
    }

    return dir_fd;
}

static int check_extension(unsigned int extension_id)
{
    switch (extension_id) {
        case EXTENSION_MAGIC:
            return 1;
        default:
            return 0;
    }

    abort();
}

inline int getlstlogent (const uid_t uid, struct lastlog *const ll)
{
    return getlstlogent_impl (uid, ll, NULL);
}

inline int getlstlogentx (const uid_t uid, struct lastlog *const ll, struct ll_extension *const ll_ex)
{
    return getlstlogent_impl (uid, ll, ll_ex);
}

static int getlstlogent_impl (const uid_t uid, struct lastlog *const ll, struct ll_extension *const ll_ex)
{
    assert (ll != NULL);

    int saved_errno = 0;

    const uid_t uid_dir = get_uid_dir (uid);
    char path[LASTLOG_FILE_LEN_PLUS] = {0};
    sprintf (path, "%s%u/%u", LASTLOG_PATH, uid_dir, uid);

    const int ll_fd = open (path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (ll_fd == -1) {
        return -1;
    }

    /* All other attributes are set by initialization. */
    struct flock ll_lock = {0};
    ll_lock.l_type = F_RDLCK;
    ll_lock.l_whence = SEEK_SET;
    /*
       As well as being removed by an explicit F_UNLCK, record locks are auto‐
       matically released when the process terminates or if it closes any file
       descriptor referring to a file on which locks are held.
    */
    if (fcntl (ll_fd, F_SETLK, &ll_lock) == -1) {
        saved_errno = errno;
        close (ll_fd);
        errno = saved_errno;
        return -1;
    }

    struct stat st = {0};
    if ((fstat (ll_fd, &st) == -1) 
        || (st.st_size < (off_t)sizeof (*ll)))
    {
        saved_errno = errno;
        close (ll_fd);
        errno = saved_errno;
        return -1;
    }

    /* Plain old format. */
    if (st.st_size == sizeof(*ll)) {
        memset (ll, 0, sizeof(*ll));
        const ssize_t n = read (ll_fd, ll, sizeof(*ll));
        if ((n == -1) || (n != sizeof(*ll))) {
            close (ll_fd);
            return -1;
        }

        close (ll_fd);
        return 1;
    }

    /* Don't care about extensions. But size if bigger than expected... */
    if (ll_ex == NULL) {
        close (ll_fd);
        return -1;
    }

    /* Format with extensions */
    if (st.st_size < (off_t)(sizeof (*ll) + sizeof (*ll_ex))) {
        close (ll_fd);
        return -1;
    }

    struct iovec iov[2] = {
        { .iov_base = (void *) ll, .iov_len = sizeof (*ll) },
        { .iov_base = (void *) ll_ex, .iov_len = sizeof (*ll_ex) }
    };

    memset (ll, 0, sizeof (*ll));
    memset (ll_ex, 0, sizeof (*ll_ex));
    const ssize_t n = readv (ll_fd, iov, (sizeof (iov) / sizeof (iov[0])));
    /* Allow more extension in future. */
    if ((n == -1) || (n < (ssize_t)(sizeof (*ll) + sizeof (*ll_ex)))) {
        close (ll_fd);
        return -1;
    }
    close (ll_fd);

    if (check_extension (ll_ex->extension_id))
    {
        return 1;
    }

    /* Be like negative at all cost. */
    return -1;
}

inline int putlstlogent (const uid_t uid, const struct lastlog *const ll)
{
    return putlstlogent_impl (uid, ll, NULL);
}

inline int putlstlogentx (const uid_t uid, const struct lastlog *const ll, const struct ll_extension *const ll_ex)
{
    return putlstlogent_impl (uid, ll, ll_ex);
}

static int putlstlogent_impl (const uid_t uid, const struct lastlog *const ll, const struct ll_extension *const ll_ex)
{
    assert (ll != NULL);

    int saved_errno = 0;

    const uid_t uid_dir = get_uid_dir (uid);

    /* No backslash. */
    char ll_path[LASTLOG_PATH_LEN] = {0};
    sprintf (ll_path, "%s%u", LASTLOG_PATH, uid_dir);

    const int dir_fd = try_create_lastlog_dir (ll_path);
    if (dir_fd == -1) {
        return -1;
    }

    /* ... + 1 for slash char */
    char ll_file [ sizeof_strs("/proc/self/fd/", STR (INT_MAX), STR (UID_MAX)) + 1] = {0};
    sprintf (ll_file, "/proc/self/fd/%u/%u", dir_fd, uid);
    const int ll_fd = open (ll_file, O_WRONLY | O_CREAT | O_CLOEXEC | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (ll_fd == -1) {
        return -1;
    }

    /* All other attributes are set by initialization. */
    struct flock ll_lock = {0};
    ll_lock.l_type = F_WRLCK;
    ll_lock.l_whence = SEEK_SET;
    /* From man pages:
       As well as being removed by an explicit F_UNLCK, record locks are auto‐
       matically released when the process terminates or if it closes any file
       descriptor referring to a file on which locks are held.
    */
    if (fcntl (ll_fd, F_SETLKW, &ll_lock) == -1) {
        saved_errno = errno;
        close (ll_fd);
        errno = saved_errno;
        return -1;
    }

    /* Don't care about extension. */
    if (ll_ex == NULL) {
        const ssize_t n = write (ll_fd, ll, sizeof (*ll));
        saved_errno = errno;
        close (ll_fd);
        /* Allow reading of extended record as a non-extended record. */
        if ((n == -1) || (n < (ssize_t)(sizeof(*ll)))) {
            close (dir_fd);
            errno = saved_errno;
            return -1;
        }

        close (dir_fd);
        return 1;

    } else {

        struct iovec iov[2] = {
            { .iov_base = (void *) ll, .iov_len = sizeof (*ll) },
            { .iov_base = (void *) ll_ex, .iov_len = sizeof (*ll_ex) }
        };
        const ssize_t n = writev (ll_fd, iov, (sizeof (iov) / sizeof (iov[0])));
        saved_errno = errno;
        close (ll_fd);
        if ((n == -1) || (n < (ssize_t)(sizeof (*ll) + sizeof (*ll_ex))))
        {
            close (dir_fd);
            errno = saved_errno;
            return -1;
        }

        close (dir_fd);
        return 1;
    }

    return -1;
}
