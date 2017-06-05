#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <ctype.h>
#include <unistd.h>
#include <mntent.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/reboot.h>
#include <linux/reboot.h>
#include <grp.h>
#include <pwd.h>

#include "util.h"
#include "hyper.h"
#include "container.h"
#include "../config.h"

#define STATELESS_PASSWD_FILE "/usr/share/defaults/etc/passwd"
#define STATELESS_GROUP_FILE "/usr/share/defaults/etc/group"

/*
 * This structure defines the default password.
 * It is used when no password file could be found.
 */
static struct passwd default_passwd = {
	.pw_name = "root",
	.pw_uid = 0,
	.pw_gid = 0,
	.pw_gecos = "root",
	/* hello-world container has not '/root', let's use '/' instead */
	.pw_dir = "/"
};

char *read_cmdline(void)
{
	return NULL;
}

int hyper_setup_env(struct env *envs, int num)
{
	int i, ret = 0;
	struct env *env;

	for (i = 0; i < num; i++) {
		env = &envs[i];
		if (setenv(env->env, env->value, 1) < 0) {
			perror("fail to setup env");
			ret = -1;
		}
	}

	return ret;
}

int hyper_list_dir(char *path)
{
	struct dirent **list;
	struct dirent *dir;
	int i, num;

	fprintf(stdout, "list %s\n", path);
	num = scandir(path, &list, NULL, NULL);
	if (num < 0) {
		perror("scan path failed");
		return -1;
	}

	for (i = 0; i < num; i++) {
		dir = list[i];
		fprintf(stdout, "%s get %s\n", path, dir->d_name);
		free(dir);
	}

	free(list);
	return 0;
}

int hyper_copy_dir(char *src, char *dest)
{
	char cmd[512];
	snprintf(cmd, sizeof(cmd), "tar cf - -C %s . | tar fx - -C %s", src, dest);

	return hyper_cmd(cmd, NULL, NULL);
}

void hyper_sync_time_hctosys() {
	int pid;
	pid = fork();
	if (pid < 0) {
		perror("fail to fork to copy directory");
	} else if (pid == 0) {
		execlp("hwclock", "hwclock", "-s", NULL);
		perror("exec hwclock -s command failed");
		exit(-1);
	}
}

int hyper_find_sd(char *addr, char **dev)
{
	struct dirent **list;
	struct dirent *dir;
	char path[512];
	int i, num;

	sprintf(path, "/sys/class/scsi_disk/0:0:%s/device/block/", addr);
	fprintf(stdout, "orig dev %s, scan path %s\n", *dev, path);

	num = scandir(path, &list, NULL, NULL);
	if (num < 0) {
		perror("scan path failed");
		return -1;
	}

	for (i = 0; i < num; i++) {
		dir = list[i];
		if (dir->d_name[0] == '.') {
			continue;
		}

		fprintf(stdout, "%s get %s\n", path, dir->d_name);
		*dev = strdup(dir->d_name);
		break;
	}

	for (i = 0; i < num; i++)
		free(list[i]);

	free(list);
	return 0;
}

static unsigned long id_or_max(const char *name)
{
	char *ptr;
	errno = 0;
	long id = strtol(name, &ptr, 10);
	if (name == ptr || id < 0 || (errno != 0 && id == 0) || *ptr != '\0')
		return ~0UL;
	return id;
}

// Checks if the name provided is a numeric value and does the conversion.
bool hyper_name_to_id(const char *name, unsigned long *val)
{
	char *ptr;
	errno = 0;
	long id = strtol(name, &ptr, 10);
	if (name == ptr || id < 0 || (errno != 0 && id == 0) || *ptr != '\0')
		return false;
	*val = id;
	return true;
}

struct passwd *hyper_getpwnam_from_file(char *path, const char *name, uid_t uid)
{
	FILE *file = fopen(path, "r");

