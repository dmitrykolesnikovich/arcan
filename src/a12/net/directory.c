#include <arcan_shmif.h>
#include <arcan_shmif_server.h>

#include <ftw.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdint.h>

#include "../a12.h"
#include "../a12_int.h"
#include "anet_helper.h"
#include "directory.h"

#include <sys/types.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>

struct cb_tag {
	struct appl_meta* dir;
	struct a12_state* S;
	struct anet_dircl_opts* clopt;
	FILE* outf;
};

static FILE* cmd_to_membuf(const char* cmd, char** out, size_t* out_sz)
{
	FILE* applin = popen(cmd, "r");
	if (!applin)
		return NULL;

	FILE* applbuf = open_memstream(out, out_sz);
	if (!applbuf){
		fclose(applin);
		return NULL;
	}

	char buf[4096];
	size_t nr;
	bool ok = true;

	while ((nr = fread(buf, 1, 4096, applin))){
		if (1 != fwrite(buf, nr, 1, applbuf)){
			ok = false;
			break;
		}
	}

	fclose(applin);
	if (!ok){
		fclose(applbuf);
		return NULL;
	}

/* actually keep both in order to allow appending elsewhere */
	fflush(applbuf);
	return applbuf;
}

/* This part is much more PoC - we'd need a nicer cache / store (sqlite?) so
 * that each time we start up, we don't have to rescan and the other end don't
 * have to redownload if nothing's changed. */
static size_t scan_appdir(int fd, struct appl_meta* dst)
{
	lseek(fd, 0, SEEK_SET);
	DIR* dir = fdopendir(fd);
	struct dirent* ent;
	size_t count = 0;

	while (dir && (ent = readdir(dir))){
		if (
			strlen(ent->d_name) >= 18 ||
			strcmp(ent->d_name, "..") == 0 || strcmp(ent->d_name, ".") == 0){
			continue;
		}

	/* just want directories */
		struct stat sbuf;
		if (-1 == stat(ent->d_name, &sbuf) || (sbuf.st_mode & S_IFMT) != S_IFDIR)
			continue;

		fchdir(fd);
		chdir(ent->d_name);

		struct appl_meta* new = malloc(sizeof(struct appl_meta));
		if (!new)
			break;

		*dst = (struct appl_meta){0};
		dst->handle = cmd_to_membuf("tar cf - .", &dst->buf, &dst->buf_sz);
		fchdir(fd);

		if (!dst->handle){
			free(new);
			continue;
		}
		dst->identifier = count++;

		blake3_hasher temp;
		blake3_hasher_init(&temp);
		blake3_hasher_update(&temp, dst->buf, dst->buf_sz);
		blake3_hasher_finalize(&temp, dst->hash, 4);
		snprintf(dst->applname, 18, "%s", ent->d_name);

		*new = (struct appl_meta){0};
		dst->next = new;
		dst = new;
	}

	closedir(dir);
	return count;
}

static void on_srv_event(
	struct arcan_shmif_cont* cont, int chid, struct arcan_event* ev, void* tag)
{
	struct cb_tag* cbt = tag;

/* the only concerns here are BCHUNK matching our directory IDs */
	if (ev->ext.kind == EVENT_EXTERNAL_BCHUNKSTATE){
/* sweep the directory, and when found: */
		if (!isdigit(ev->ext.bchunk.extensions[0])){
			a12int_trace(A12_TRACE_DIRECTORY, "event=bchunkstate:error=invalid_id");
			return;
		}

		uint16_t extid = (uint16_t)
			strtoul((char*)ev->ext.bchunk.extensions, NULL, 10);

		struct appl_meta* meta = cbt->dir;
		while (meta){
			if (extid == meta->identifier){
				a12int_trace(A12_TRACE_DIRECTORY,
					"event=bchunkstate:send=%s", meta->applname);
				a12_enqueue_blob(cbt->S, meta->buf, meta->buf_sz, meta->identifier);
				return;
			}
			meta = meta->next;
		}
		a12int_trace(A12_TRACE_DIRECTORY,
			"event=bchunkstate:error=no_match:id=%"PRIu16, extid);
	}
	else
		a12int_trace(A12_TRACE_DIRECTORY,
			"event=%s", arcan_shmif_eventstr(ev, NULL, 0));
}

