/* Copyright (C) 2013, F-Secure Corporation */

#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/file.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
#include <pthread.h>
#include <errno.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <regex.h>
#include <fsdyn/date.h>
#include <fsdyn/fsalloc.h>
#include "fstrace.h"
#include "fstrace_imp.h"

#define straux(s) #s
#define stringify(s) straux(s)
const char *fstrace_version_tag = "F-S_v:: fstrace " stringify(BUILD);

static enum {
    FSTRACE_STATE_UNINITIALIZED,
    FSTRACE_STATE_INITIALIZED,
    FSTRACE_STATE_ERRORED
} fstrace_state = FSTRACE_STATE_UNINITIALIZED;

static sigset_t SIG_MASK;
static int THE_MUTEX; /* serializes processes */
static pthread_mutex_t the_mutex =
    PTHREAD_MUTEX_INITIALIZER;  /* serializes threads */
static char lock_path[1000] = "/tmp/fstrace.lock.XXXXXX";
static uid_t THE_USER_ID;
static gid_t THE_GROUP_ID;

static void __attribute__((constructor)) unique_constructor(void)
{
    sigfillset(&SIG_MASK);
    sigdelset(&SIG_MASK, SIGSEGV);
    sigdelset(&SIG_MASK, SIGBUS);
    THE_USER_ID = geteuid();
    THE_GROUP_ID = getegid();
}

static void __attribute__((destructor)) unique_destructor(void)
{
    if (fstrace_state == FSTRACE_STATE_INITIALIZED)
        unlink(lock_path);
}

unsigned FSTRACE_FAILURE_LINE;  /* for the debugger */

#define FSTRACE_FAIL() do {                     \
        FSTRACE_FAILURE_LINE = __LINE__;        \
        fstrace_state = FSTRACE_STATE_ERRORED;  \
    } while (false)

uint64_t fstrace_get_unique_id()
{
    sigset_t old_mask;
    int status = pthread_sigmask(SIG_BLOCK, &SIG_MASK, &old_mask);
    assert(status >= 0);
    int err = pthread_mutex_lock(&the_mutex);
    assert(err == 0);
    static uint64_t next_id;
    uint64_t raw_id = next_id++;
    err = pthread_mutex_unlock(&the_mutex);
    assert(err == 0);
    status = pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
    assert(status >= 0);
    return (unsigned long) getpid() * 1000000 +
        (raw_id & 0x3ffff) + (raw_id & ~0x3ffff) * 1410065407;
}

static fstrace_memblock_t *replenish_mempool(fstrace_t *trace)
{
    fstrace_memblock_t *block = fsalloc(sizeof *block);
    block->start = mmap(NULL, BLOCK_SIZE, PROT_READ | PROT_WRITE,
                        MAP_ANON | MAP_SHARED, 0, 0);
    assert(block->start != NULL); 
    block->free = block->start;
    list_append(trace->mempool, block);
    return block;
}

static void *shared_alloc(fstrace_t *trace, size_t size)
{
    /* assert: trace is locked */
    assert(size <= BLOCK_SIZE);
    fstrace_memblock_t *block = (fstrace_memblock_t *)
        list_elem_get_value(list_get_last(trace->mempool));
    size_t remaining = block->start + BLOCK_SIZE - block->free;
    if (remaining < size)
        block = replenish_mempool(trace);
    void *obj = block->free;
    block->free += (size + ALIGNMENT_MASK) & ~ALIGNMENT_MASK;
    return obj;
}

void fstrace_set_lock_path(const char *pathname)
{
    switch (fstrace_state) {
        case FSTRACE_STATE_UNINITIALIZED:
            assert(strlen(pathname) < sizeof lock_path);
            strcpy(lock_path, pathname);
            fstrace_state = FSTRACE_STATE_INITIALIZED;
            THE_MUTEX = open(lock_path, O_CREAT | O_WRONLY, 0600);
            if (THE_MUTEX < 0)
                FSTRACE_FAIL();
            else if (fchown(THE_MUTEX, THE_USER_ID, THE_GROUP_ID) < 0)
                FSTRACE_FAIL();
            break;
        case FSTRACE_STATE_INITIALIZED:
            assert(false);
            break;
        default:
            assert(true);
    }
}

static void delayed_init()
{
    if (fstrace_state != FSTRACE_STATE_UNINITIALIZED)
        return;
    fstrace_state = FSTRACE_STATE_INITIALIZED;
    THE_MUTEX = mkstemp(lock_path);
    if (THE_MUTEX < 0)
        FSTRACE_FAIL();
    else if (fchown(THE_MUTEX, THE_USER_ID, THE_GROUP_ID) < 0)
        FSTRACE_FAIL();
}