	if (!file) {
		fprintf(stderr, "failed to open file %s: %s\n",
				path, strerror(errno));
		return NULL;
	}
	for (;;) {
		struct passwd *pwd = fgetpwent(file);
		if (!pwd)
			break;
		if (!strcmp(pwd->pw_name, name) || pwd->pw_uid == uid) {
			fclose(file);
			return pwd;
		}
	}
	fclose(file);
	return NULL;
}

/* the same as getpwnam(), but it only parses "/etc/passwd" and stateless file 
 * "/usr/share/defaults/etc/passwd" and allows name to be id string.
 */
struct passwd *hyper_getpwnam(const char *name)
{
	struct passwd *pwd;
	uid_t uid = (uid_t)id_or_max(name);

	pwd = hyper_getpwnam_from_file("/etc/passwd", name, uid);
	if ( !pwd) {
		pwd = hyper_getpwnam_from_file(STATELESS_PASSWD_FILE, name, uid);
		if ( !pwd) {
			/* return default passwd to avoid errors */
			pwd = &default_passwd;
		}
	}

	return pwd;
}

struct group *hyper_getgrnam_from_file(char *path, const char *name, gid_t gid)
{
	FILE *file;

	if (!path || !name) {
		return NULL;
	}
	file = fopen(path, "r");

	if (!file) {
		fprintf(stderr, "failed to open file %s\n", path);
		return NULL;
	}
	for (;;) {
		struct group *gr = fgetgrent(file);
		if (!gr)
			break;
		if (!strcmp(gr->gr_name, name) || gr->gr_gid == gid) {
			fclose(file);
			return gr;
		}
	}
	fclose(file);
	return NULL;
}


/* the same as getgrnam(), but it only parses /etc/group and /usr/share/defaults/etc/group
 * and allows the name to be id string
 */
struct group *hyper_getgrnam(const char *name)
{
	struct group *grp;
	gid_t gid = (gid_t)id_or_max(name);

	grp = hyper_getgrnam_from_file("/etc/group", name, gid);
	if (!grp)
		grp = hyper_getgrnam_from_file(STATELESS_GROUP_FILE,
						 name, gid);
	return grp;
}

// the same as getgrouplist(), but it only parses /etc/group
int hyper_getgrouplist(const char *user, gid_t group, gid_t *groups, int *ngroups)
{
	int nr = 0, ret;
	bool stateless = false;

	FILE *file= fopen("/etc/group", "r");
	if (!file) {
		perror("failed to open /etc/group, trying stateless file to get group list");
		file = fopen(STATELESS_GROUP_FILE, "r");
		stateless = true;
		if (!file) {
			perror("not able to open stateless file either");
			/* This is not fatal, let's set ngroups to '0' and return '0' */
			ngroups = 0;
			return 0;
		}
	}

	/* We are parsing both the etc and stateless files here,
	 * trying to add groups from both. Not sure if we should just read one
         */
	for (;;) {
		struct group *gr = fgetgrent(file);

		/* All entries in both /etc and stateless group files
		 * have been parsed.
		 */
		if (!gr && stateless) {
			break;
		} else if ( !gr && !stateless ) {
			/* Try to parse the stateless file if present
			*/
			stateless = true;
			fclose(file);
			file = fopen(STATELESS_GROUP_FILE, "r");
			if (!file)
				break;
			else
				continue;
		}

		int j;
		for (j = 0; gr->gr_mem && gr->gr_mem[j]; j++) {
			if (!strcmp(gr->gr_mem[j], user)) {
				if (nr + 1 < *ngroups)
					groups[nr] = gr->gr_gid;
				nr++;
			}
		}
	}

	if (file)
		fclose(file);

	if (nr == 0) {
		if (nr + 1 < *ngroups)
			groups[nr] = group;
		nr++;
	}
	ret = nr <= *ngroups ? nr : -1;
	*ngroups = nr;
	return ret;
}

