#include "cache.h"
#include "run-command.h"
#include "exec_cmd.h"

static inline void close_pair(int fd[2])
{
	close(fd[0]);
	close(fd[1]);
}

static inline void dup_devnull(int to)
{
	int fd = open("/dev/null", O_RDWR);
	dup2(fd, to);
	close(fd);
}

static const char **prepare_shell_cmd(const char **argv)
{
	int argc, nargc = 0;
	const char **nargv;

	for (argc = 0; argv[argc]; argc++)
		; /* just counting */
	/* +1 for NULL, +3 for "sh -c" plus extra $0 */
	nargv = xmalloc(sizeof(*nargv) * (argc + 1 + 3));

	if (argc < 1)
		die("BUG: shell command is empty");

	if (strcspn(argv[0], "|&;<>()$`\\\"' \t\n*?[#~=%") != strlen(argv[0])) {
		nargv[nargc++] = "sh";
		nargv[nargc++] = "-c";

		if (argc < 2)
			nargv[nargc++] = argv[0];
		else {
			struct strbuf arg0 = STRBUF_INIT;
			strbuf_addf(&arg0, "%s \"$@\"", argv[0]);
			nargv[nargc++] = strbuf_detach(&arg0, NULL);
		}
	}

	for (argc = 0; argv[argc]; argc++)
		nargv[nargc++] = argv[argc];
	nargv[nargc] = NULL;

	return nargv;
}

#ifndef WIN32
static int execv_shell_cmd(const char **argv)
{
	const char **nargv = prepare_shell_cmd(argv);
	trace_argv_printf(nargv, "trace: exec:");
	execvp(nargv[0], (char **)nargv);
	free(nargv);
	return -1;
}
#endif