static fstrace_t *do_open(const char *pathname_prefix, ssize_t rotate_size,
                          FILE *outf)
{
    delayed_init();
    fstrace_t *trace = fsalloc(sizeof *trace);
    trace->mempool = make_list();
    replenish_mempool(trace);
    trace->pathname_prefix = strdup(pathname_prefix);
    trace->rotate_size = rotate_size;
    trace->events = make_list();
    trace->shared_ordinal = shared_alloc(trace, sizeof *trace->shared_ordinal);
    trace->ordinal = *trace->shared_ordinal = 0;
    trace->params = shared_alloc(trace, sizeof *trace->params);
    trace->params->uid = THE_USER_ID;
    trace->params->gid = THE_GROUP_ID;
    trace->params->max_files = -1;
    trace->params->max_seconds = -1;
    trace->params->max_bytes = -1;
    if (outf) {
        trace->rotatable = NULL;
        trace->outf = outf;
    } else {
        trace->rotatable =
            make_rotatable(pathname_prefix, ".log", rotate_size, trace->params);
        struct timeval tv;
        gettimeofday(&tv, NULL);
        time_t t = tv.tv_sec;
        struct tm tm;
        epoch_to_utc(t, &tm);
        (void) rotatable_rename(trace->rotatable, &tm, tv.tv_usec);
    }
    return trace;
}

fstrace_t *fstrace_open(const char *pathname_prefix, ssize_t rotate_size)
{
    return do_open(pathname_prefix, rotate_size, NULL);
}

fstrace_t *fstrace_direct(FILE *outf)
{
    return do_open("", -1, outf);
}

int fstrace_close(fstrace_t *trace)
{
    if (trace->rotatable)
        destroy_rotatable(trace->rotatable);
    list_elem_t *elem;
    for (elem = list_get_first(trace->events);
         elem != NULL;
         elem = list_next(elem)) {
        struct fstrace_event_impl *ev_imp = (void *) list_elem_get_value(elem);
        list_elem_t *elf;
        for (elf = list_get_first(ev_imp->fields);
             elf != NULL;
             elf = list_next(elf)) {
            struct fstrace_field *field = (void *) list_elem_get_value(elf);
            fsfree(field->leader);
            fsfree(field);
        }
        destroy_list(ev_imp->fields);
        free(ev_imp->id);
        fsfree(ev_imp->trailer);
        fsfree(ev_imp);
    }
    destroy_list(trace->events);
    free(trace->pathname_prefix);
    while (!list_empty(trace->mempool)) {
        fstrace_memblock_t *block = (fstrace_memblock_t *)
            list_pop_first(trace->mempool);
        int status = munmap(block->start, BLOCK_SIZE);
        fsfree(block);
        assert(status >= 0);
    }
    destroy_list(trace->mempool);
    fsfree(trace);
    return 0;
}

static bool lock(fstrace_t *trace)
{
    if (fstrace_state != FSTRACE_STATE_INITIALIZED)
        return false;
    if (pthread_sigmask(SIG_BLOCK, &SIG_MASK, &trace->old_mask) < 0) {
        FSTRACE_FAIL();
        return false;
    }
    if (pthread_mutex_lock(&the_mutex) != 0) {
        FSTRACE_FAIL();
        (void) pthread_sigmask(SIG_SETMASK, &trace->old_mask, NULL);
        return false;
    }
    if (lockf(THE_MUTEX, F_LOCK, 0) < 0) {
        FSTRACE_FAIL();
        (void) pthread_mutex_unlock(&the_mutex);
        (void) pthread_sigmask(SIG_SETMASK, &trace->old_mask, NULL);
        return false;
    }
    return true;
}

static void unlock(fstrace_t *trace)
{
    if (fstrace_state != FSTRACE_STATE_INITIALIZED)
        return;
    if (lockf(THE_MUTEX, F_ULOCK, 0) < 0) {
        FSTRACE_FAIL();
        (void) pthread_mutex_unlock(&the_mutex);
        (void) pthread_sigmask(SIG_SETMASK, &trace->old_mask, NULL);
        return;
    }
    if (pthread_mutex_unlock(&the_mutex) != 0) {
        FSTRACE_FAIL();
        (void) pthread_sigmask(SIG_SETMASK, &trace->old_mask, NULL);
        return;
    }
    if (pthread_sigmask(SIG_SETMASK, &trace->old_mask, NULL) < 0)
        FSTRACE_FAIL();
}

static void process_percent(fstrace_t *trace, va_list *pap)
{
    fputc('%', trace->outf);
}

static void process_signed(fstrace_t *trace, va_list *pap)
{
    fprintf(trace->outf, "%d", va_arg(*pap, int));
}

static void process_unsigned(fstrace_t *trace, va_list *pap)
{
    fprintf(trace->outf, "%u", va_arg(*pap, unsigned));
}

static void process_hex(fstrace_t *trace, va_list *pap)
{
    fprintf(trace->outf, "%x", va_arg(*pap, unsigned));
}

static void process_signed64(fstrace_t *trace, va_list *pap)
{
    fprintf(trace->outf, "%lld", (long long) va_arg(*pap, int64_t));
}

static void process_unsigned64(fstrace_t *trace, va_list *pap)
{
    fprintf(trace->outf, "%llu",
            (unsigned long long) va_arg(*pap, uint64_t));
}