int hyper_write_file(const char *path, const char *value, size_t len)
{
	size_t size = 0;
	ssize_t l;
	int fd = open(path, O_WRONLY);
	if (fd < 0) {
		perror("open file failed");
		return -1;
	}

	while (size < len) {
		l = write(fd, value + size, len - size);
		if (l < 0) {
			perror("fail to write to file");
			close(fd);
			return -1;
		}
		size += l;
	}

	close(fd);
	return 0;
}

/* Trim all trailing '/' of a hyper_path except for the prefix one. */
void hyper_filize(char *hyper_path)
{
	char *p;

	if (strlen(hyper_path) == 0)
		return;

	p = &hyper_path[strlen(hyper_path) - 1];

	for (; *p == '/' && p != hyper_path; p--) {
		*p = '\0';
	}
}

static int hyper_create_parent_dir(const char *hyper_path)
{
	char *p, *path = strdup(hyper_path);
	int ret = 0;

	if (path == NULL)
		return -1;
	p = strrchr(path, '/');
	if (p != NULL && p != path) {
		*p = '\0';
		ret = hyper_mkdir(path, 0777);
	}
	free(path);

	return ret;
}

/* hyper_path must point to a file rather than a directory, e.g., having trailing '/' */
int hyper_create_file(const char *hyper_path)
{
	int fd;
	struct stat stbuf;

	if (stat(hyper_path, &stbuf) >= 0) {
		if (S_ISREG(stbuf.st_mode))
			return 0;
		errno = S_ISDIR(stbuf.st_mode) ? EISDIR : EINVAL;
		return -1;
	}

	if (hyper_create_parent_dir(hyper_path) < 0)
		return -1;

	fd = open(hyper_path, O_CREAT|O_WRONLY, 0666);
	if (fd < 0)
		return -1;
	close(fd);
	fprintf(stdout, "created file %s\n", hyper_path);
	return 0;
}

int hyper_mkdir(char *hyper_path, mode_t mode)
{
	struct stat st;
	char *p, *path = strdup(hyper_path);

	if (path == NULL) {
		errno = ENOMEM;
		goto fail;
	}

	if (stat(path, &st) >= 0) {
		if (S_ISDIR(st.st_mode))
			goto out;
		errno = ENOTDIR;
		goto fail;
	}

	if (errno != ENOENT)
		goto fail;

	p = strrchr(path, '/');
	if (p == NULL) {
		errno = EINVAL;
		goto fail;
	}

	if (p != path) {
		*p = '\0';

		if (hyper_mkdir(path, mode) < 0)
			goto fail;

		*p = '/';
	}

	fprintf(stdout, "create directory %s\n", path);
	if (mkdir(path, mode) < 0 && errno != EEXIST) {
		perror("failed to create directory");
		goto fail;
	}
out:
	free(path);
	return 0;

fail:
	free(path);
	return -1;
}

void online_cpu(void)
{
	DIR *dir = opendir("/sys/devices/system/cpu");
	if (dir == NULL) {
		fprintf(stderr, "open dir /sys/devices/system/cpu failed\n");
		return;
	}
	printf("online_cpu()\n");
	for (;;) {
		int num;
		int ret;
		char path[256];
		int fd;

		struct dirent *entry = readdir(dir);
		if (entry == NULL)
			break;
		if (entry->d_type != DT_DIR)
			continue;
		ret = sscanf(entry->d_name, "cpu%d", &num);
		if (ret < 1 || num == 0) /* skip none cpu%d and cpu0 */
			continue;
		sprintf(path, "/sys/devices/system/cpu/%s/online", entry->d_name);
		fd = open(path, O_RDWR);
		if (fd < 0) {
			fprintf(stderr, "open %s failed\n", path);
			continue;
		}
		printf("try to online %s\n", entry->d_name);
		ret = write(fd, "1", sizeof("1"));
		printf("online %s result: %s\n", entry->d_name, ret == 2 ? "success" : "failed");
		close(fd);
	}
	closedir(dir);
}

