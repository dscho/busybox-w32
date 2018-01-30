#include "libbb.h"

#undef getenv
#undef putenv

char *mingw_getenv(const char *name)
{
	char *result = getenv(name);
	if (!result && !strcmp(name, "TMPDIR")) {
		/* on Windows it is TMP and TEMP */
		result = getenv("TMP");
		if (!result)
			result = getenv("TEMP");
	}
	return result;
}

int setenv(const char *name, const char *value, int replace)
{
	int out;
	char *envstr;

	if (!name || !*name || strchr(name, '=') || !value) return -1;
	if (!replace) {
		if (getenv(name)) return 0;
	}

	envstr = xasprintf("%s=%s", name, value);
	out = mingw_putenv(envstr);
	free(envstr);

	return out;
}

/*
 * Removing an environment variable with WIN32 putenv requires an argument
 * like "NAME="; glibc omits the '='.  The implementations of unsetenv and
 * clearenv allow for this.
 *
 * It isn't possible to create an environment variable with an empty value
 * using WIN32 putenv.
 */
int unsetenv(const char *name)
{
	char *envstr;
	int ret;

	if (!name || !*name || strchr(name, '=') ) {
		return -1;
	}

	envstr = xmalloc(strlen(name)+2);
	strcat(strcpy(envstr, name), "=");
	ret = putenv(envstr);
	free(envstr);

	return ret;
}

int clearenv(void)
{
	int ret = 0;
	LPWSTR env, p;
	WCHAR buf[32768];

	p = env = GetEnvironmentStringsW();
	while (p && *p) {
		int len = wcslen(p);
		LPWSTR equal = wcschr(p, L'=');

		if (equal) {
			wcsncpy(buf, p, equal - p);
			buf[equal - p] = L'\0';
			if (!SetEnvironmentVariableW(buf, NULL)) {
				ret = -1;
				break;
			}
		}
		p += len + 1;
	}

	FreeEnvironmentStringsW(env);

	return ret;
}

int mingw_putenv(const char *env)
{
	char *s;

	if ( (s=strchr(env, '=')) == NULL ) {
		return unsetenv(env);
	}

	if ( s[1] != '\0' ) {
		return putenv(env);
	}

	/* can't set empty value */
	return 0;
}
