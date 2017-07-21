#include "libbb.h"
#include <tlhelp32.h>

#define WARGV_OOM ((void *)(intptr_t)-1ll)

static wchar_t **argv_to_wargv(char *const *argv, const char *prepend)
{
	size_t size = 0, count = 1;
	wchar_t **w0, *w1, **wargv;
	int i;

	if (!argv)
		return NULL;

	if (prepend) {
		count++;
		size += MultiByteToWideChar(CP_UTF8, 0, prepend, -1, NULL, 0);
	}
	for (i = 0; argv[i]; i++) {
		count++;
		size += MultiByteToWideChar(CP_UTF8, 0, argv[i], -1, NULL, 0);
	}
	wargv = malloc(count * sizeof(wchar_t *) + size * sizeof(wchar_t));
	if (!wargv)
		return WARGV_OOM;
	w0 = wargv;
	w1 = (void *)(w0 + count);
	if (prepend) {
		*(w0++) = w1;
		w1 += MultiByteToWideChar(CP_UTF8, 0, prepend, -1, w1, size);
	}
	for (i = 0; argv[i]; i++) {
		*(w0++) = w1;
		w1 += MultiByteToWideChar(CP_UTF8, 0, argv[i], -1, w1, size);
	}
	*w0 = NULL;

	return wargv;
}

static int exit_process(HANDLE process, int exit_code);

static struct {
	CRITICAL_SECTION mutex;
	int nr, alloc;
	HANDLE *h;
} spawned_processes;

#ifndef SIGRTMAX
#define SIGRTMAX 63
#endif

static void kill_spawned_processes_on_signal(void)
{
        DWORD status;
	int i, signal;

	/*
	 * Only continue if the process was terminated by a signal, as
	 * indicated by the exit status (128 + sig_no).
	 *
	 * As we are running in an atexit() handler, the exit code has been
	 * set at this stage by the ExitProcess() function already.
	 */
	if (!GetExitCodeProcess(GetCurrentProcess(), &status) ||
			status <= 128 || status > 128 + SIGRTMAX)
		return;
	signal = status - 128;

	EnterCriticalSection(&spawned_processes.mutex);
	for (i = 0; i < spawned_processes.nr; i++) {
		if (GetExitCodeProcess(spawned_processes.h[i], &status) &&
				status == STILL_ACTIVE)
			exit_process(spawned_processes.h[i], 128 + signal);
		CloseHandle(spawned_processes.h[i]);
	}
	spawned_processes.nr = 0;
	LeaveCriticalSection(&spawned_processes.mutex);
}

static void cull_exited_processes(void)
{
	DWORD status;
	int i;

	EnterCriticalSection(&spawned_processes.mutex);
	/* cull exited processes */
	for (i = 0; i < spawned_processes.nr; i++)
		if (GetExitCodeProcess(spawned_processes.h[i], &status) &&
				status != STILL_ACTIVE) {
			CloseHandle(spawned_processes.h[i]);
			spawned_processes.h[i] =
				spawned_processes.h[--spawned_processes.nr];
			i--;
		}
	LeaveCriticalSection(&spawned_processes.mutex);
}

static void exit_process_on_signal(HANDLE process)
{
	cull_exited_processes();
	EnterCriticalSection(&spawned_processes.mutex);
	/* grow array if necessary */
	if (spawned_processes.nr == spawned_processes.alloc) {
		int new_alloc = (spawned_processes.alloc + 8) * 3 / 2;
		HANDLE *new_h = realloc(spawned_processes.h,
				new_alloc * sizeof(HANDLE));
		if (!new_h) {
			LeaveCriticalSection(&spawned_processes.mutex);
			return; /* punt */
		}
		spawned_processes.h = new_h;
		spawned_processes.alloc = new_alloc;
	}

	spawned_processes.h[spawned_processes.nr++] = process;
	LeaveCriticalSection(&spawned_processes.mutex);
}