static void process_hex64(fstrace_t *trace, va_list *pap)
{
    fprintf(trace->outf, "%llx",
            (unsigned long long) va_arg(*pap, uint64_t));
}

static void process_ssize_t(fstrace_t *trace, va_list *pap)
{
    fprintf(trace->outf, "%lld", (long long) va_arg(*pap, ssize_t));
}

static void process_float(fstrace_t *trace, va_list *pap)
{
    fprintf(trace->outf, "%.17g", va_arg(*pap, double));
}

static void process_bool(fstrace_t *trace, va_list *pap)
{
    fputs(va_arg(*pap, int) ? "true" : "false", trace->outf);
}

static char url_encode_table[256] = {
    '%', '%', '%', '%', '%', '%', '%', '%',
    '%', '%', '%', '%', '%', '%', '%', '%',
    '%', '%', '%', '%', '%', '%', '%', '%',
    '%', '%', '%', '%', '%', '%', '%', '%',
    '+', '%', '"', '%', '%', '%', '%', '%',
    '%', '%', '%', '%', '%', '-', '.', '/',
    '0', '1', '2', '3', '4', '5', '6', '7',
    '8', '9', '%', '%', '<', '%', '>', '%',
    '%', 'A', 'B', 'C', 'D', 'E', 'F', 'G',
    'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O',
    'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W',
    'X', 'Y', 'Z', '%', '\\', '%', '^', '_',
    '`', 'a', 'b', 'c', 'd', 'e', 'f', 'g',
    'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o',
    'p', 'q', 'r', 's', 't', 'u', 'v', 'w',
    'x', 'y', 'z', '{', '|', '}', '~', '%',
    '%', '%', '%', '%', '%', '%', '%', '%',
    '%', '%', '%', '%', '%', '%', '%', '%',
    '%', '%', '%', '%', '%', '%', '%', '%',
    '%', '%', '%', '%', '%', '%', '%', '%',
    '%', '%', '%', '%', '%', '%', '%', '%',
    '%', '%', '%', '%', '%', '%', '%', '%',
    '%', '%', '%', '%', '%', '%', '%', '%',
    '%', '%', '%', '%', '%', '%', '%', '%',
    '%', '%', '%', '%', '%', '%', '%', '%',
    '%', '%', '%', '%', '%', '%', '%', '%',
    '%', '%', '%', '%', '%', '%', '%', '%',
    '%', '%', '%', '%', '%', '%', '%', '%',
    '%', '%', '%', '%', '%', '%', '%', '%',
    '%', '%', '%', '%', '%', '%', '%', '%',
    '%', '%', '%', '%', '%', '%', '%', '%',
    '%', '%', '%', '%', '%', '%', '%', '%'
};

static void emit_byte_verbatim(FILE *outf, uint8_t byte)
{
    fputc(byte, outf);
}

static void emit_byte(FILE *outf, uint8_t byte)
{
    char c = url_encode_table[byte];
    if (c == '%')
        fprintf(outf, "%%%02x", byte);
    else fputc(c, outf);
}

static void emit_string(FILE *outf, const char *begin, const char *end)
{
    if (begin == NULL) {
        emit_byte(outf, 0);
        return;
    }
    const char *p;
    for (p = begin; *p && p != end; p++)
        emit_byte(outf, (uint8_t) *p);
}

static void process_string(fstrace_t *trace, va_list *pap)
{
    emit_string(trace->outf, va_arg(*pap, const char *), NULL);
}

static void process_limited_string(fstrace_t *trace, va_list *pap)
{
    const char *string = va_arg(*pap, const char *);
    ssize_t size = va_arg(*pap, ssize_t);
    if (size < 0)
        size = 0;
    emit_string(trace->outf, string, string + size);
}

static void process_indirect_string(fstrace_t *trace, va_list *pap)
{
    const char *(*func)(void *) = va_arg(*pap, const char *(*)(void *));
    void *arg = va_arg(*pap, void *);
    emit_string(trace->outf, func(arg), NULL);
}

static void process_iteration(fstrace_t *trace, va_list *pap)
{
    const char *(*func)(void *) = va_arg(*pap, const char *(*)(void *));
    void *arg = va_arg(*pap, void *);
    emit_byte_verbatim(trace->outf, '[');
    const char *s = func(arg);
    if (s) {
        emit_string(trace->outf, s, NULL);
        for (;;) {
            s = func(arg);
            if (!s)
                break;
            emit_byte_verbatim(trace->outf, ',');
            emit_string(trace->outf, s, NULL);
        }
    }
    emit_byte_verbatim(trace->outf, ']');
}

static void process_arbitrary_string(fstrace_t *trace, va_list *pap)
{
    const uint8_t *p = va_arg(*pap, const uint8_t *);
    ssize_t size = va_arg(*pap, ssize_t);
    while (size-- > 0)
        emit_byte(trace->outf, (uint8_t) *p++);
}