void online_memory(void)
{
	DIR *dir = opendir("/sys/devices/system/memory");
	if (dir == NULL) {
		fprintf(stderr, "open dir /sys/devices/system/memory failed\n");
		return;
	}
	printf("online_memory()\n");
	for (;;) {
		int num;
		int ret;
		char path[256];
		int fd;

		struct dirent *entry = readdir(dir);
		if (entry == NULL)
			break;
		if (entry->d_type != DT_DIR)
			continue;
		ret = sscanf(entry->d_name, "memory%d", &num);
		if (ret < 1 || num == 0) /* skip none memory%d and memory0 */
			continue;
		sprintf(path, "/sys/devices/system/memory/%s/online", entry->d_name);
		fd = open(path, O_RDWR);
		if (fd < 0) {
			fprintf(stderr, "open %s failed\n", path);
			continue;
		}
		printf("try to online %s\n", entry->d_name);
		ret = write(fd, "1", sizeof("1"));
		printf("online %s result: %s\n", entry->d_name, ret == 2 ? "success" : "failed");
		close(fd);
	}
	closedir(dir);
}

#if WITH_VBOX

#include <termios.h>
int hyper_open_channel(char *channel, int mode)
{
	struct termios term;
	int fd = open(channel, O_RDWR | O_CLOEXEC | mode);
	if (fd < 0) {
		perror("fail to open channel device");
		return -1;
	}

	fprintf(stdout, "open %s get %d\n", channel, fd);
	bzero(&term, sizeof(term));

	cfmakeraw(&term);
	term.c_cflag |= CLOCAL | CREAD| CRTSCTS;
	term.c_cc[VTIME] = 0;
	term.c_cc[VMIN] = 0;

	cfsetispeed(&term, B115200);
	cfsetospeed(&term, B115200);

	tcsetattr(fd, TCSANOW, &term);

	return fd;
}

static const char *moderror(int err)
{
	switch (err) {
	case ENOEXEC:
		return "Invalid module format";
	case ENOENT:
		return "Unknown symbol in module";
	case ESRCH:
		return "Module has wrong symbol version";
	case EINVAL:
		return "Invalid parameters";
	default:
		return strerror(err);
	}
}

extern long init_module (void *, unsigned long, const char *);

int hyper_insmod(char *module)
{
	size_t size, offset = 0, rc;
	struct stat st;
	char *buf = NULL;
	int ret;

	int fd = open(module, O_RDONLY);
	if (fd == -1) {
		fprintf (stderr, "insmod: open: %s: %m\n", module);
		return -1;
	}

	if (fstat(fd, &st) == -1) {
		perror ("insmod: fstat");
		goto err;
	}

	size = st.st_size;
	buf = malloc(size);
	if (buf == NULL)
		goto err;

	do {
		rc = read(fd, buf + offset, size - offset);
		if (rc == -1) {
			perror ("insmod: read");
			goto err;
		}
		offset += rc;
	} while (offset < size);

	if (init_module(buf, size, "") != 0) {
		fprintf (stderr, "insmod: init_module: %s: %s\n", module, moderror(errno));
		goto err;
	}

	ret = 0;
out:
	close(fd);
	free(buf);

	return ret;
err:
	ret = -1;
	goto out;
}
#else
int hyper_open_channel(char *channel, int mode)
{
	struct dirent **list;
	struct dirent *dir;
	int fd = -1, i, num;
	char path[256], name[128];
	ssize_t bytes_read;

	num = scandir("/sys/class/virtio-ports/", &list, NULL, NULL);
	if (num < 0) {
		perror("scan /sys/class/virtio-ports/ failed");
		return -1;
	}

	memset(path, 0, sizeof(path));

	for (i = 0; i < num; i++) {
		dir = list[i];
		if (dir->d_name[0] == '.')
			continue;

		if (snprintf(path, sizeof(path), "/sys/class/virtio-ports/%s/name", dir->d_name) < 0) {
			fprintf(stderr, "get channel device %s path failed\n", dir->d_name);
			continue;
		}

		fd = open(path, O_RDONLY);
		if (fd < 0)
			continue;

		memset(name, 0, sizeof(name));

		bytes_read = read(fd, name, sizeof(name));
		if (bytes_read < 0) {
			close(fd);
			continue;
		}

		close(fd);

		if (strncmp(name, channel, strlen(channel))) {
			continue;
		}

		if (snprintf(path, sizeof(path), "/dev/%s", dir->d_name) < 0) {
			fprintf(stderr, "get channel device %s path failed\n", dir->d_name);
			continue;
		}

		fprintf(stdout, "open hyper channel %s\n", path);
		fd = open(path, O_RDWR | O_CLOEXEC | mode);
		if (fd < 0)
			perror("fail to open channel device");

		break;
	}

	free(list);
	return fd;
}