int start_command(struct child_process *cmd)
{
	int need_in, need_out, need_err;
	int fdin[2], fdout[2], fderr[2];
	int failed_errno = failed_errno;

	/*
	 * In case of errors we must keep the promise to close FDs
	 * that have been passed in via ->in and ->out.
	 */

	need_in = !cmd->no_stdin && cmd->in < 0;
	if (need_in) {
		if (pipe(fdin) < 0) {
			failed_errno = errno;
			if (cmd->out > 0)
				close(cmd->out);
			goto fail_pipe;
		}
		cmd->in = fdin[1];
	}

	need_out = !cmd->no_stdout
		&& !cmd->stdout_to_stderr
		&& cmd->out < 0;
	if (need_out) {
		if (pipe(fdout) < 0) {
			failed_errno = errno;
			if (need_in)
				close_pair(fdin);
			else if (cmd->in)
				close(cmd->in);
			goto fail_pipe;
		}
		cmd->out = fdout[0];
	}

	need_err = !cmd->no_stderr && cmd->err < 0;
	if (need_err) {
		if (pipe(fderr) < 0) {
			failed_errno = errno;
			if (need_in)
				close_pair(fdin);
			else if (cmd->in)
				close(cmd->in);
			if (need_out)
				close_pair(fdout);
			else if (cmd->out)
				close(cmd->out);
fail_pipe:
			error("cannot create pipe for %s: %s",
				cmd->argv[0], strerror(failed_errno));
			errno = failed_errno;
			return -1;
		}
		cmd->err = fderr[0];
	}

	trace_argv_printf(cmd->argv, "trace: run_command:");

#ifndef WIN32
	fflush(NULL);
	cmd->pid = fork();
	if (!cmd->pid) {
		if (cmd->no_stdin)
			dup_devnull(0);
		else if (need_in) {
			dup2(fdin[0], 0);
			close_pair(fdin);
		} else if (cmd->in) {
			dup2(cmd->in, 0);
			close(cmd->in);
		}

		if (cmd->no_stderr)
			dup_devnull(2);
		else if (need_err) {
			dup2(fderr[1], 2);
			close_pair(fderr);
		}

		if (cmd->no_stdout)
			dup_devnull(1);
		else if (cmd->stdout_to_stderr)
			dup2(2, 1);
		else if (need_out) {
			dup2(fdout[1], 1);
			close_pair(fdout);
		} else if (cmd->out > 1) {
			dup2(cmd->out, 1);
			close(cmd->out);
		}

		if (cmd->dir && chdir(cmd->dir))
			die_errno("exec '%s': cd to '%s' failed", cmd->argv[0],
			    cmd->dir);
		if (cmd->env) {
			for (; *cmd->env; cmd->env++) {
				if (strchr(*cmd->env, '='))
					putenv((char *)*cmd->env);
				else
					unsetenv(*cmd->env);
			}
		}
		if (cmd->preexec_cb)
			cmd->preexec_cb();
		if (cmd->git_cmd) {
			execv_git_cmd(cmd->argv);
		} else if (cmd->use_shell) {
			execv_shell_cmd(cmd->argv);
		} else {
			execvp(cmd->argv[0], (char *const*) cmd->argv);
		}
		trace_printf("trace: exec '%s' failed: %s\n", cmd->argv[0],
				strerror(errno));
		exit(127);
	}
	if (cmd->pid < 0)
		error("cannot fork() for %s: %s", cmd->argv[0],
			strerror(failed_errno = errno));
#else
{
	int s0 = -1, s1 = -1, s2 = -1;	/* backups of stdin, stdout, stderr */
	const char **sargv = cmd->argv;
	char **env = environ;

	if (cmd->no_stdin) {
		s0 = dup(0);
		dup_devnull(0);
	} else if (need_in) {
		s0 = dup(0);
		dup2(fdin[0], 0);
	} else if (cmd->in) {
		s0 = dup(0);
		dup2(cmd->in, 0);
	}

	if (cmd->no_stderr) {
		s2 = dup(2);
		dup_devnull(2);
	} else if (need_err) {
		s2 = dup(2);
		dup2(fderr[1], 2);
	}

	if (cmd->no_stdout) {
		s1 = dup(1);
		dup_devnull(1);
	} else if (cmd->stdout_to_stderr) {
		s1 = dup(1);
		dup2(2, 1);
	} else if (need_out) {
		s1 = dup(1);
		dup2(fdout[1], 1);
	} else if (cmd->out > 1) {
		s1 = dup(1);
		dup2(cmd->out, 1);
	}

	if (cmd->dir)
		die("chdir in start_command() not implemented");
	if (cmd->env)
		env = make_augmented_environ(cmd->env);

	if (cmd->git_cmd) {
		cmd->argv = prepare_git_cmd(cmd->argv);
	} else if (cmd->use_shell) {
		cmd->argv = prepare_shell_cmd(cmd->argv);
	}

	cmd->pid = mingw_spawnvpe(cmd->argv[0], cmd->argv, env);
	failed_errno = errno;
	if (cmd->pid < 0 && (!cmd->silent_exec_failure || errno != ENOENT))
		error("cannot spawn %s: %s", cmd->argv[0], strerror(errno));

	if (cmd->env)
		free_environ(env);
	if (cmd->git_cmd)
		free(cmd->argv);

	cmd->argv = sargv;
	if (s0 >= 0)
		dup2(s0, 0), close(s0);
	if (s1 >= 0)
		dup2(s1, 1), close(s1);
	if (s2 >= 0)
		dup2(s2, 2), close(s2);
}
#endif

	if (cmd->pid < 0) {
		if (need_in)
			close_pair(fdin);
		else if (cmd->in)
			close(cmd->in);
		if (need_out)
			close_pair(fdout);
		else if (cmd->out)
			close(cmd->out);
		if (need_err)
			close_pair(fderr);
		errno = failed_errno;
		return -1;
	}

	if (need_in)
		close(fdin[0]);
	else if (cmd->in)
		close(cmd->in);

	if (need_out)
		close(fdout[1]);
	else if (cmd->out)
		close(cmd->out);

	if (need_err)
		close(fderr[1]);

	return 0;
}

static int wait_or_whine(pid_t pid, const char *argv0, int silent_exec_failure)
{
	int status, code = -1;
	pid_t waiting;
	int failed_errno = 0;

	while ((waiting = waitpid(pid, &status, 0)) < 0 && errno == EINTR)
		;	/* nothing */

	if (waiting < 0) {
		failed_errno = errno;
		error("waitpid for %s failed: %s", argv0, strerror(errno));
	} else if (waiting != pid) {
		error("waitpid is confused (%s)", argv0);
	} else if (WIFSIGNALED(status)) {
		code = WTERMSIG(status);
		error("%s died of signal %d", argv0, code);
		/*
		 * This return value is chosen so that code & 0xff
		 * mimics the exit code that a POSIX shell would report for
		 * a program that died from this signal.
		 */
		code -= 128;
	} else if (WIFEXITED(status)) {
		code = WEXITSTATUS(status);
		/*
		 * Convert special exit code when execvp failed.
		 */
		if (code == 127) {
			code = -1;
			failed_errno = ENOENT;
			if (!silent_exec_failure)
				error("cannot run %s: %s", argv0,
					strerror(ENOENT));
		}
	} else {
		error("waitpid is confused (%s)", argv0);
	}
	errno = failed_errno;
	return code;
}

