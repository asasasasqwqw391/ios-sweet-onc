/*
 * logdump - print logd diagnostics to /dev/console (freestanding arm64 Mach-O).
 *
 * The iOS userland has no shell and daemon stdout goes to logd, so once the
 * boot tasks finish the console goes quiet.  launchd runs this daemon at boot:
 * it walks /private/var/db/diagnostics (and /private/var/log) and for every
 * tracev3/log file prints its name/size and the printable text runs it finds.
 * Two rounds, 45 s apart, so logd's first lines are included.
 *
 * No libSystem: raw BSD syscalls via svc #0x80 (syscall number in x16).  On
 * Darwin the carry flag reports an error and x0 holds the errno.
 */
typedef unsigned long u64;
typedef long s64;
typedef unsigned short u16;
typedef unsigned char u8;

#define SYS_exit 1
#define SYS_read 3
#define SYS_write 4
#define SYS_open 5
#define SYS_close 6
#define SYS_getdirentries 196
#define SYS_lseek 199
#define SYS_nanosleep 240

#define O_RDONLY 0
#define O_WRONLY 1
#define O_DIRECTORY 0x100000

static s64
sys6(s64 n, s64 a, s64 b, s64 c, s64 d, s64 e, s64 f)
{
	register s64 x16 __asm__("x16") = n;
	register s64 x0 __asm__("x0") = a;
	register s64 x1 __asm__("x1") = b;
	register s64 x2 __asm__("x2") = c;
	register s64 x3 __asm__("x3") = d;
	register s64 x4 __asm__("x4") = e;
	register s64 x5 __asm__("x5") = f;
	register u32 carry __asm__("w9");

	__asm__ volatile("svc #0x80\n\t"
	    "cset %w0, cs"
	    : "=r"(carry), "+r"(x0)
	    : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x16)
	    : "memory", "cc");
	if (carry != 0) {
		return -x0;	/* error: x0 = errno */
	}
	return x0;
}

static s64
sys1(s64 n, s64 a)
{
	return sys6(n, a, 0, 0, 0, 0, 0);
}

static s64
sys2(s64 n, s64 a, s64 b)
{
	return sys6(n, a, b, 0, 0, 0, 0);
}

static s64
sys3(s64 n, s64 a, s64 b, s64 c)
{
	return sys6(n, a, b, c, 0, 0, 0);
}

static int con = -1;
static unsigned long lines;

static unsigned long
slen(const char *s)
{
	unsigned long n = 0;

	while (s[n] != 0) {
		n++;
	}
	return n;
}

static void
out(const char *s, unsigned long n)
{
	if (con >= 0 && lines < 240) {
		(void)sys3(SYS_write, con, (s64)s, (s64)n);
	}
}

static void
outs(const char *s)
{
	out(s, slen(s));
}

static void
outnl(void)
{
	out("\n", 1);
}

static void
outnum(u64 v)
{
	char b[24];
	int i = 23;

	if (v == 0) {
		out("0", 1);
		return;
	}
	b[i--] = 0;
	while (v != 0 && i >= 0) {
		b[i--] = (char)('0' + (v % 10));
		v /= 10;
	}
	outs(&b[i + 1]);
}

static void
sleep_s(long sec)
{
	struct ts {
		long tv_sec;
		long tv_nsec;
	} t;

	t.tv_sec = sec;
	t.tv_nsec = 0;
	(void)sys2(SYS_nanosleep, (s64)&t, 0);
}

static int
ends_with(const char *name, const char *suf)
{
	unsigned long nl = slen(name);
	unsigned long sl = slen(suf);

	if (nl < sl) {
		return 0;
	}
	for (unsigned long i = 0; i < sl; i++) {
		if (name[nl - sl + i] != suf[i]) {
			return 0;
		}
	}
	return 1;
}

/* Print printable runs of at least 8 chars (tracev3 payloads are compressed,
 * but daemon text still shows up in many blocks). */