static void emit_blob(FILE *outf, const uint8_t *p, ssize_t size)
{
    while (size-- > 0)
        fprintf(outf, "%02x", *p++);
}

static void process_blob(fstrace_t *trace, va_list *pap)
{
    const uint8_t *p = va_arg(*pap, const uint8_t *);
    ssize_t size = va_arg(*pap, ssize_t);
    emit_blob(trace->outf, p, size);
}

static void process_pid(fstrace_t *trace, va_list *pap)
{
    fprintf(trace->outf, "%lu", (unsigned long) getpid());
}

static void process_tid(fstrace_t *trace, va_list *pap)
{
    fprintf(trace->outf, "%lu", (unsigned long) pthread_self());
}

static void process_file(fstrace_t *trace, va_list *pap)
{
    emit_string(trace->outf, trace->file, NULL);
}

static void process_line(fstrace_t *trace, va_list *pap)
{
    fprintf(trace->outf, "%u", trace->lineno);
}

static const char *errid(int err)
{
    switch (err) {
        case E2BIG:           return "E2BIG";
        case EACCES:          return "EACCES";
        case EADDRINUSE:      return "EADDRINUSE";
        case EADDRNOTAVAIL:   return "EADDRNOTAVAIL";
        case EAFNOSUPPORT:    return "EAFNOSUPPORT";
        case EAGAIN:          return "EAGAIN";
        case EALREADY:        return "EALREADY";
        case EBADF:           return "EBADF";
        case EBADMSG:         return "EBADMSG";
        case EBUSY:           return "EBUSY";
        case ECANCELED:       return "ECANCELED";
        case ECHILD:          return "ECHILD";
        case ECONNABORTED:    return "ECONNABORTED";
        case ECONNREFUSED:    return "ECONNREFUSED";
        case ECONNRESET:      return "ECONNRESET";
        case EDEADLK:         return "EDEADLK";
        case EDESTADDRREQ:    return "EDESTADDRREQ";
        case EDOM:            return "EDOM";
        case EDQUOT:          return "EDQUOT";
        case EEXIST:          return "EEXIST";
        case EFAULT:          return "EFAULT";
        case EFBIG:           return "EFBIG";
        case EHOSTDOWN:       return "EHOSTDOWN";
        case EHOSTUNREACH:    return "EHOSTUNREACH";
        case EIDRM:           return "EIDRM";
        case EILSEQ:          return "EILSEQ";
        case EINPROGRESS:     return "EINPROGRESS";
        case EINTR:           return "EINTR";
        case EINVAL:          return "EINVAL";
        case EIO:             return "EIO";
        case EISCONN:         return "EISCONN";
        case EISDIR:          return "EISDIR";
        case ELOOP:           return "ELOOP";
        case EMFILE:          return "EMFILE";
        case EMLINK:          return "EMLINK";
        case EMSGSIZE:        return "EMSGSIZE";
        case EMULTIHOP:       return "EMULTIHOP";
        case ENAMETOOLONG:    return "ENAMETOOLONG";
        case ENETDOWN:        return "ENETDOWN";
        case ENETRESET:       return "ENETRESET";
        case ENETUNREACH:     return "ENETUNREACH";
        case ENFILE:          return "ENFILE";
        case ENOBUFS:         return "ENOBUFS";
        case ENODATA:         return "ENODATA";
        case ENODEV:          return "ENODEV";
        case ENOENT:          return "ENOENT";
        case ENOEXEC:         return "ENOEXEC";
        case ENOLCK:          return "ENOLCK";
        case ENOLINK:         return "ENOLINK";
        case ENOMEM:          return "ENOMEM";
        case ENOMSG:          return "ENOMSG";
        case ENOPROTOOPT:     return "ENOPROTOOPT";
        case ENOSPC:          return "ENOSPC";
        case ENOSR:           return "ENOSR";
        case ENOSTR:          return "ENOSTR";
        case ENOSYS:          return "ENOSYS";
        case ENOTBLK:         return "ENOTBLK";
        case ENOTCONN:        return "ENOTCONN";
        case ENOTDIR:         return "ENOTDIR";
        case ENOTEMPTY:       return "ENOTEMPTY";
        case ENOTRECOVERABLE: return "ENOTRECOVERABLE";
        case ENOTSOCK:        return "ENOTSOCK";
        case ENOTTY:          return "ENOTTY";
        case ENXIO:           return "ENXIO";
        case EOPNOTSUPP:      return "EOPNOTSUPP";
        case EOVERFLOW:       return "EOVERFLOW";
        case EOWNERDEAD:      return "EOWNERDEAD";
        case EPERM:           return "EPERM";
        case EPFNOSUPPORT:    return "EPFNOSUPPORT";
        case EPIPE:           return "EPIPE";
        case EPROTO:          return "EPROTO";
        case EPROTONOSUPPORT: return "EPROTONOSUPPORT";
        case EPROTOTYPE:      return "EPROTOTYPE";
        case ERANGE:          return "ERANGE";
        case EREMOTE:         return "EREMOTE";
        case EROFS:           return "EROFS";
        case ESHUTDOWN:       return "ESHUTDOWN";
        case ESOCKTNOSUPPORT: return "ESOCKTNOSUPPORT";
        case ESPIPE:          return "ESPIPE";
        case ESRCH:           return "ESRCH";
        case ESTALE:          return "ESTALE";
        case ETIME:           return "ETIME";
        case ETIMEDOUT:       return "ETIMEDOUT";
        case ETOOMANYREFS:    return "ETOOMANYREFS";
        case ETXTBSY:         return "ETXTBSY";
        case EUSERS:          return "EUSERS";
        case EXDEV:           return "EXDEV";
#ifdef __linux__
        case EADV:            return "EADV";
        case EBADE:           return "EBADE";
        case EBADFD:          return "EBADFD";
        case EBADR:           return "EBADR";
        case EBADRQC:         return "EBADRQC";
        case EBADSLT:         return "EBADSLT";
        case EBFONT:          return "EBFONT";
        case ECHRNG:          return "ECHRNG";
        case ECOMM:           return "ECOMM";
        case EDOTDOT:         return "EDOTDOT";
#ifdef EHWPOISON
        case EHWPOISON:       return "EHWPOISON";
#endif
        case EISNAM:          return "EISNAM";
        case EKEYEXPIRED:     return "EKEYEXPIRED";
        case EKEYREJECTED:    return "EKEYREJECTED";
        case EKEYREVOKED:     return "EKEYREVOKED";
        case EL2HLT:          return "EL2HLT";
        case EL2NSYNC:        return "EL2NSYNC";
        case EL3HLT:          return "EL3HLT";
        case EL3RST:          return "EL3RST";
        case ELIBACC:         return "ELIBACC";
        case ELIBBAD:         return "ELIBBAD";
        case ELIBEXEC:        return "ELIBEXEC";
        case ELIBMAX:         return "ELIBMAX";
        case ELIBSCN:         return "ELIBSCN";
        case ELNRNG:          return "ELNRNG";
        case EMEDIUMTYPE:     return "EMEDIUMTYPE";
        case ENAVAIL:         return "ENAVAIL";
        case ENOANO:          return "ENOANO";
        case ENOCSI:          return "ENOCSI";
        case ENOKEY:          return "ENOKEY";
        case ENOMEDIUM:       return "ENOMEDIUM";
        case ENONET:          return "ENONET";
        case ENOPKG:          return "ENOPKG";
        case ENOTNAM:         return "ENOTNAM";
        case ENOTUNIQ:        return "ENOTUNIQ";
        case EREMCHG:         return "EREMCHG";
        case EREMOTEIO:       return "EREMOTEIO";
        case ERESTART:        return "ERESTART";
        case ERFKILL:         return "ERFKILL";
        case ESRMNT:          return "ESRMNT";
        case ESTRPIPE:        return "ESTRPIPE";
        case EUCLEAN:         return "EUCLEAN";
        case EUNATCH:         return "EUNATCH";
        case EXFULL:          return "EXFULL";
#else
        case EAUTH:           return "EAUTH";
        case EBADARCH:        return "EBADARCH";
        case EBADEXEC:        return "EBADEXEC";
        case EBADMACHO:       return "EBADMACHO";
        case EBADRPC:         return "EBADRPC";
        case EDEVERR:         return "EDEVERR";
        case EFTYPE:          return "EFTYPE";
        case ELAST:           return "ELAST";
        case ENEEDAUTH:       return "ENEEDAUTH";
        case ENOATTR:         return "ENOATTR";
        case ENOPOLICY:       return "ENOPOLICY";
        case EPROCLIM:        return "EPROCLIM";
        case EPROCUNAVAIL:    return "EPROCUNAVAIL";
        case EPROGMISMATCH:   return "EPROGMISMATCH";
        case EPROGUNAVAIL:    return "EPROGUNAVAIL";
        case EPWROFF:         return "EPWROFF";
        case ERPCMISMATCH:    return "ERPCMISMATCH";
        case ESHLIBVERS:      return "ESHLIBVERS";
#endif
        default: return NULL;
    }
}