int finish_command(struct child_process *cmd)
{
	return wait_or_whine(cmd->pid, cmd->argv[0], cmd->silent_exec_failure);
}

int run_command(struct child_process *cmd)
{
	int code = start_command(cmd);
	if (code)
		return code;
	return finish_command(cmd);
}

static void prepare_run_command_v_opt(struct child_process *cmd,
				      const char **argv,
				      int opt)
{
	memset(cmd, 0, sizeof(*cmd));
	cmd->argv = argv;
	cmd->no_stdin = opt & RUN_COMMAND_NO_STDIN ? 1 : 0;
	cmd->git_cmd = opt & RUN_GIT_CMD ? 1 : 0;
	cmd->stdout_to_stderr = opt & RUN_COMMAND_STDOUT_TO_STDERR ? 1 : 0;
	cmd->silent_exec_failure = opt & RUN_SILENT_EXEC_FAILURE ? 1 : 0;
	cmd->use_shell = opt & RUN_USING_SHELL ? 1 : 0;
}

int run_command_v_opt(const char **argv, int opt)
{
	struct child_process cmd;
	prepare_run_command_v_opt(&cmd, argv, opt);
	return run_command(&cmd);
}

int run_command_v_opt_cd_env(const char **argv, int opt, const char *dir, const char *const *env)
{
	struct child_process cmd;
	prepare_run_command_v_opt(&cmd, argv, opt);
	cmd.dir = dir;
	cmd.env = env;
	return run_command(&cmd);
}

#ifdef WIN32
static unsigned __stdcall run_thread(void *data)
{
	struct async *async = data;
	return async->proc(async->fd_for_proc, async->data);
}
#endif

int start_async(struct async *async)
{
	int pipe_out[2];

	if (pipe(pipe_out) < 0)
		return error("cannot create pipe: %s", strerror(errno));
	async->out = pipe_out[0];

#ifndef WIN32
	/* Flush stdio before fork() to avoid cloning buffers */
	fflush(NULL);

	async->pid = fork();
	if (async->pid < 0) {
		error("fork (async) failed: %s", strerror(errno));
		close_pair(pipe_out);
		return -1;
	}
	if (!async->pid) {
		close(pipe_out[0]);
		exit(!!async->proc(pipe_out[1], async->data));
	}
	close(pipe_out[1]);
#else
	async->fd_for_proc = pipe_out[1];
	async->tid = (HANDLE) _beginthreadex(NULL, 0, run_thread, async, 0, NULL);
	if (!async->tid) {
		error("cannot create thread: %s", strerror(errno));
		close_pair(pipe_out);
		return -1;
	}
#endif
	return 0;
}

int finish_async(struct async *async)
{
#ifndef WIN32
	int ret = wait_or_whine(async->pid, "child process", 0);
#else
	DWORD ret = 0;
	if (WaitForSingleObject(async->tid, INFINITE) != WAIT_OBJECT_0)
		ret = error("waiting for thread failed: %lu", GetLastError());
	else if (!GetExitCodeThread(async->tid, &ret))
		ret = error("cannot get thread exit code: %lu", GetLastError());
	CloseHandle(async->tid);
#endif
	return ret;
}

int run_hook(const char *index_file, const char *name, ...)
{
	struct child_process hook;
	const char **argv = NULL, *env[2];
	char index[PATH_MAX];
	va_list args;
	int ret;
	size_t i = 0, alloc = 0;

	if (access(git_path("hooks/%s", name), X_OK) < 0)
		return 0;

	va_start(args, name);
	ALLOC_GROW(argv, i + 1, alloc);
	argv[i++] = git_path("hooks/%s", name);
	while (argv[i-1]) {
		ALLOC_GROW(argv, i + 1, alloc);
		argv[i++] = va_arg(args, const char *);
	}
	va_end(args);

	memset(&hook, 0, sizeof(hook));
	hook.argv = argv;
	hook.no_stdin = 1;
	hook.stdout_to_stderr = 1;
	if (index_file) {
		snprintf(index, sizeof(index), "GIT_INDEX_FILE=%s", index_file);
		env[0] = index;
		env[1] = NULL;
		hook.env = env;
	}

	ret = run_command(&hook);
	free(argv);
	return ret;
}