static void
dump_strings(const char *buf, long len, int max_lines)
{
	long i = 0;
	int printed = 0;

	while (i < len && printed < max_lines && lines < 240) {
		long start;
		long j;

		if ((u8)buf[i] < 0x20 || (u8)buf[i] > 0x7e) {
			i++;
			continue;
		}
		start = i;
		while (i < len && (u8)buf[i] >= 0x20 && (u8)buf[i] <= 0x7e) {
			i++;
		}
		j = i - start;
		if (j >= 8) {
			out("    ", 4);
			out(buf + start, (unsigned long)j);
			outnl();
			printed++;
			lines++;
		}
	}
}

static void
dump_file(const char *dir, const char *name)
{
	char path[512];
	char fbuf[32768];
	unsigned long dl = slen(dir);
	unsigned long nl = slen(name);
	unsigned long p = 0;
	s64 ffd;
	long fsz;

	if (dl + 1 + nl >= sizeof(path)) {
		return;
	}
	for (unsigned long k = 0; k < dl; k++) {
		path[p++] = dir[k];
	}
	path[p++] = '/';
	for (unsigned long k = 0; k < nl; k++) {
		path[p++] = name[k];
	}
	path[p] = 0;
	ffd = sys3(SYS_open, (s64)path, O_RDONLY, 0);
	if (ffd < 0) {
		return;
	}
	fsz = (long)sys3(SYS_lseek, ffd, 0, 2);
	outs("LOGD: file ");
	outs(name);
	out(" size=", 6);
	outnum((u64)fsz);
	outnl();
	(void)sys3(SYS_lseek, ffd, 0, 0);
	if (fsz > 0) {
		long got = (long)sys3(SYS_read, ffd, (s64)fbuf,
		    (s64)(fsz < (long)sizeof(fbuf) ? fsz : (long)sizeof(fbuf)));
		if (got > 0) {
			dump_strings(fbuf, got, 40);
		}
	}
	(void)sys1(SYS_close, ffd);
}

static void
dump_dir(const char *dir)
{
	char dbuf[4096];
	s64 dfd;

	dfd = sys3(SYS_open, (s64)dir, O_RDONLY | O_DIRECTORY, 0);
	if (dfd < 0) {
		return;
	}
	outs("LOGD: dir ");
	outs(dir);
	outnl();
	for (;;) {
		s64 n = sys3(SYS_getdirentries, dfd, (s64)dbuf,
		    (s64)sizeof(dbuf), 0);
		long off = 0;

		if (n <= 0) {
			break;
		}
		while (off < n && lines < 240) {
			struct dirent_ {
				u64 ino;
				u64 seekoff;
				u16 reclen;
				u16 namlen;
				u8 type;
				char name[1024];
			} *de = (struct dirent_ *)(void *)(dbuf + off);

			if (de->reclen < 24 ||
			    off + (long)de->reclen > n) {
				break;
			}
			off += (long)de->reclen;
			if (de->namlen == 0 || de->name[0] == '.') {
				continue;
			}
			de->name[de->namlen < 1023 ? de->namlen : 1023] = 0;
			if (ends_with(de->name, ".tracev3") ||
			    ends_with(de->name, ".log") ||
			    ends_with(de->name, ".txt")) {
				dump_file(dir, de->name);
			}
		}
	}
	(void)sys1(SYS_close, dfd);
}

void
_start(void)
{
	int round;

	con = (int)sys3(SYS_open, (s64)"/dev/console", O_WRONLY, 0);
	if (con < 0) {
		con = 1;
	}
	for (round = 0; round < 2; round++) {
		if (round != 0) {
			sleep_s(45);
		}
		lines = 0;
		outs("LOGD: ==== logdump round ");
		outnum((u64)round);
		outs(" ====");
		outnl();
		dump_dir("/private/var/db/diagnostics");
		dump_dir("/private/var/db/diagnostics/Persist");
		dump_dir("/private/var/db/diagnostics/Special");
		dump_dir("/private/var/db/diagnostics/Signpost");
		dump_dir("/private/var/log");
		outs("LOGD: ==== end round ");
		outnum((u64)round);
		outs(" ====");
		outnl();
	}
	(void)sys1(SYS_exit, 0);
}