static void process_errno(fstrace_t *trace, va_list *pap)
{
    const char *id = errid(trace->err);
    if (id)
        fputs(id, trace->outf);
    else fprintf(trace->outf, "%d", trace->err);
}

static void process_errno_arg(fstrace_t *trace, va_list *pap)
{
    int err = va_arg(*pap, int);
    const char *id = errid(err);
    if (id)
        fputs(id, trace->outf);
    else fprintf(trace->outf, "%d", err);
}

static void process_pointer(fstrace_t *trace, va_list *pap)
{
    fprintf(trace->outf, "%llx",
            (unsigned long long) (uintptr_t) va_arg(*pap, void *));
}

static void emit_unknown_address(FILE *outf, const struct sockaddr *addr,
                                 socklen_t addrlen)
{
    fprintf(outf, "OTHER`");
    emit_blob(outf, (const uint8_t *) addr, addrlen);
}

static void emit_ipv4_address(FILE *outf, const struct sockaddr_in *addr,
                              socklen_t addrlen)
{
    if (addrlen < sizeof(struct sockaddr_in)) {
        emit_unknown_address(outf, (const struct sockaddr *) addr, addrlen);
        return;
    }
    unsigned port = ntohs(addr->sin_port);
    unsigned long addr4 = ntohl(addr->sin_addr.s_addr);
    fprintf(outf, "AF_INET`%lu.%lu.%lu.%lu`%u",
            addr4 >> 24 & 0xff, addr4 >> 16 & 0xff, addr4 >> 8 & 0xff,
            addr4 & 0xff, port);
}