void initialize_critical_sections(void)
{
	InitializeCriticalSection(&spawned_processes.mutex);
	atexit(kill_spawned_processes_on_signal);
}

static intptr_t mingw_spawnve(int mode,
		const char *cmd, char *const *argv, char *const *env)
{
	wchar_t *wcmd, **wargv, **wenv;
	intptr_t ret;

if (!strncmp(cmd, "/bin/", 5) && !strcmp(argv[0], "/bin/sh")) argv++; /* TO-UNDO!!! */
	wcmd = mingw_pathconv(cmd);
	wargv = argv_to_wargv(argv, NULL);
	wenv = argv_to_wargv(env, NULL);
	if (!wcmd || wargv == WARGV_OOM || wenv == WARGV_OOM) {
		errno = ENOMEM;
		return -1;
	}

if (getenv("SPAWNVE_TRACE")) {
	int i;
	fprintf(stderr, "_wspawnve called with '%S', argv:", wcmd);
	for (i = 0; wargv[i]; i++)
		fprintf(stderr, " '%S'", wargv[i]);
	fprintf(stderr, "\n"); fflush(stderr);
}

	/*
	 * When /bin/<command> does not exist, and <command> is an applet,
	 * run that applet instead.
	 */
	if (!strncmp(cmd, "/bin/", 5) &&
			GetFileAttributesW(wcmd) == INVALID_FILE_ATTRIBUTES &&
			find_applet_by_name(cmd + 5) >= 0) {
fprintf(stderr, "wcmd before: %S, wargv[0]: %S\n", wcmd, wargv[0]); fflush(stderr);
		if (!argv[0] || (strcmp(argv[0], cmd) &&
					strcmp(argv[0], cmd + 5))) {
			/* argv[0] is different from cmd, let's shift in cmd */
			free(wargv);
			wargv = argv_to_wargv(argv, cmd + 5);
			if (wargv == WARGV_OOM) {
				errno = ENOMEM;
				return -1;
			}
		}
		wcmd = mingw_pathconv(get_busybox_exec_path());
fprintf(stderr, "wcmd after: %S, wargv[0]: %S\n", wcmd, wargv[0]); fflush(stderr);
	}

	/*
	 * It can be easily verified that _wspawnve() has no problems with a
	 * wcmd that has the \\?\ prefix and it may be a long path.
	 *
	 * However, some programs inspect their own absolute path, e.g. to
	 * infer the location of related files (think: git.exe and its exec
	 * path), and not all of these programs handle a \\?\ prefix well, e.g.
	 * MSYS2's gdb (MINGW version).
	 *
	 * So let's skip the prefix when possible (i.e. when the path fits
	 * within PATH_MAX minus some wiggling room).
	 */
	if (!wcsncmp(wcmd, L"\\\\?\\", 4) && wcslen(wcmd) < PATH_MAX)
		wcmd += 4;

	/*
	 * We cannot use _P_WAIT here because we need to kill spawned processes
	 * if we're killed, and _P_WAIT does not let us.
	 */
	ret = _wspawnve(_P_NOWAIT, wcmd,
		(const wchar_t *const *)wargv, (const wchar_t *const *)wenv);

	if (ret != (intptr_t)-1)
		exit_process_on_signal((HANDLE)ret);

	free(wargv);
	free(wenv);

	if (ret == (intptr_t)-1 || mode != _P_WAIT)
		return ret;

	for (;;) {
		DWORD exit_code;
		WaitForSingleObject((HANDLE)ret, INFINITE);
		if (!GetExitCodeProcess((HANDLE)ret, &exit_code)) {
			errno = err_win_to_posix(GetLastError());
			CloseHandle((HANDLE)ret);
			return -1;
		}
		if (exit_code != STILL_ACTIVE) {
			cull_exited_processes();
			CloseHandle((HANDLE)ret);
			return (intptr_t)exit_code;
		}
	}
}

