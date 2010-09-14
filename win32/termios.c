#include "libbb.h"

int tcsetattr(int fd UNUSED_PARAM, int mode UNUSED_PARAM,  const struct termios *t UNUSED_PARAM)
{
	return -1;
}

int tcgetattr(int fd UNUSED_PARAM, struct termios *t UNUSED_PARAM)
{
	return -1;
}

int64_t FAST_FUNC read_key(int fd, char *buf, int timeout UNUSED_PARAM)
{
	HANDLE cin = GetStdHandle(STD_INPUT_HANDLE);
	INPUT_RECORD record;
	DWORD nevent_out, mode;
	int ret = -1;

	if (fd != 0)
		bb_error_msg_and_die("read_key only works on stdin");
	if (cin == INVALID_HANDLE_VALUE)
		return -1;
	GetConsoleMode(cin, &mode);
	SetConsoleMode(cin, 0);

	while (1) {
		if (!ReadConsoleInput(cin, &record, 1, &nevent_out))
			goto done;
		if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown)
			continue;
		if (!record.Event.KeyEvent.uChar.AsciiChar)
			continue;
		ret = record.Event.KeyEvent.uChar.AsciiChar;
		break;
	}
 done:
	SetConsoleMode(cin, mode);
	return ret;
}