static void emit_ipv6_segment(FILE *outf, const uint16_t *p,
                              const uint16_t *end)
{
    if (p < end) {
        fprintf(outf, "%x", ntohs(*p++));
        while (p < end)
            fprintf(outf, ":%x", ntohs(*p++));
    }
}

static void emit_ipv6_address(FILE *outf, const struct sockaddr_in6 *addr,
                              socklen_t addrlen)
{
    if (addrlen < sizeof(struct sockaddr_in6)) {
        emit_unknown_address(outf, (const struct sockaddr *) addr, addrlen);
        return;
    }
    unsigned port = ntohs(addr->sin6_port);
    const uint16_t *begin = (const uint16_t *) addr->sin6_addr.s6_addr;
    const uint16_t *end = begin + 8;
    const uint16_t *best_zero = NULL;
    size_t best_seq = 0;
    const uint16_t *p = begin;
    for (p = begin; p < end; p++) {
        const uint16_t *q = p;
        for (; p < end && !*p; p++)
            ;
        size_t seq = p - q;
        if (seq > best_seq) {
            best_zero = q;
            best_seq = seq;
        }
    }
    fprintf(outf, "AF_INET6`");
    if (!best_zero)
        emit_ipv6_segment(outf, begin, end);
    else {
        emit_ipv6_segment(outf, begin, best_zero);
        fprintf(outf, "::");
        emit_ipv6_segment(outf, best_zero + best_seq, end);
    }
    fprintf(outf, "`%u", port);
}

static void emit_unix_address(FILE *outf, const struct sockaddr_un *addr,
                              socklen_t addrlen)
{
    const char *path = addr->sun_path;
    size_t offset = path - (const char *) addr;
    size_t pathlen = addrlen - offset;
    if (!pathlen)
        fprintf(outf, "AF_UNIX`unnamed");
    else if (!path[0]) {
        fprintf(outf, "AF_UNIX`abstract`");
        emit_string(outf, path + 1, path + pathlen - 1);
    } else {
        size_t i;
        for (i = 0; i < pathlen; i++)
            if (path[i] == 0) {
                fprintf(outf, "AF_UNIX`path`");
                emit_string(outf, path + 1, NULL);
                return;
            }
        fprintf(outf, "AF_UNIX`bad`");
        emit_string(outf, path, path + pathlen);
    }
}

static void process_address(fstrace_t *trace, va_list *pap)
{
    const struct sockaddr *addr = va_arg(*pap, const struct sockaddr *);
    socklen_t addrlen = va_arg(*pap, socklen_t);
    if (!addr) {
        fprintf(trace->outf, "-");
        return;
    }
    if (addrlen >= sizeof addr->sa_family)
        switch (addr->sa_family) {
            case AF_INET:
                emit_ipv4_address(trace->outf,
                                  (const struct sockaddr_in *) addr, addrlen);
                return;
            case AF_INET6:
                emit_ipv6_address(trace->outf,
                                  (const struct sockaddr_in6 *) addr, addrlen);
                return;
            case AF_UNIX:
                emit_unix_address(trace->outf,
                                  (const struct sockaddr_un *) addr, addrlen);
                return;
            default:
                ;
        }
    emit_unknown_address(trace->outf, addr, addrlen);
}

const char *skip(const char *s, const char *prefix)
{
    while (*prefix)
        if (*s++ != *prefix++)
            return NULL;
    return s;
}

static struct field_descr {
    const char *directive;
    void (*processor)(fstrace_t *trace, va_list *pap);
} fields[] = {
    { "%%", process_percent },
    { "%d", process_signed },
    { "%u", process_unsigned },
    { "%x", process_hex },
    { "%64d", process_signed64 },
    { "%64u", process_unsigned64 },
    { "%64x", process_hex64 },
    { "%z", process_ssize_t },
    { "%f", process_float },
    { "%b", process_bool },
    { "%s", process_string },
    { "%S", process_limited_string },
    { "%I", process_indirect_string },
    { "%J", process_iteration },
    { "%A", process_arbitrary_string },
    { "%B", process_blob },
    { "%P", process_pid },
    { "%T", process_tid },
    { "%F", process_file },
    { "%L", process_line },
    { "%e", process_errno },
    { "%E", process_errno_arg },
    { "%p", process_pointer },
    { "%a", process_address },
    { NULL, }
};