int hyper_insmod(char *module)
{
	return 0;
}
#endif

int hyper_setfd_cloexec(int fd)
{
	int flags = fcntl(fd, F_GETFD);

	if (flags < 0) {
		perror("fcntl F_GETFD failed");
		return -1;
	}

	if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
		perror("fcntl F_SETFD failed");
		return -1;
	}

	return 0;
}

int hyper_setfd_block(int fd)
{
	int flags = fcntl(fd, F_GETFL);

	if (flags < 0) {
		perror("fcntl F_GETFL failed");
		return -1;
	}

	if (fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0) {
		perror("fcntl F_SETFD failed");
		return -1;
	}

	return flags;
}

int hyper_setfd_nonblock(int fd)
{
	int flags = fcntl(fd, F_GETFL);

	if (flags < 0) {
		perror("fcntl F_GETFL failed");
		return -1;
	}

	if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
		perror("fcntl F_SETFD failed");
		return -1;
	}

	return flags;
}

static void hyper_unmount_all(void)
{
	FILE *mtab;
	struct mntent *mnt;
	char *mntlist[128];
	int i, n = 0;
	char *filesys;

	mtab = setmntent("/proc/mounts", "r");
	if (mtab == NULL) {
		fprintf(stderr, "cannot open /proc/mount");
		return;
	}

	while (n < 128) {
		mnt = getmntent(mtab);
		if (mnt == NULL)
			break;

		if (strcmp(mnt->mnt_type, "devtmpfs") == 0 ||
		    strcmp(mnt->mnt_type, "proc") == 0 ||
		    strcmp(mnt->mnt_type, "sysfs") == 0 ||
		    strcmp(mnt->mnt_type, "ramfs") == 0 ||
		    strcmp(mnt->mnt_type, "tmpfs") == 0 ||
		    strcmp(mnt->mnt_type, "rootfs") == 0 ||
		    strcmp(mnt->mnt_type, "devpts") == 0)
			continue;

		mntlist[n++] = strdup(mnt->mnt_dir);
	}

	endmntent(mtab);

	for (i = n - 1; i >= 0; i--) {
		filesys = mntlist[i];
		fprintf(stdout, "umount %s\n", filesys);
		if ((umount(mntlist[i]) < 0) && (umount2(mntlist[i], MNT_DETACH) < 0)) {
			fprintf(stdout, ("umount %s: %s failed\n"),
				filesys, strerror(errno));
		}
		free(filesys);
		mntlist[i] = NULL;
	}

	sync();
}

void hyper_shutdown()
{
	hyper_unmount_all();
	reboot(LINUX_REBOOT_CMD_POWER_OFF);
}

/**
 * Reads command stdout
 *
 * \param stdout_pipe command stdout fd
 * \param[out] output command stdout
 * \param[out] size command stdout size, if is NULL then it is not used
 *
 * \return 0 on success, else -1
 */