static void ioloop(struct a12_state* S, void* tag, int fdin, int fdout, void
	(*on_event)(struct arcan_shmif_cont* cont, int chid, struct arcan_event*, void*),
	bool (*on_directory)(struct a12_state* S, struct appl_meta* dir, void*))
{
	int errmask = POLLERR | POLLNVAL | POLLHUP;
	struct pollfd fds[2] =
	{
		{.fd = fdin, .events = POLLIN | errmask},
		{.fd = fdout, .events = POLLOUT | errmask}
	};

	size_t n_fd = 1;
	uint8_t inbuf[9000];
	uint8_t* outbuf = NULL;
	uint64_t ts = 0;

	fcntl(fdin, F_SETFD, FD_CLOEXEC);
	fcntl(fdout, F_SETFD, FD_CLOEXEC);

	size_t outbuf_sz = a12_flush(S, &outbuf, A12_FLUSH_ALL);
	if (outbuf_sz)
		n_fd++;

/* regular simple processing loop, wait for DIRECTORY-LIST command */
	while (a12_ok(S) && -1 != poll(fds, n_fd, -1)){
		if (
			(fds[0].revents & errmask) ||
			(n_fd == 2 && fds[1].revents & errmask))
				break;

		if (n_fd == 2 && (fds[1].revents & POLLOUT) && outbuf_sz){
			ssize_t nw = write(fdout, outbuf, outbuf_sz);
			if (nw > 0){
				outbuf += nw;
				outbuf_sz -= nw;
			}
		}

		if (fds[0].revents & POLLIN){
			ssize_t nr = recv(fdin, inbuf, 9000, 0);
			if (-1 == nr && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR){
				a12int_trace(A12_TRACE_DIRECTORY, "shutdown:reason=rw_error");
				break;
			}
			else if (0 == nr){
				a12int_trace(A12_TRACE_DIRECTORY, "shutdown:reason=closed");
				break;
			}
			a12_unpack(S, inbuf, nr, tag, on_event);

/* check if there has been a change to the directory state after each unpack */
			uint64_t new_ts;
			if (on_directory){
				struct appl_meta* dir = a12int_get_directory(S, &new_ts);
				if (new_ts != ts){
					ts = new_ts;
					if (!on_directory(S, dir, tag))
						return;
				}
			}
		}

		if (!outbuf_sz){
			outbuf_sz = a12_flush(S, &outbuf, A12_FLUSH_ALL);
		}

		n_fd = outbuf_sz > 0 ? 2 : 1;
	}

}

/* this will just keep / cache the built .tars in memory, the startup times
 * will still be long and there is no detection when / if to rebuild or when
 * the state has changed - a better server would use sqlite and some basic
 * signalling. */
void anet_directory_srv_rescan(struct anet_dirsrv_opts* opts)
{
	opts->dir_count = scan_appdir(dup(opts->basedir), &opts->dir);
}

void anet_directory_srv(
	struct a12_state* S, struct anet_dirsrv_opts opts, int fdin, int fdout)
{
	struct cb_tag cbt = {
		.dir = &opts.dir,
		.S = S
	};

	if (!opts.dir_count){
		a12int_trace(A12_TRACE_DIRECTORY, "shutdown:reason=no_valid_appls");
		return;
	}

	a12int_set_directory(S, &opts.dir);
	ioloop(S, &cbt, fdin, fdout, on_srv_event, NULL);
}

static void on_cl_event(
	struct arcan_shmif_cont* cont, int chid, struct arcan_event* ev, void* tag)
{
/* the only concerns here are BCHUNK matching our directory IDs,
 * with the BCHUNK_IN, spin up a new fork + tar from stdin to temp applfolder
 * and exec into arcan_lwa */
	a12int_trace(A12_TRACE_DIRECTORY, "event=%s", arcan_shmif_eventstr(ev, NULL, 0));
}

static int cleancb(
	const char* path, const struct stat* s, int tfl, struct FTW* ftwbuf)
{
	if (remove(path))
		fprintf(stderr, "error during cleanup of %s\n", path);
	return 0;
}

static bool clean_appldir(const char* name, int basedir)
{
	if (-1 != basedir){
		fchdir(basedir);
	}

/* more careful would get the current pressure through rlimits and sweep until
 * we know how many real slots are available and break at that, better option
 * still would be to just keep this in a memfs like setup and rebuild the
 * scratch dir entirely */
	return 0 == nftw(name, cleancb, 32, FTW_DEPTH | FTW_PHYS);
}

static bool ensure_appldir(const char* name, int basedir)
{
/* this should also ensure that we have a correct statedir
 * and try to retrieve it if possible */
	if (-1 != basedir){
		fchdir(basedir);
	}

/* make sure we don't have a collision */
	clean_appldir(name, basedir);

	if (-1 == mkdir(name, S_IRWXU) || -1 == chdir(name)){
		fprintf(stderr, "Couldn't create [basedir]/%s\n", name);
		return false;
	}

	return true;
}