static struct field_descr *identify_field(const char *format,
                                          const char **next)
{
    struct field_descr *descr;
    for (descr = fields; descr->directive; descr++) {
        const char *q = skip(format, descr->directive);
        if (q) {
            *next = q;
            return descr;
        }
    }
    return NULL;
}

static int parse_format(struct fstrace_event_impl *ev_imp, const char *format)
{
    const char *p = format;
    for (;;) {
        const char *q = p;
        while (*q && *q != '%')
            q++;
        size_t snippet_size = q - p;
        char *snippet = fsalloc(snippet_size + 1);
        memcpy(snippet, p, snippet_size);
        snippet[snippet_size] = '\0';
        if (*q != '%') {
            ev_imp->trailer = snippet;
            return 1;
        }
        const char *r;
        struct field_descr *descr = identify_field(q, &r);
        if (descr == NULL) {
            ev_imp->trailer = snippet;
            return 0;
        }
        struct fstrace_field *field = fsalloc(sizeof *field);
        field->leader = snippet;
        field->processor = descr->processor;
        list_append(ev_imp->fields, field);
        p = r;
    }
}

fstrace_event_t *fstrace_declare(fstrace_t *trace, const char *id,
                                 const char *format)
{
    struct fstrace_event_impl *ev_imp = fsalloc(sizeof *ev_imp);
    ev_imp->trace = trace;
    ev_imp->id = strdup(id);
    ev_imp->fields = make_list();
    if (!lock(trace))
        return NULL;
    fstrace_event_t *event = shared_alloc(trace, sizeof *event);
    event->impl = ev_imp;         /* redundant */
    event->enabled = 0;
    ev_imp->shared = event;
    list_append(trace->events, ev_imp);
    unlock(trace);
    if (!parse_format(ev_imp, format)) {
        /* Leave the bad object allocated. It is permanently disabled
         * and will get cleaned with fstrace_close(). Or more to the
         * point, the programmer will fix the bug. */
        return NULL;
    }
    return event;
}

static bool emit_timestamp(fstrace_t *trace)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_t t = tv.tv_sec;
    struct tm tm;
    epoch_to_utc(t, &tm);
    if (trace->rotatable) {
        bool in_sync = trace->ordinal == *trace->shared_ordinal;
        switch (rotatable_rotate_maybe(trace->rotatable, &tm, tv.tv_usec,
                                       !in_sync)) {
            case ROTATION_OK:
                trace->ordinal = *trace->shared_ordinal;
                break;
            case ROTATION_ROTATED:
                trace->ordinal = ++*trace->shared_ordinal;
                break;
            default:
                return false;
        }
        trace->outf = rotatable_file(trace->rotatable);
    }
    fprintf(trace->outf, "%04d-%02d-%02d %02d:%02d:%02d.%06d ",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec, (int) tv.tv_usec);
    return true;
}

static void __fstrace_log(fstrace_event_t *event,
                          const char *file, unsigned lineno, int err,
                          va_list *pap)
{
    struct fstrace_event_impl *ev_imp = event->impl;
    fstrace_t *trace = ev_imp->trace;
    trace->file = file;
    trace->lineno = lineno;
    trace->err = err;
    fprintf(trace->outf, "%s ", ev_imp->id);
    list_elem_t *elem;
    for (elem = list_get_first(ev_imp->fields);
         elem != NULL;
         elem = list_next(elem)) {
        struct fstrace_field *field = (void *) list_elem_get_value(elem);
        fprintf(trace->outf, "%s", field->leader);
        field->processor(trace, pap);
    }
    fprintf(trace->outf, "%s\n", ev_imp->trailer);
    fflush(trace->outf);
}

static void __fstrace_lock_and_log(fstrace_event_t *event,
                                   const char *file, unsigned lineno,
                                   va_list *pap)
{
    int err = errno;
    struct fstrace_event_impl *ev_imp = event->impl;
    fstrace_t *trace = ev_imp->trace;
    if (lock(trace)) {
        if (event->enabled && emit_timestamp(ev_imp->trace))
            __fstrace_log(event, file, lineno, err, pap);
        unlock(trace);
    }
    errno = err;
}

void fstrace_log(fstrace_event_t *event, ...)
{
    va_list ap;
    va_start(ap, event);
    __fstrace_lock_and_log(event, "unknown", 0, &ap);
    va_end(ap);
}

void fstrace_log_2(fstrace_event_t *event, const char *file, unsigned lineno,
                   ...)
{
    va_list ap;
    va_start(ap, lineno);
    __fstrace_lock_and_log(event, file, lineno, &ap);
    va_end(ap);
}

void fstrace_select_safe(fstrace_t *trace,
                         int (*select)(void *data, const char *id),
                         void *data)
{
    if (lock(trace)) {
        fstrace_select(trace, select, data);
        unlock(trace);
    }
}