int waitpid(pid_t pid, int *status, int options)
{
	HANDLE proc;
	intptr_t ret;

	/* Windows does not understand parent-child */
	if (pid > 0 && options == 0) {
		if ( (proc=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_INFORMATION,
						FALSE, pid)) != NULL ) {
			ret = _cwait(status, (intptr_t)proc, 0);
			CloseHandle(proc);
			return ret == -1 ? -1 : pid;
		}
	}
	errno = EINVAL;
	return -1;
}

const char *
next_path_sep(const char *path)
{
	static const char *from = NULL, *to;
	static int has_semicolon;
	int len = strlen(path);

	if (!from || !(path >= from && path+len <= to)) {
		from = path;
		to = from+len;
		has_semicolon = strchr(path, ';') != NULL;
	}

	/* Semicolons take precedence, it's Windows PATH */
	if (has_semicolon)
		return strchr(path, ';');
	/* PATH=C:, not really a separator */
	return strchr(has_dos_drive_prefix(path) ? path+2 : path, ':');
}

#define MAX_OPT 10

static const char *
parse_interpreter(const char *cmd, char ***opts, int *nopts)
{
	static char buf[100], *opt[MAX_OPT];
	char *p, *s, *t;
	int n, fd;

	*nopts = 0;
	*opts = opt;

	/* don't even try a .exe */
	n = strlen(cmd);
	if (n >= 4 &&
	    (!strcasecmp(cmd+n-4, ".exe") ||
	     !strcasecmp(cmd+n-4, ".com")))
		return NULL;

	fd = open(cmd, O_RDONLY);
	if (fd < 0)
		return NULL;
	n = read(fd, buf, sizeof(buf)-1);
	close(fd);
	if (n < 4)	/* at least '#!/x' and not error */
		return NULL;

	/*
	 * See http://www.in-ulm.de/~mascheck/various/shebang/ for trivia
	 * relating to '#!'.
	 */
	if (buf[0] != '#' || buf[1] != '!')
		return NULL;
	buf[n] = '\0';
	p = strchr(buf, '\n');
	if (!p)
		return NULL;
	*p = '\0';

	/* remove trailing whitespace */
	while ( isspace(*--p) ) {
		*p = '\0';
	}

	/* skip whitespace after '#!' */
	for ( s=buf+2; *s && isspace(*s); ++s ) {
	}

	/* move to end of interpreter path (which may not contain spaces) */
	for ( ; *s && !isspace(*s); ++s ) {
	}

	n = 0;
	if ( *s != '\0' ) {
		/* there are options */
		*s++ = '\0';

		while ( (t=strtok(s, " \t")) && n < MAX_OPT ) {
			s = NULL;
			opt[n++] = t;
		}
	}

	/* find interpreter name */
	if (!(p = strrchr(buf+2, '/')))
		return NULL;

	*nopts = n;
	*opts = opt;

	return p+1;
}

static char *quote_arg_msys2(const char *arg)
{
	int escapes = 0, has_white_space = 0;
	const char *p;
	char *q, *result;

	for (p = arg; *p; p++)
		if (isspace(*p))
			has_white_space = 1;
		else if (*p == '"' || *p == '\\')
			escapes++;

	if (!escapes && !has_white_space && p != arg)
		return (char *)arg;

	q = result = malloc(p - arg + escapes + 3);
	if (!q)
		return (char *)arg; /* out of memory: punt */
	*(q++) = '"';
	for (p = arg; *p; p++) {
		if (*p == '"' || *p == '\\')
			*(q++) = '\\';
		*(q++) = *p;
	}
	*(q++) = '"';
	*q = '\0';

	return result;
}

/*
 * See http://msdn2.microsoft.com/en-us/library/17w5ykft(vs.71).aspx
 * (Parsing C++ Command-Line Arguments)
 */