/* assumes that cwd points to our scratch folder for extracted appls */
static bool handover_exec(struct a12_state* S,
	const char* name, struct anet_dircl_opts* opts, int* state, size_t* state_sz)
{
	char buf[strlen(name) + sizeof("./")];
	snprintf(buf, sizeof(buf), "./%s", name);
	*state_sz = 0;

	const char* binary = "arcan";
	const char* cpath = getenv("ARCAN_CONNPATH");
	size_t maxlen = sizeof((struct sockaddr_un){0}.sun_path);
	char cpath_full[maxlen];

/* to avoid the connpaths duelling with an outer arcan, or the light security
 * issue of the loaded appl being able to namespace-collide/ override - first
 * make sure to resolve the current connpath to absolute - then set the XDG_
 * runtime dir to our scratch folder. */
	if (cpath){
		if (cpath[0] != '/'){
			char* basedir = getenv("XDG_RUNTIME_DIR");
			if (snprintf(cpath_full, maxlen, "%s/%s%s",
				basedir ? basedir : getenv("HOME"),
				basedir ? "" : ".", cpath) >= maxlen){
				fprintf(stderr,
					"Resolved path too long, cannot handover to arcan(_lwa)\n");
				return false;
				*state = -1;
			}
		}

		binary = "arcan_lwa";
	}

	char logfd_str[16];
	int pstdin[2], pstdout[2];

	if (-1 == pipe(pstdin) || -1 == pipe(pstdout)){
		fprintf(stderr,
			"Couldn't setup control pipe in arcan handover\n");
		return false;
		*state = -1;
	}

	snprintf(logfd_str, 16, "LOGFD:%d", pstdout[1]);

/*
 * The current default here is to collect crash- dumps or in-memory k/v store
 * synch and push them back to us, while taking controls from stdin.
 *
 * One thing that is important yet on the design- table is to allow the appl to
 * access a12- networked resources (or other protocols for that matter) and
 * have the request routed through the directory end and mapped to the
 * define_recordtarget, net_open, ... class of .lua functions.
 *
 * A use-case to 'test' against would be an appl that fetches a remote image
 * resource, takes a local webcam, overlays the remote image and streams the
 * result somewhere else - like a sink attached to the directory.
 */
	char* argv[] = {&buf[2],
		"-d", ":memory:",
		"-M", "-1",
		"-O", logfd_str,
		"-C",
		buf, NULL
	};

/* exec- over and monitor, keep connection alive */
	pid_t pid = fork();
	if (pid == 0){
/* remap control into STDIN */
		if (cpath)
			setenv("ARCAN_CONNPATH", cpath_full, 1);
		setenv("XDG_RUNTIME_DIR", "./", 1);
		dup2(pstdin[0], STDIN_FILENO);
		close(pstdin[0]);
		close(pstdin[1]);
		close(pstdout[0]);
		close(STDERR_FILENO);
		close(STDOUT_FILENO);
		open("/dev/null", O_WRONLY);
		open("/dev/null", O_WRONLY);
		execvp(binary, argv);
	}
	if (pid == -1){
		clean_appldir(name, opts->basedir);
		fprintf(stderr, "Couldn't spawn child process");
		*state = -1;
		return false;
	}
	close(pstdin[0]);
	close(pstdout[1]);

	FILE* pfin = fdopen(pstdin[1], "w");
	FILE* pfout = fdopen(pstdout[0], "r");
	setlinebuf(pfin);
	setlinebuf(pfout);

/* if we have state, now is a good time to do something with it */
	fprintf(pfin, "continue\n");

/* capture the state block, write into an unlinked tmp-file so the
 * file descriptor can be rewound and set as a bstream */
	int pret = 0;
	char* out = NULL;
	char filename[] = "statetemp-XXXXXX";
	int state_fd;
	if (-1 == (state_fd = mkstemp(filename))){
		fprintf(stderr, "Couldn't create temp-store, state transfer disabled\n");
	}
	else
		unlink(filename);

	while (!feof(pfout)){
		char buf[4096];

/* couldn't get more state, STDOUT is likely broken - process dead or dying */
		if (!fgets(buf, 4096, pfout)){
			while ((waitpid(pid, &pret, 0)) != pid
				&& (errno == EINTR || errno == EAGAIN)){}
		}

/* try to cache it to our temporary state store */
		else if (-1 != state_fd){
			size_t ntw = strlen(buf);
			size_t pos = 0;

/* normal POSIX write shenanigans */
			while (ntw){
				ssize_t nw = write(state_fd, &buf[pos], ntw);
				if (-1 == nw){
					if (errno == EAGAIN || errno == EINTR)
						continue;

					fprintf(stderr, "Out of space caching state, transfer disabled\n");
					close(state_fd);
					state_fd = -1;
					*state_sz = 0;
					break;
				}

				ntw -= nw;
				pos += nw;
				*state_sz += nw;
			}
		}
	}

/* exited successfully? then the state snapshot should only contain the K/V
 * dump, and if empty - nothing to do */
	clean_appldir(name, opts->basedir);
	*state = state_fd;

	if (WIFEXITED(pret) && !WEXITSTATUS(pret)){
		return true;
	}
/* exited with error code or sig(abrt,kill,...) */
	if (
		(WIFEXITED(pret) && WEXITSTATUS(pret)) ||
		WIFSIGNALED(pret))
	{
		;
	}
	else {
		fprintf(stderr, "unhandled application termination state\n");
	}

	return false;
}