void fstrace_select(fstrace_t *trace,
                    int (*select)(void *data, const char *id),
                    void *data)
{
    list_elem_t *elem;
    for (elem = list_get_first(trace->events);
         elem != NULL;
         elem = list_next(elem)) {
        struct fstrace_event_impl *ev_imp = (void *) list_elem_get_value(elem);
        int selection = select(data, ev_imp->id);
        if (selection < 0)
            ev_imp->shared->enabled = 0;
        else if (selection > 0)
            ev_imp->shared->enabled = 1;
    }
}

struct selector {
    regex_t preg_include;
    regex_t preg_exclude;
};

static int select_event(void *data, const char *id)
{
    struct selector *selector = data;
    if (regexec(&selector->preg_exclude, id, 0, NULL, 0) != 0 &&
        regexec(&selector->preg_include, id, 0, NULL, 0) == 0) {
        return 1;
    }
    return -1;
}

bool fstrace_select_regex(fstrace_t *trace, const char *include_re,
                          const char *exclude_re)
{
    const char *IMPOSSIBLE_RE = "X^"; /* nothing can match this re */
    struct selector selector;
    int code = regcomp(&selector.preg_include, include_re ?: IMPOSSIBLE_RE,
                       REG_EXTENDED | REG_NOSUB);
    if (code != 0)
        return false;
    code = regcomp(&selector.preg_exclude, exclude_re ?: IMPOSSIBLE_RE,
                   REG_EXTENDED | REG_NOSUB);
    if (code != 0) {
        regfree(&selector.preg_include);
        return false;
    }
    fstrace_select(trace, select_event, &selector);
    regfree(&selector.preg_exclude);
    regfree(&selector.preg_include);
    return true;
}

void fstrace_select_event(fstrace_event_t *event, int selection)
{
    if (selection == 0)
        return;
    struct fstrace_event_impl *ev_imp = event->impl;
    fstrace_t *trace = ev_imp->trace;
    if (lock(trace)) {
        event->enabled = selection > 0;
        unlock(trace);
    }
}

int fstrace_chown(uid_t owner, gid_t group)
{
    THE_USER_ID = owner;
    THE_GROUP_ID = group;
    if (fstrace_state == FSTRACE_STATE_INITIALIZED &&
        fchown(THE_MUTEX, owner, group) < 0)
        FSTRACE_FAIL();
    return 0;
}

int fstrace_reinit()
{
    if (fstrace_state != FSTRACE_STATE_INITIALIZED)
        return 0;
    close(THE_MUTEX);
    int fd = open(lock_path, O_WRONLY);
    if (fd < 0) {
        FSTRACE_FAIL();
        return 0;
    }
    if (fd != THE_MUTEX) {
        if (dup2(fd, THE_MUTEX) <0)
            FSTRACE_FAIL();
        close(fd);
    }
    return 0;
}


int fstrace_reopen(fstrace_t *trace)
{
    fstrace_reinit();
    if (fstrace_state == FSTRACE_STATE_INITIALIZED && trace->rotatable)
        rotatable_invalidate(trace->rotatable);
    return 0;
}

void fstrace_limit_rotation_file_count(fstrace_t *trace, int max_files)
{
    if (lock(trace)) {
        trace->params->max_files = max_files;
        unlock(trace);
    }
}

void fstrace_limit_rotation_file_age(fstrace_t *trace, int max_seconds)
{
    if (lock(trace)) {
        trace->params->max_seconds = max_seconds;
        unlock(trace);
    }
}

void fstrace_limit_rotation_byte_count(fstrace_t *trace, int64_t max_bytes)
{
    if (lock(trace)) {
        trace->params->max_bytes = max_bytes;
        unlock(trace);
    }
}

static fstrace_event_spec_t *specs;

void fstrace_specify(fstrace_event_spec_t *spec, fstrace_event_t **variable,
                     char name[], const char *format)
{
    spec->next = specs;
    specs = spec;
    spec->variable = variable;
    spec->id = name;
    spec->format = format;
    /* Turn underscores to hyphens (tradition): */
    char *p;
    for (p = name; *p; p++)
        if (*p == '_')
            *p = '-';
}

void fstrace_declare_globals(fstrace_t *trace)
{
    fstrace_event_spec_t *spec = specs;
    for (; spec; spec = spec->next)
        *spec->variable = fstrace_declare(trace, spec->id, spec->format);
}

static char repr_buf[20];

const char *fstrace_signed_repr(int64_t n)
{
    snprintf(repr_buf, sizeof repr_buf, "%lld", (long long) n);
    return repr_buf;
}

const char *fstrace_unsigned_repr(uint64_t n)
{
    snprintf(repr_buf, sizeof repr_buf, "%llu", (unsigned long long) n);
    return repr_buf;
}

const char *fstrace_hex_repr(uint64_t n)
{
    snprintf(repr_buf, sizeof repr_buf, "%llx", (unsigned long long) n);
    return repr_buf;
}