static char *
quote_arg_mingw(const char *arg)
{
	int len = 0, n = 0;
	int force_quotes = 0;
	char *q, *d;
	const char *p = arg;

	/* empty arguments must be quoted */
	if (!*p) {
		force_quotes = 1;
	}

	while (*p) {
		if (isspace(*p)) {
			/* arguments containing whitespace must be quoted */
			force_quotes = 1;
		}
		else if (*p == '"') {
			/* double quotes in arguments need to be escaped */
			n++;
		}
		else if (*p == '\\') {
			/* count contiguous backslashes */
			int count = 0;
			while (*p == '\\') {
				count++;
				p++;
				len++;
			}

			/*
			 * Only escape backslashes before explicit double quotes or
			 * or where the backslashes are at the end of an argument
			 * that is scheduled to be quoted.
			 */
			if (*p == '"' || (force_quotes && *p == '\0')) {
				n += count*2 + 1;
			}

			if (*p == '\0') {
				break;
			}
			continue;
		}
		len++;
		p++;
	}

	if (!force_quotes && n == 0) {
		return (char*)arg;
	}

	/* insert double quotes and backslashes where necessary */
	d = q = xmalloc(len+n+3);
	if (force_quotes) {
		*d++ = '"';
	}

	while (*arg) {
		if (*arg == '"') {
			*d++ = '\\';
		}
		else if (*arg == '\\') {
			int count = 0;
			while (*arg == '\\') {
				count++;
				*d++ = *arg++;
			}

			if (*arg == '"' || (force_quotes && *arg == '\0')) {
				while (count-- > 0) {
					*d++ = '\\';
				}
				if (*arg == '"') {
					*d++ = '\\';
				}
			}
		}
		if (*arg != '\0') {
			*d++ = *arg++;
		}
	}
	if (force_quotes) {
		*d++ = '"';
	}
	*d = '\0';

	return q;
}

static inline int is_slash(char c)
{
	return c == '/' || c == '\\';
}

static int is_msys2_cmd(const char *cmd)
{
	int len = strlen(cmd);

	if (len < 9)
		return 0;

	while (len-- && !is_slash(cmd[len]))
		; /* do nothing */
	return len > 7 && is_slash(cmd[len - 8]) && is_slash(cmd[len - 4]) &&
		!_strnicmp(cmd + len - 7, "usr", 3) &&
		!_strnicmp(cmd + len - 3, "bin", 3);
}

static intptr_t
spawnveq(int mode, const char *path, char *const *argv, char *const *env)
{
	char **new_argv;
	int i, argc = 0;
	intptr_t ret;
	char *(*quote_arg)(const char *);

fprintf(stderr, "%s:%d: path=%s\n", __FILE__, __LINE__, path); fflush(stderr);
	if (!argv) {
		char *const empty_argv[] = { (char *)path, NULL };
		return mingw_spawnve(mode, path, empty_argv, env);
	}

	quote_arg = is_msys2_cmd(path) ? quote_arg_msys2 : quote_arg_mingw;
if (getenv("DEBUG_QUOTING")) { fprintf(stderr, "'%s' is MSYS2: %d\n", path, quote_arg == quote_arg_msys2); fflush(stderr); }
	while (argv[argc])
		argc++;

	new_argv = malloc(sizeof(*argv)*(argc+1));
	for (i = 0;i < argc;i++)
{
		new_argv[i] = quote_arg(argv[i]);
if (getenv("DEBUG_QUOTING")) { fprintf(stderr, "quoted arg #%d '%s' to '%s'\n", i, argv[i], new_argv[i]); fflush(stderr); }
}
	new_argv[argc] = NULL;
	ret = mingw_spawnve(mode, path, new_argv, env);
	for (i = 0;i < argc;i++)
		if (new_argv[i] != argv[i])
			free(new_argv[i]);
	free(new_argv);
	return ret;
}

static intptr_t
mingw_spawn_applet(int mode,
		   const char *applet,
		   char *const *argv,
		   char *const *envp)
{
	char **env = copy_environ(envp);
	char path[MAX_PATH+20];
	intptr_t ret;

fprintf(stderr, "%s:%d: applet=%s\n", __FILE__, __LINE__, applet); fflush(stderr);
	sprintf(path, "BUSYBOX_APPLET_NAME=%s", applet);
	env = env_setenv(env, path);
	ret = spawnveq(mode, (char *)get_busybox_exec_path(), argv, env);
	free_environ(env);
	return ret;
}