static struct a12_bhandler_res cl_bevent(
	struct a12_state* S, struct a12_bhandler_meta M, void* tag)
{
	struct cb_tag* cbt = tag;
	struct a12_bhandler_res res = {
		.fd = -1,
		.flag = A12_BHANDLER_DONTWANT
	};

	switch (M.state){
	case A12_BHANDLER_COMPLETED:
		if (cbt->outf){
			int state_out = -1;
			size_t state_sz = 0;

			pclose(cbt->outf);
			cbt->outf = NULL;

/*
 * run the appl, collect state into *state_out and if need be, synch it onwards
 * - the a12 implementation will take care of the actual transfer and
 *   cancellation should the other end not care for our state synch
 *
 * this is a placeholder setup, the better approach is to keep track of the
 * child we are running while still processing other a12 events - to open up
 * for multiplexing any resources the child might want dynamically, as well
 * as handle 'push' updates to the appl itself and just 'force' through the
 * monitor interface.
 */
			bool exec_res = handover_exec(S,
				cbt->clopt->applname, cbt->clopt, &state_out, &state_sz);

			if (-1 != state_out){
				if (state_sz){
					a12_enqueue_bstream(S, state_out,
						exec_res ? A12_BTYPE_STATE : A12_BTYPE_CRASHDUMP,
						M.identifier, false, state_sz
					);
				}
				else
					close(state_out);
			}
		}
	break;
	case A12_BHANDLER_INITIALIZE:{
		if (cbt->outf){
			return res;
		}
/* could also use -C */
		if (!ensure_appldir(cbt->clopt->applname, cbt->clopt->basedir)){
			fprintf(stderr, "Couldn't create temporary appl directory");
			return res;
		}

		fprintf(stderr, "got identifier: %"PRIu32"\n", M.identifier);

/* for restoring the DB, can simply popen to arcan_db with the piped
 * mode (arcan_db add_appl_kv basename key value) and send a SIGUSR2
 * to the process to indicate that the state has been updated. */
		cbt->outf = popen("tar xf -", "w");
		res.fd = fileno(cbt->outf);
		res.flag = A12_BHANDLER_NEWFD;

		if (-1 != cbt->clopt->basedir)
			fchdir(cbt->clopt->basedir);
		else
			chdir("..");
	}
	break;
/* set to fail for now? */
	case A12_BHANDLER_CANCELLED:
		fprintf(stderr, "appl download cancelled\n");
		if (cbt->outf){
			pclose(cbt->outf);
			cbt->outf = NULL;
		}
		clean_appldir(cbt->clopt->applname, cbt->clopt->basedir);
	break;
	}

	return res;
}

static bool cl_got_dir(struct a12_state* S, struct appl_meta* M, void* tag)
{
	struct cb_tag* cbt = tag;

	struct anet_dircl_opts* opts = tag;
	while (M){

/* use identifier to request binary */
		if (cbt->clopt->applname){
			if (strcasecmp(M->applname, cbt->clopt->applname) == 0){
				struct arcan_event ev =
				{
					.ext.kind = ARCAN_EVENT(BCHUNKSTATE),
					.category = EVENT_EXTERNAL,
					.ext.bchunk = {
						.input = true,
						.hint = false
					}
				};
				snprintf(
					(char*)ev.ext.bchunk.extensions, 6, "%"PRIu16, M->identifier);
				a12_channel_enqueue(S, &ev);

/* and register our store+launch handler */
				a12_set_bhandler(S, cl_bevent, tag);
				return true;
			}
		}
		else
			printf("name=%s\n", M->applname);
		M = M->next;
	}

	if (cbt->clopt->applname){
		fprintf(stderr, "appl:%s not found\n", cbt->clopt->applname);
		return false;
	}

	if (cbt->clopt->die_on_list)
		return false;

	return true;
}

void anet_directory_cl(
	struct a12_state* S, struct anet_dircl_opts opts, int fdin, int fdout)
{
	struct cb_tag cbt = {
		.S = S,
		.clopt = &opts
	};

	sigaction(SIGPIPE,&(struct sigaction){.sa_handler = SIG_IGN}, 0);

/* always request dirlist so we can resolve applname against the server-local
 * ID as that might change */
	a12int_request_dirlist(S, false);
	ioloop(S, &cbt, fdin, fdout, on_cl_event, cl_got_dir);
}