static int hyper_read_cmd_stdout(int stdout_pipe, uint8_t **output, size_t *size)
{
	size_t total_bytes_read = 0;
	int bytes_read = 0;
	size_t bufsize = 512;
	uint8_t *buffer = NULL;

	if (stdout_pipe < 0 || output == NULL) {
		return -1;
	}

	buffer = calloc(bufsize, sizeof(*buffer));

	if (buffer == NULL) {
		/* try once again */
		buffer = calloc(bufsize, sizeof(*buffer));
	}

	if (buffer == NULL) {
		return -1;
	}

	do {
		bytes_read = read(stdout_pipe,
			buffer+total_bytes_read, bufsize-total_bytes_read);

		if (bytes_read < 0) {
			/* there is no more data */
			break;
		}

		if (bytes_read > 0) {
			total_bytes_read += bytes_read;
		}

		if (total_bytes_read > bufsize-1) {
			uint8_t *tmp = NULL;
			bufsize+=bufsize;
			tmp = realloc(buffer, bufsize * sizeof(*buffer));
			if (tmp == NULL) {
				/* try to reallocate once again */
				tmp = realloc(buffer, bufsize * sizeof(*buffer));
			}
			if (tmp == NULL) {
				/* still NULL, free and return -1 */
				free(buffer);
				return -1;
			}
			buffer = tmp;
		}
	} while (bytes_read > 0);

	/* end of string */
	buffer[total_bytes_read] = '\0';

	*output = buffer;

	if (size) {
		*size = total_bytes_read;
	}

	return 0;
}

/**
 * runs a command
 *
 * \param cmd command to run
 * \param[out] output command stdout, if is NULL then it is not used
 * \param[out] size command stdout size, if is NULL then it is not used
 *
 * \return 0 if command exited normally, else -1
 */
int hyper_cmd(char *cmd, uint8_t **output, size_t *size)
{
	int pid, status;
	int stdout_pipe[2] = { -1, -1 };

	if (output != NULL) {
		if (pipe(stdout_pipe) == -1) {
			perror("failed to open stdout pipe");
			return -1;
		}
	}

	pid = fork();
	if (pid < 0) {
		perror("fail to fork");
		return -1;
	} else if (pid > 0) {
		if (output != NULL) {
			close(stdout_pipe[1]);
			if (hyper_read_cmd_stdout(stdout_pipe[0], output, size) != 0) {
				return -1;
			}
			close(stdout_pipe[0]);
		}

		if (waitpid(pid, &status, 0) <= 0) {
			perror("waiting fork cmd failed");
			return -1;
		}
		if (WIFEXITED(status)) {
			int ret = WEXITSTATUS(status);
			fprintf(stdout, "%s cmd exit normally, status %" PRIu8 "\n", cmd, ret);
			if (ret == 0)
				return 0;
		}

		fprintf(stdout, "cmd %s exit unexpectedly, status %" PRIu8 "\n", cmd, status);
		return -1;
	} else {
		fprintf(stdout, "executing cmd %s\n", cmd);

		if (output != NULL) {
			close(stdout_pipe[0]);
			dup2(stdout_pipe[1], STDOUT_FILENO);
		}

		execlp("sh", "sh", "-c", cmd, NULL);
	}

	return -1;
}

ssize_t nonblock_read(int fd, void *buf, size_t count)
{
	ssize_t len = 0, ret = 0;

	while (len < count) {
		ret = read(fd, buf + len, count - len);
		if (ret <= 0) {
			if (errno == EINTR) {
				continue;
			}
			if (errno == EAGAIN) {
				ret = 0;
			}
			break;
		}

		len += ret;
	}

	return len > 0 ? len : ret;
}

int set_win_size(int fd, int rows, int columns)
{
	int ret;
	struct winsize size = {
		.ws_row = rows,
		.ws_col = columns,
	};

	ret = ioctl(fd, TIOCSWINSZ, &size);
	if (ret < 0) {
		perror("cannot ioctl to set pty device term size");
	}
	return ret;
}