static intptr_t
mingw_spawn_interpreter(int mode, const char *prog, char *const *argv, char *const *envp)
{
	intptr_t ret;
	char **opts;
	int nopts;
	const char *interpr = parse_interpreter(prog, &opts, &nopts);
	char **new_argv;
	int argc = 0;

fprintf(stderr, "%s:%d: prog=%s\n", __FILE__, __LINE__, prog); fflush(stderr);
	if (!interpr)
		return spawnveq(mode, prog, argv, envp);


	while (argv[argc])
		argc++;
	new_argv = malloc(sizeof(*argv)*(argc+nopts+2));
	memcpy(new_argv+1, opts, sizeof(*opts)*nopts);
	memcpy(new_argv+nopts+2, argv+1, sizeof(*argv)*argc);
	new_argv[nopts+1] = (char *)prog; /* pass absolute path */

	if (ENABLE_FEATURE_PREFER_APPLETS && find_applet_by_name(interpr) >= 0) {
		new_argv[0] = (char *)interpr;
		ret = mingw_spawn_applet(mode, interpr, new_argv, envp);
	}
	else {
		char *path = xstrdup(getenv("PATH"));
		char *tmp = path;
		char *iprog = find_executable(interpr, &tmp);
		free(path);
		if (!iprog) {
			free(new_argv);
			errno = ENOENT;
			return -1;
		}
		new_argv[0] = iprog;
		ret = spawnveq(mode, iprog, new_argv, envp);
		free(iprog);
	}

	free(new_argv);
	return ret;
}

static intptr_t
mingw_spawn_1(int mode, const char *cmd, char *const *argv, char *const *envp)
{
	intptr_t ret;

fprintf(stderr, "%s:%d: cmd=%s\n", __FILE__, __LINE__, cmd); fflush(stderr);
	if (ENABLE_FEATURE_PREFER_APPLETS &&
	    find_applet_by_name(cmd) >= 0)
		return mingw_spawn_applet(mode, cmd, argv, envp);
	else if (is_absolute_path(cmd))
		return mingw_spawn_interpreter(mode, cmd, argv, envp);
	else {
		char *tmp, *path = getenv("PATH");
		char *prog;

		if (!path) {
			errno = ENOENT;
			return -1;
		}

		/* executable_exists() does not return new file name */
		tmp = path = xstrdup(path);
		prog = find_executable(cmd, &tmp);
		free(path);
		if (!prog) {
			errno = ENOENT;
			return -1;
		}
		ret = mingw_spawn_interpreter(mode, prog, argv, envp);
		free(prog);
	}
	return ret;
}

pid_t FAST_FUNC
mingw_spawn(char **argv)
{
	intptr_t ret;

fprintf(stderr, "%s:%d: argv[0]=%s\n", __FILE__, __LINE__, argv[0]); fflush(stderr);
	ret = mingw_spawn_1(P_NOWAIT, argv[0], (char *const *)argv, environ);

	return ret == -1 ? -1 : GetProcessId((HANDLE)ret);
}

intptr_t FAST_FUNC
mingw_spawn_proc(const char **argv)
{
fprintf(stderr, "%s:%d: argv[0]=%s\n", __FILE__, __LINE__, argv[0]); fflush(stderr);
	return mingw_spawn_1(P_NOWAIT, argv[0], (char *const *)argv, environ);
}

int
mingw_execvp(const char *cmd, char *const *argv)
{
fprintf(stderr, "%s:%d: cmd=%s\n", __FILE__, __LINE__, cmd); fflush(stderr);
{
	int ret = (int)mingw_spawn_1(P_WAIT, cmd, argv, environ);
	if (ret != -1)
		exit(ret);
	return ret;
}
}

int
mingw_execve(const char *cmd, char *const *argv, char *const *envp)
{
	int ret;
	int mode = P_WAIT;

fprintf(stderr, "%s:%d: cmd=%s\n", __FILE__, __LINE__, cmd); fflush(stderr);
	if (ENABLE_FEATURE_PREFER_APPLETS &&
	    find_applet_by_name(cmd) >= 0)
		ret = mingw_spawn_applet(mode, cmd, argv, envp);
	/*
	 * execve(bb_busybox_exec_path, argv, envp) won't work
	 * because argv[0] will be replaced to bb_busybox_exec_path
	 * by MSVC runtime
	 */
	else if (argv && cmd != argv[0] && cmd == bb_busybox_exec_path)
		ret = mingw_spawn_applet(mode, argv[0], argv, envp);
	else
		ret = mingw_spawn_interpreter(mode, cmd, argv, envp);
	if (ret != -1)
		exit(ret);
	return ret;
}

int
mingw_execv(const char *cmd, char *const *argv)
{
fprintf(stderr, "%s:%d: cmd=%s\n", __FILE__, __LINE__, cmd); fflush(stderr);
	return mingw_execve(cmd, argv, environ);
}

/* POSIX version in libbb/procps.c */
procps_status_t* FAST_FUNC procps_scan(procps_status_t* sp, int flags UNUSED_PARAM)
{
	PROCESSENTRY32 pe;

	pe.dwSize = sizeof(pe);
	if (!sp) {
		sp = xzalloc(sizeof(struct procps_status_t));
		sp->snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (sp->snapshot == INVALID_HANDLE_VALUE) {
			free(sp);
			return NULL;
		}
		if (!Process32First(sp->snapshot, &pe)) {
			CloseHandle(sp->snapshot);
			free(sp);
			return NULL;
		}
	}
	else {
		if (!Process32Next(sp->snapshot, &pe)) {
			CloseHandle(sp->snapshot);
			free(sp);
			return NULL;
		}
	}

	sp->pid = pe.th32ProcessID;
	safe_strncpy(sp->comm, pe.szExeFile, COMM_LEN);
	return sp;
}

/*
 * Terminates the process corresponding to the process ID and all of its
 * directly and indirectly spawned subprocesses.
 *
 * This way of terminating the processes is not gentle: the processes get
 * no chance of cleaning up after themselves (closing file handles, removing
 * .lock files, terminating spawned processes (if any), etc).
 */
static int terminate_process_tree(HANDLE main_process, int exit_status)
{
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	PROCESSENTRY32 entry;
	DWORD pids[16384];
	int max_len = sizeof(pids) / sizeof(*pids), i, len, ret = 0;
	pid_t pid = GetProcessId(main_process);

	pids[0] = (DWORD)pid;
	len = 1;

	/*
	 * Even if Process32First()/Process32Next() seem to traverse the
	 * processes in topological order (i.e. parent processes before
	 * child processes), there is nothing in the Win32 API documentation
	 * suggesting that this is guaranteed.
	 *
	 * Therefore, run through them at least twice and stop when no more
	 * process IDs were added to the list.
	 */
	for (;;) {
		int orig_len = len;

		memset(&entry, 0, sizeof(entry));
		entry.dwSize = sizeof(entry);

		if (!Process32First(snapshot, &entry))
			break;

		do {
			for (i = len - 1; i >= 0; i--) {
				if (pids[i] == entry.th32ProcessID)
					break;
				if (pids[i] == entry.th32ParentProcessID)
					pids[len++] = entry.th32ProcessID;
			}
		} while (len < max_len && Process32Next(snapshot, &entry));

		if (orig_len == len || len >= max_len)
			break;
	}

	for (i = len - 1; i > 0; i--) {
		HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, pids[i]);

		if (process) {
			if (!TerminateProcess(process, exit_status))
				ret = -1;
			CloseHandle(process);
		}
	}
	if (!TerminateProcess(main_process, exit_status))
		ret = -1;
	CloseHandle(main_process);

	return ret;
}

/**
 * Determine whether a process runs in the same architecture as the current
 * one. That test is required before we assume that GetProcAddress() returns
 * a valid address *for the target process*.
 */
static inline int process_architecture_matches_current(HANDLE process)
{
	static BOOL current_is_wow = -1;
	BOOL is_wow;

	if (current_is_wow == -1 &&
	    !IsWow64Process (GetCurrentProcess(), &current_is_wow))
		current_is_wow = -2;
	if (current_is_wow == -2)
		return 0; /* could not determine current process' WoW-ness */
	if (!IsWow64Process (process, &is_wow))
		return 0; /* cannot determine */
	return is_wow == current_is_wow;
}

/**
 * This function tries to terminate a Win32 process, as gently as possible.
 *
 * At first, we will attempt to inject a thread that calls ExitProcess(). If
 * that fails, we will fall back to terminating the entire process tree.
 *
 * Note: as kernel32.dll is loaded before any process, the other process and
 * this process will have ExitProcess() at the same address.
 *
 * This function expects the process handle to have the access rights for
 * CreateRemoteThread(): PROCESS_CREATE_THREAD, PROCESS_QUERY_INFORMATION,
 * PROCESS_VM_OPERATION, PROCESS_VM_WRITE, and PROCESS_VM_READ.
 *
 * The idea comes from the Dr Dobb's article "A Safer Alternative to
 * TerminateProcess()" by Andrew Tucker (July 1, 1999),
 * http://www.drdobbs.com/a-safer-alternative-to-terminateprocess/184416547
 *
 * If this method fails, we fall back to running terminate_process_tree().
 */
static int exit_process(HANDLE process, int exit_code)
{
	DWORD code;

	cull_exited_processes();
	if (GetExitCodeProcess(process, &code) && code == STILL_ACTIVE) {
		static int initialized;
		static LPTHREAD_START_ROUTINE exit_process_address;
		PVOID arg = (PVOID)(intptr_t)exit_code;
		DWORD thread_id;
		HANDLE thread = NULL;

		if (!initialized) {
			HINSTANCE kernel32 = GetModuleHandle("kernel32");
			if (!kernel32) {
				fprintf(stderr, "BUG: cannot find kernel32");
				return -1;
			}
			exit_process_address = (LPTHREAD_START_ROUTINE)
				GetProcAddress(kernel32, "ExitProcess");
			initialized = 1;
		}
		if (!exit_process_address ||
		    !process_architecture_matches_current(process))
			return terminate_process_tree(process, exit_code);

		thread = CreateRemoteThread(process, NULL, 0,
					    exit_process_address,
					    arg, 0, &thread_id);
		if (thread) {
			DWORD result;

			CloseHandle(thread);
			/*
			 * If the process survives for 10 seconds (a completely
			 * arbitrary value picked from thin air), fall back to
			 * killing the process tree via TerminateProcess().
			 */
			result = WaitForSingleObject(process, 10000);
			if (result == WAIT_OBJECT_0) {
				CloseHandle(process);
				return 0;
			}
		}

		return terminate_process_tree(process, exit_code);
	}

	return 0;
}

int kill(pid_t pid, int sig)
{
	HANDLE h;

	if (pid > 0 && sig == SIGTERM) {
		if ((h = OpenProcess(SYNCHRONIZE | PROCESS_CREATE_THREAD |
				PROCESS_QUERY_INFORMATION |
				PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
				PROCESS_VM_READ, FALSE, pid)))
			return exit_process(h, 128 + sig);
		if ((h=OpenProcess(PROCESS_TERMINATE, FALSE, pid)) != NULL &&
				terminate_process_tree(h, 128 + sig)) {
			CloseHandle(h);
			return 0;
		}

		errno = err_win_to_posix(GetLastError());
		if (h != NULL)
			CloseHandle(h);
		return -1;
	}

	errno = EINVAL;
	return -1;
}
